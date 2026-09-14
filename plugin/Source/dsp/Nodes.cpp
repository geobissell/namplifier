#include "Nodes.h"
#include "host/HostedVstNode.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace namplifier
{
namespace
{
bool validateIrFile (const juce::File& file, juce::String& errorOut)
{
  if (! file.existsAsFile())
  {
    errorOut = "IR file not found";
    return false;
  }

  const auto bytes = file.getSize();
  if (bytes < 256)
  {
    errorOut = "IR file is corrupt or too small (" + juce::String (bytes) + " bytes)";
    return false;
  }

  juce::FileInputStream stream (file);
  if (! stream.openedOk())
  {
    errorOut = "Could not open IR file";
    return false;
  }

  char header[12] {};
  if (stream.read (header, 12) < 12)
  {
    errorOut = "IR file is unreadable";
    return false;
  }

  const bool isRiff = header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F'
                      && header[8] == 'W' && header[9] == 'A' && header[10] == 'V' && header[11] == 'E';
  const bool isAiff = header[0] == 'F' && header[1] == 'O' && header[2] == 'R' && header[3] == 'M';
  if (! isRiff && ! isAiff)
  {
    errorOut = "IR is not a valid WAV/AIFF file";
    return false;
  }
  return true;
}

bool loadIrIntoConvolution (juce::dsp::Convolution& conv, const juce::File& file, juce::String& errorOut)
{
  if (! validateIrFile (file, errorOut))
    return false;

  try
  {
    conv.loadImpulseResponse (file, juce::dsp::Convolution::Stereo::no,
                              juce::dsp::Convolution::Trim::yes, 0);
  }
  catch (...)
  {
    errorOut = "IR loader failed (corrupt cab file)";
    return false;
  }
  return true;
}
} // namespace

bool loadIrFileIntoConvolution (juce::dsp::Convolution& conv, const juce::File& file, juce::String& errorOut)
{
  return loadIrIntoConvolution (conv, file, errorOut);
}

void NamRuntimeNode::prepare (double sampleRate, int maxBlockSize)
{
  mSampleRate = sampleRate;
  mMaxBlock = maxBlockSize;
  mScratch.assign ((size_t) maxBlockSize, 0.0f);
  juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 1 };
  mBass.prepare (spec);
  mMid.prepare (spec);
  mTreble.prepare (spec);
  mBass.reset();
  mMid.reset();
  mTreble.reset();
  if (mModel)
    mModel->Reset (sampleRate, maxBlockSize);
}

void NamRuntimeNode::processMono (float* buffer, int numSamples)
{
  applyStaging();
  if (bypass || mModel == nullptr)
    return;

  mInPtr[0] = buffer;
  mOutPtr[0] = mScratch.data();
  mModel->process (mInPtr, mOutPtr, numSamples);
  juce::FloatVectorOperations::copy (buffer, mScratch.data(), numSamples);

  // Match official NAM plugin "Normalized" mode — without this, raw model
  // output is often well above 0 dBFS and sounds like harsh HF clipping.
  if (mNormGain != 1.0f)
    juce::FloatVectorOperations::multiply (buffer, mNormGain, numSamples);

  if (mEqActive)
  {
    for (int i = 0; i < numSamples; ++i)
    {
      float s = buffer[i];
      s = mBass.processSample (s);
      s = mMid.processSample (s);
      s = mTreble.processSample (s);
      buffer[i] = s;
    }
  }

  if (mGain != 1.0f)
    juce::FloatVectorOperations::multiply (buffer, mGain, numSamples);
}

