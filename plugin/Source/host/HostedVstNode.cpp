#include "HostedVstNode.h"

namespace namplifier
{

void vstChannelCountsFromPlugin (juce::AudioPluginInstance& plugin, int& insOut, int& outsOut)
{
  auto layout = plugin.getBusesLayout();
  const int inMain = layout.getMainInputChannelSet().isDisabled()
                       ? 0
                       : layout.getMainInputChannelSet().size();
  const int outMain = layout.getMainOutputChannelSet().isDisabled()
                        ? 0
                        : layout.getMainOutputChannelSet().size();

  // Instruments / generators: treat as sources with outs only — still expose 1 silent in for wiring.
  insOut = juce::jlimit (1, 2, juce::jmax (1, inMain));
  outsOut = juce::jlimit (1, 2, juce::jmax (1, outMain));

  // Prefer stereo when the plugin reports 0/0 (unknown).
  if (inMain <= 0 && outMain <= 0)
  {
    insOut = 2;
    outsOut = 2;
  }
}

void VstRuntimeNode::preparePluginLocked (juce::AudioPluginInstance& plugin)
{
  juce::AudioProcessor::BusesLayout layout = plugin.getBusesLayout();
  auto inSet = layout.getMainInputChannelSet();
  auto outSet = layout.getMainOutputChannelSet();

  if (inSet.isDisabled() || inSet.size() < 1)
    inSet = juce::AudioChannelSet::stereo();
  if (outSet.isDisabled() || outSet.size() < 1)
    outSet = juce::AudioChannelSet::stereo();

  // Prefer matching mono/stereo layouts the plugin accepts.
  juce::AudioProcessor::BusesLayout wanted;
  wanted.inputBuses.add (inSet.size() >= 2 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono());
  wanted.outputBuses.add (outSet.size() >= 2 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono());

  if (! plugin.setBusesLayout (wanted))
  {
    wanted.inputBuses.clear();
    wanted.outputBuses.clear();
    wanted.inputBuses.add (juce::AudioChannelSet::stereo());
    wanted.outputBuses.add (juce::AudioChannelSet::stereo());
    plugin.setBusesLayout (wanted);
  }

  plugin.setPlayConfigDetails (plugin.getTotalNumInputChannels(),
                               plugin.getTotalNumOutputChannels(),
                               mSampleRate,
                               mMaxBlock);
  plugin.prepareToPlay (mSampleRate, mMaxBlock);
}

void VstRuntimeNode::prepare (double sampleRate, int maxBlockSize)
{
  mSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
  mMaxBlock = juce::jmax (1, maxBlockSize);
  mScratch.setSize (4, mMaxBlock, false, true, false);
  if (mPlugin != nullptr)
    preparePluginLocked (*mPlugin);
}

void VstRuntimeNode::stageInstance (std::unique_ptr<juce::AudioPluginInstance> plugin, int ins, int outs)
{
  if (plugin != nullptr)
    preparePluginLocked (*plugin);
  mStaged = std::move (plugin);
  mStagedIns = juce::jlimit (1, 2, ins);
  mStagedOuts = juce::jlimit (1, 2, outs);
  mHasStaging.store (true, std::memory_order_release);
}

void VstRuntimeNode::applyStaging()
{
  if (! mHasStaging.exchange (false, std::memory_order_acq_rel))
    return;

  if (mPlugin != nullptr)
    mPlugin->releaseResources();

  mPlugin = std::move (mStaged);
  mIns = mStagedIns;
  mOuts = mStagedOuts;
  mReady.store (mPlugin != nullptr, std::memory_order_release);
  if (mPlugin != nullptr)
    lastError.clear();
}

void VstRuntimeNode::setParams (const NodeParams& p)
{
  bypass = p.bypass;
  mGain = juce::Decibels::decibelsToGain (p.levelDb);
  displayName = p.displayName;
  filePath = p.filePath;
  pluginUid = p.modelId;
  if (p.vstIns > 0)
    mIns = juce::jlimit (1, 2, p.vstIns);
  if (p.vstOuts > 0)
    mOuts = juce::jlimit (1, 2, p.vstOuts);
}

juce::String VstRuntimeNode::captureStateBase64() const
{
  if (mPlugin == nullptr)
    return {};
  juce::MemoryBlock mb;
  mPlugin->getStateInformation (mb);
  if (mb.isEmpty())
    return {};
  return juce::Base64::toBase64 (mb.getData(), mb.getSize());
}

void VstRuntimeNode::restoreStateBase64 (const juce::String& b64)
{
  if (mPlugin == nullptr || b64.isEmpty())
    return;
  juce::MemoryOutputStream mos;
  if (! juce::Base64::convertFromBase64 (mos, b64))
    return;
  mPlugin->setStateInformation (mos.getData(), (int) mos.getDataSize());
}

void VstRuntimeNode::processMono (float* buffer, int numSamples)
{
  process (buffer, buffer, numSamples);
}

void VstRuntimeNode::process (float* left, float* right, int numSamples)
{
  applyStaging();

  if (left == nullptr)
    return;

  if (bypass || mPlugin == nullptr || ! mReady.load (std::memory_order_acquire))
    return;

  const int n = juce::jmax (0, numSamples);
  if (n > mScratch.getNumSamples())
    mScratch.setSize (4, n, false, false, true);

  const int pluginIns = juce::jmax (1, mPlugin->getTotalNumInputChannels());
  const int pluginOuts = juce::jmax (1, mPlugin->getTotalNumOutputChannels());
  const int bufCh = juce::jmax (pluginIns, pluginOuts);

  mScratch.clear();
  // Feed graph L/R into plugin channels.
  if (pluginIns >= 1)
    juce::FloatVectorOperations::copy (mScratch.getWritePointer (0), left, n);
  if (pluginIns >= 2)
  {
    const float* srcR = (right != nullptr && right != left) ? right : left;
    juce::FloatVectorOperations::copy (mScratch.getWritePointer (1), srcR, n);
  }

  juce::AudioBuffer<float> view (mScratch.getArrayOfWritePointers(), bufCh, n);
  mMidi.clear();
  {
    const juce::ScopedLock lock (mPlugin->getCallbackLock());
    if (! mPlugin->isSuspended())
      mPlugin->processBlock (view, mMidi);
  }

  // Read back
  if (pluginOuts >= 1)
    juce::FloatVectorOperations::copy (left, mScratch.getReadPointer (0), n);
  if (right != nullptr && right != left)
  {
    if (pluginOuts >= 2)
      juce::FloatVectorOperations::copy (right, mScratch.getReadPointer (1), n);
    else
      juce::FloatVectorOperations::copy (right, left, n);
  }

  if (mGain != 1.0f)
  {
    juce::FloatVectorOperations::multiply (left, mGain, n);
    if (right != nullptr && right != left)
      juce::FloatVectorOperations::multiply (right, mGain, n);
  }
}

} // namespace namplifier
