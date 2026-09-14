#include "Nodes.h"

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

  if (numSamples > mMaxBlock)
    prepare (mSampleRate, numSamples);

  // High-gain NAMs turn digital silence into a constant hiss/whistle. If the host
  // isn't feeding us (bus negotiation / monitoring), stay silent instead.
  float inPeak = 0.0f;
  for (int i = 0; i < numSamples; ++i)
    inPeak = juce::jmax (inPeak, std::abs (buffer[i]));
  if (inPeak < 1.0e-5f)
  {
    juce::FloatVectorOperations::clear (buffer, numSamples);
    return;
  }

  mInPtr[0] = buffer;
  mOutPtr[0] = mScratch.data();
  try
  {
    mModel->process (mInPtr, mOutPtr, numSamples);
  }
  catch (...)
  {
    return;
  }
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
  return node;
}

} // namespace namplifier