void NamRuntimeNode::setParams (const NodeParams& p)
{
  bypass = p.bypass;
  mGain = juce::Decibels::decibelsToGain (p.levelDb);
  mSlim = p.slim;
  mBassDb = p.bassDb;
  mMidDb = p.midDb;
  mTrebleDb = p.trebleDb;
  displayName = p.displayName;
  filePath = p.filePath;

  mEqActive = std::abs (mBassDb) > 0.05f || std::abs (mMidDb) > 0.05f || std::abs (mTrebleDb) > 0.05f;
  *mBass.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowShelf (
    mSampleRate, 120.0, 0.707f, juce::Decibels::decibelsToGain (mBassDb));
  *mMid.coefficients = *juce::dsp::IIR::Coefficients<float>::makePeakFilter (
    mSampleRate, 750.0, 0.8f, juce::Decibels::decibelsToGain (mMidDb));
  *mTreble.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighShelf (
    mSampleRate, 3500.0, 0.707f, juce::Decibels::decibelsToGain (mTrebleDb));

  if (mModel)
  {
    if (auto* slim = mModel->getSlimmableModel())
      slim->SetSlimmableSize (mSlim);
    refreshNormGain();
  }
}

void NamRuntimeNode::stageModel (std::unique_ptr<ResamplingNAM> model)
{
  mStaged = std::move (model);
  mHasStaged.store (true);
}

void NamRuntimeNode::applyStaging()
{
  if (! mHasStaged.exchange (false))
    return;
  mModel = std::move (mStaged);
  if (mModel)
  {
    // Already Reset/prewarmed on the worker — Reset here pops on the audio thread.
    if (auto* slim = mModel->getSlimmableModel())
      slim->SetSlimmableSize (mSlim);
    refreshNormGain();
  }
}

void NamRuntimeNode::refreshNormGain()
{
  mNormGain = 1.0f;
  if (mModel == nullptr || ! mModel->HasLoudness())
    return;
  // Official NAM "Normalized" targets −18 dBFS loudness.
  // Only attenuate hot models — never boost quiet ones (boosting amplifies
  // model noise into a constant HF fizz even when you play quietly).
  constexpr double targetLoudness = -18.0;
  const double loudness = mModel->GetLoudness();
  if (loudness <= targetLoudness)
    return;
  mNormGain = (float) std::pow (10.0, (targetLoudness - loudness) / 20.0);
  mNormGain = juce::jlimit (0.004f, 1.0f, mNormGain);
}

void IrRuntimeNode::prepare (double sampleRate, int maxBlockSize)
{
  mSampleRate = sampleRate;
  mMaxBlock = maxBlockSize;
  mDry.setSize (1, maxBlockSize);
  // Fresh empty engine — IR data is loaded off-thread via loadIrAsync / stageReadyConvolution.
  mConv = std::make_unique<juce::dsp::Convolution>();
  juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 1 };
  mConv->prepare (spec);
  mConv->reset();
  mLoadedPath.clear();
}

void IrRuntimeNode::processMono (float* buffer, int numSamples)
{
  applyStaging();
  if (bypass || mConv == nullptr || mLoadedPath.isEmpty())
    return;

  mDry.copyFrom (0, 0, buffer, numSamples);
  juce::dsp::AudioBlock<float> block (mDry);
  auto sub = block.getSubBlock (0, (size_t) numSamples);
  juce::dsp::ProcessContextReplacing<float> ctx (sub);
  mConv->process (ctx);
  const float dryGain = 1.0f - mMix;
  const float wetGain = mMix * mGain;
  for (int i = 0; i < numSamples; ++i)
    buffer[i] = buffer[i] * dryGain + mDry.getSample (0, i) * wetGain;
}

void IrRuntimeNode::setParams (const NodeParams& p)
{
  bypass = p.bypass;
  mMix = juce::jlimit (0.0f, 1.0f, p.mix);
  mGain = juce::Decibels::decibelsToGain (p.levelDb);
  displayName = p.displayName;
  // File loads go through GraphEngine::loadIrAsync — never sync-load here (deadlocks / audio stalls).
  filePath = p.filePath;
}

void IrRuntimeNode::stageIr (const juce::File&)
{
  // Legacy entry — GraphEngine now stages a fully prepared Convolution instead.
}

