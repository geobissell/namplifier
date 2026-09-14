#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>

#include "dsp/GraphModel.h"
#include "dsp/Nodes.h"

namespace namplifier
{

class VstRuntimeNode : public RuntimeNode
{
public:
  void prepare (double sampleRate, int maxBlockSize) override;
  void process (float* left, float* right, int numSamples) override;
  void processMono (float* buffer, int numSamples) override;
  void setParams (const NodeParams& p) override;
  NodeType type() const override { return NodeType::Vst; }
  bool isStereoProcessor() const override { return mOuts >= 2; }

  void stageInstance (std::unique_ptr<juce::AudioPluginInstance> plugin, int ins, int outs);
  void applyStaging();
  bool hasPlugin() const { return mReady.load (std::memory_order_acquire) && mPlugin != nullptr; }

  juce::AudioPluginInstance* getPlugin() const { return mPlugin.get(); }
  juce::String captureStateBase64() const;
  void restoreStateBase64 (const juce::String& b64);

  int inputChannels() const { return mIns; }
  int outputChannels() const { return mOuts; }

  juce::String filePath;
  juce::String pluginUid;
  juce::String displayName;
  juce::String lastError;

private:
  void preparePluginLocked (juce::AudioPluginInstance& plugin);

  double mSampleRate = 44100.0;
  int mMaxBlock = 512;
  float mGain = 1.0f;
  int mIns = 2;
  int mOuts = 2;

  std::unique_ptr<juce::AudioPluginInstance> mPlugin;
  std::unique_ptr<juce::AudioPluginInstance> mStaged;
  int mStagedIns = 2;
  int mStagedOuts = 2;
  std::atomic<bool> mHasStaging { false };
  std::atomic<bool> mReady { false };

  juce::AudioBuffer<float> mScratch;
  juce::MidiBuffer mMidi;
};

/** Infer graph port counts (clamped to 1–2) from a loaded plugin. */
void vstChannelCountsFromPlugin (juce::AudioPluginInstance& plugin, int& insOut, int& outsOut);

} // namespace namplifier