void IrRuntimeNode::stageReadyConvolution (std::unique_ptr<juce::dsp::Convolution> conv, const juce::String& path)
{
  mStagedConv = std::move (conv);
  mStagedPath = path;
  mHasStaged.store (true);
}

void IrRuntimeNode::applyStaging()
{
  if (! mHasStaged.exchange (false))
    return;

  if (mStagedConv != nullptr)
  {
    mConv = std::move (mStagedConv);
    mLoadedPath = mStagedPath;
    filePath = mLoadedPath;
    lastError.clear();
  }
  else
  {
    mLoadedPath.clear();
  }
  mStagedPath.clear();
}

bool loadAudioFileResampled (const juce::File& file, double targetSampleRate,
                             juce::AudioBuffer<float>& outStereo, juce::String& errorOut)
{
  errorOut.clear();
  if (! file.existsAsFile())
  {
    errorOut = "Audio file not found";
    return false;
  }

  juce::AudioFormatManager formats;
  formats.registerBasicFormats();
#if JUCE_USE_FLAC
  formats.registerFormat (new juce::FlacAudioFormat(), false);
#endif
#if JUCE_USE_OGGVORBIS
  formats.registerFormat (new juce::OggVorbisAudioFormat(), false);
#endif

  std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
  if (reader == nullptr)
  {
    errorOut = "Unsupported or unreadable audio file";
    return false;
  }

  const auto srcSamples = (int) reader->lengthInSamples;
  const int srcCh = (int) juce::jmax (1, (int) reader->numChannels);
  if (srcSamples <= 0)
  {
    errorOut = "Audio file is empty";
    return false;
  }

  juce::AudioBuffer<float> src (srcCh, srcSamples);
  if (! reader->read (&src, 0, srcSamples, 0, true, true))
  {
    errorOut = "Failed to decode audio file";
    return false;
  }

  const double srcRate = reader->sampleRate > 0.0 ? reader->sampleRate : targetSampleRate;
  const int dstSamples = juce::jmax (1, (int) std::llround ((double) srcSamples * (targetSampleRate / srcRate)));
  outStereo.setSize (2, dstSamples, false, true, false);

  auto sampleAt = [&] (int ch, double pos) -> float
  {
    if (srcSamples <= 1)
      return src.getSample (juce::jmin (ch, srcCh - 1), 0);
    const int i0 = juce::jlimit (0, srcSamples - 1, (int) pos);
    const int i1 = juce::jmin (srcSamples - 1, i0 + 1);
    const float frac = (float) (pos - (double) i0);
    const int useCh = juce::jmin (ch, srcCh - 1);
    return src.getSample (useCh, i0) * (1.0f - frac) + src.getSample (useCh, i1) * frac;
  };

  const double step = srcRate / targetSampleRate;
  for (int i = 0; i < dstSamples; ++i)
  {
    const double pos = (double) i * step;
    const float L = sampleAt (0, pos);
    const float R = srcCh > 1 ? sampleAt (1, pos) : L;
    outStereo.setSample (0, i, L);
    outStereo.setSample (1, i, R);
  }
  return true;
}

void MediaPlayerRuntimeNode::prepare (double sampleRate, int)
{
  mSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
}

void MediaPlayerRuntimeNode::clearAudio()
{
  mReady.store (false, std::memory_order_release);
  mAudio.setSize (0, 0);
  mPositionSamples.store (0.0, std::memory_order_relaxed);
  mPlaying.store (false, std::memory_order_relaxed);
}

void MediaPlayerRuntimeNode::stageAudio (juce::AudioBuffer<float> buffer, juce::String path)
{
  mStagedAudio = std::move (buffer);
  mStagedPath = std::move (path);
  mHasStaging.store (true, std::memory_order_release);
}

void MediaPlayerRuntimeNode::applyStaging()
{
  if (! mHasStaging.exchange (false, std::memory_order_acq_rel))
    return;

  mAudio = std::move (mStagedAudio);
  filePath = mStagedPath;
  mStagedPath.clear();
  mPositionSamples.store (0.0, std::memory_order_relaxed);
  mReady.store (mAudio.getNumSamples() > 0, std::memory_order_release);
  lastError.clear();
}

void MediaPlayerRuntimeNode::setParams (const NodeParams& p)
{
  bypass = p.bypass;
  mGain = juce::Decibels::decibelsToGain (p.levelDb);
  mLoop.store (p.mediaLoop, std::memory_order_relaxed);
  displayName = p.displayName;
  mediaUrl = p.mediaUrl;
  if (p.filePath.isNotEmpty())
    filePath = p.filePath;

  if (p.mediaSeekSec >= 0.0f && mSampleRate > 0.0)
  {
    const double samples = (double) p.mediaSeekSec * mSampleRate;
    mSeekSamples.store (samples, std::memory_order_relaxed);
  }

  const bool wantPlay = p.mediaPlaying && ! p.bypass;
  if (wantPlay)
  {
    const int total = mAudio.getNumSamples();
    const double pos = mPositionSamples.load (std::memory_order_relaxed);
    // Fresh Play after natural end: rewind so level tweaks don't re-trigger mid-file.
    if (! mPlaying.load (std::memory_order_relaxed) && total > 0
        && pos >= (double) juce::jmax (0, total - 2) && ! p.mediaLoop
        && p.mediaSeekSec < 0.0f)
      mPositionSamples.store (0.0, std::memory_order_relaxed);
    mPlaying.store (true, std::memory_order_relaxed);
  }
  else
  {
    mPlaying.store (false, std::memory_order_relaxed);
  }
}

double MediaPlayerRuntimeNode::positionSec() const
{
  if (mSampleRate <= 0.0)
    return 0.0;
  return mPositionSamples.load (std::memory_order_relaxed) / mSampleRate;
}

double MediaPlayerRuntimeNode::durationSec() const
{
  if (! mReady.load (std::memory_order_acquire) || mSampleRate <= 0.0)
    return 0.0;
  return (double) mAudio.getNumSamples() / mSampleRate;
}

void MediaPlayerRuntimeNode::processMono (float* buffer, int numSamples)
{
  process (buffer, buffer, numSamples);
}

void MediaPlayerRuntimeNode::process (float* left, float* right, int numSamples)
{
  applyStaging();

  if (left == nullptr)
    return;

  juce::FloatVectorOperations::clear (left, numSamples);
  if (right != nullptr && right != left)
    juce::FloatVectorOperations::clear (right, numSamples);

  if (bypass || ! mReady.load (std::memory_order_acquire) || mAudio.getNumSamples() <= 0)
    return;

  const double seek = mSeekSamples.exchange (-1.0, std::memory_order_acq_rel);
  if (seek >= 0.0)
    mPositionSamples.store (juce::jlimit (0.0, (double) juce::jmax (0, mAudio.getNumSamples() - 1), seek),
                            std::memory_order_relaxed);

  if (! mPlaying.load (std::memory_order_relaxed))
  {
    // Still output a single frame at the parked position so meters aren't dead-silent while paused? No — silence when paused.
    return;
  }

  const int total = mAudio.getNumSamples();
  const float* srcL = mAudio.getReadPointer (0);
  const float* srcR = mAudio.getNumChannels() > 1 ? mAudio.getReadPointer (1) : srcL;
  const bool loop = mLoop.load (std::memory_order_relaxed);
  double pos = mPositionSamples.load (std::memory_order_relaxed);
  const float g = mGain;

  for (int i = 0; i < numSamples; ++i)
  {
    if (pos >= (double) total)
    {
      if (loop && total > 0)
        pos = std::fmod (pos, (double) total);
      else
      {
        mPlaying.store (false, std::memory_order_relaxed);
        mPositionSamples.store ((double) juce::jmax (0, total - 1), std::memory_order_relaxed);
        break;
      }
    }

    const int i0 = juce::jlimit (0, total - 1, (int) pos);
    left[i] = srcL[i0] * g;
    if (right != nullptr && right != left)
      right[i] = srcR[i0] * g;
    pos += 1.0;
  }

  mPositionSamples.store (pos, std::memory_order_relaxed);
}

std::unique_ptr<RuntimeNode> createRuntimeNode (const GraphNode& desc)
{
  std::unique_ptr<RuntimeNode> node;
  switch (desc.type)
  {
    case NodeType::Nam:
      node = std::make_unique<NamRuntimeNode>();
      break;
    case NodeType::Ir:
      node = std::make_unique<IrRuntimeNode>();
      break;
    case NodeType::Split:
      node = std::make_unique<SplitRuntimeNode>();
      break;
    case NodeType::Merge:
      node = std::make_unique<MergeRuntimeNode>();
      break;
    case NodeType::MediaFile:
    case NodeType::YouTube:
      node = std::make_unique<MediaPlayerRuntimeNode> (desc.type);
      break;
    case NodeType::Vst:
      node = std::make_unique<VstRuntimeNode>();
      break;
    case NodeType::Fx:
      if (desc.params.fxId.equalsIgnoreCase ("delay")
          || desc.params.fxId.equalsIgnoreCase ("pingpong")
          || desc.params.displayName.containsIgnoreCase ("delay")
          || desc.params.displayName.containsIgnoreCase ("ping"))
        node = std::make_unique<DelayRuntimeNode>();
      else if (desc.params.fxId.equalsIgnoreCase ("gate")
               || desc.params.displayName.containsIgnoreCase ("gate")
               || desc.params.displayName.containsIgnoreCase ("noise"))
        node = std::make_unique<NoiseGateRuntimeNode>();
      else if (desc.params.fxId.equalsIgnoreCase ("chorus")
               || desc.params.displayName.containsIgnoreCase ("chorus"))
        node = std::make_unique<ChorusRuntimeNode>();
      else if (desc.params.fxId.equalsIgnoreCase ("compressor")
               || desc.params.fxId.equalsIgnoreCase ("comp")
               || desc.params.displayName.containsIgnoreCase ("compress"))
        node = std::make_unique<CompressorRuntimeNode>();
      else if (desc.params.fxId.equalsIgnoreCase ("reverb")
               || desc.params.displayName.containsIgnoreCase ("reverb"))
        node = std::make_unique<ReverbRuntimeNode>();
      else
        node = std::make_unique<PassThroughNode> (NodeType::Fx);
      break;
    case NodeType::Input:
    case NodeType::Output:
    case NodeType::Bypass:
    default:
      node = std::make_unique<PassThroughNode> (desc.type);
      break;
  }
  node->id = desc.id;
  node->setParams (desc.params);
  if (auto* nam = dynamic_cast<NamRuntimeNode*> (node.get()))
  {
    nam->filePath = desc.params.filePath;
    nam->displayName = desc.params.displayName;
  }
  if (auto* ir = dynamic_cast<IrRuntimeNode*> (node.get()))
  {
    ir->filePath = desc.params.filePath;
    ir->displayName = desc.params.displayName;
  }
  if (auto* media = dynamic_cast<MediaPlayerRuntimeNode*> (node.get()))
  {
    media->filePath = desc.params.filePath;
    media->displayName = desc.params.displayName;
    media->mediaUrl = desc.params.mediaUrl;
  }
  if (auto* vst = dynamic_cast<VstRuntimeNode*> (node.get()))
  {
    vst->filePath = desc.params.filePath;
    vst->pluginUid = desc.params.modelId;
    vst->displayName = desc.params.displayName;
  }
  return node;
}

} // namespace namplifier
