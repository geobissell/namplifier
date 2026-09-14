#pragma once

#include <mutex>
#include <thread>
#include <queue>
#include <functional>

#include "GraphModel.h"
#include "Nodes.h"
namespace namplifier
{

class GraphEngine
{
public:
  GraphEngine();
  ~GraphEngine();

  void prepare (double sampleRate, int maxBlockSize);
  void reset();
  void process (juce::AudioBuffer<float>& buffer);

  void setGraph (const GraphDocument& doc);
  GraphDocument getGraph() const;

  void updateNodeParams (const juce::String& nodeId, const NodeParams& params);
  void loadFileOntoNode (const juce::String& nodeId, const juce::File& file);

  float getCpuLoad() const { return mCpuLoad.load(); }
  float getInputPeak() const { return mInputPeak.load(); }
  float getOutputPeak() const { return mOutputPeak.load(); }
  float getInputPeakL() const { return mInputPeakL.load(); }
  float getInputPeakR() const { return mInputPeakR.load(); }
  float getOutputPeakL() const { return mOutputPeakL.load(); }
  float getOutputPeakR() const { return mOutputPeakR.load(); }
  int getActiveInputChannel() const;
  int getActiveOutputChannel() const { return mOutputChannel; }
  void setHostChannelCounts (int numIns, int numOuts);
  /** VST/AU: ignore device channel picks — host owns I/O. Standalone: false. */
  void setPluginHosted (bool pluginHosted);
  bool isPluginHosted() const { return mPluginHosted; }
  int getNumInputChannels() const { return mHostIns; }
  int getNumOutputChannels() const { return mHostOuts; }
  juce::var getDspStatus() const;

  /** Global master trims (dB). Input scales host → graph; output scales after Output sum. */
  void setMasterInDb (float db);
  void setMasterOutDb (float db);
  float getMasterInDb() const { return mMasterInDb.load(); }
  float getMasterOutDb() const { return mMasterOutDb.load(); }
  /** @deprecated alias for getMasterOutDb */
  float getMasterDb() const { return getMasterOutDb(); }
  void setMasterDb (float db) { setMasterOutDb (db); }
  bool wasClipping() const { return mWasClipping.load(); }

  std::function<void()> onAsyncLoadFinished;

private:
  struct CompiledStep
  {
    juce::String nodeId;
    int mode = 0; // 0 process in-place, 1 split copy to branch, 2 merge
    juce::String auxBufferKey;
  };

  void rebuildLocked();
  void loadNamAsync (NamRuntimeNode* node, juce::File file);
  void loadIrAsync (IrRuntimeNode* node, juce::File file);
  std::vector<juce::String> topologicalOrder (const GraphDocument& doc) const;

  mutable std::mutex mGraphMutex;
  GraphDocument mDocument;
  std::unordered_map<juce::String, std::unique_ptr<RuntimeNode>> mNodes;
  std::vector<juce::String> mOrder;
  std::unordered_map<juce::String, std::vector<GraphEdge>> mOutgoing;
  std::unordered_map<juce::String, std::vector<GraphEdge>> mIncoming;

  double mSampleRate = 44100.0;
  int mMaxBlock = 512;
  juce::AudioBuffer<float> mMono;
  juce::AudioBuffer<float> mBranchA;
  juce::AudioBuffer<float> mBranchB;
  std::atomic<float> mCpuLoad { 0.0f };
  std::atomic<float> mInputPeak { 0.0f };
  std::atomic<float> mOutputPeak { 0.0f };
  std::atomic<float> mInputPeakL { 0.0f };
  std::atomic<float> mInputPeakR { 0.0f };
  std::atomic<float> mOutputPeakL { 0.0f };
  std::atomic<float> mOutputPeakR { 0.0f };
  int mHostIns = 2;
  int mHostOuts = 2;
  bool mPluginHosted = false;
  int mInputChannel = 0;
  int mOutputChannel = 0;
  float mOutputPan = 0.0f;
  float mOutputGain = 1.0f;
  std::atomic<float> mMasterInDb { 0.0f };
  std::atomic<float> mMasterOutDb { -6.0f }; // headroom — hot NAMs / interfaces often clip at 0
  std::atomic<bool> mWasClipping { false };

  std::mutex mJobMutex;
  std::queue<std::function<void()>> mJobs;
  std::thread mWorker;
  std::atomic<bool> mRunning { true };
};

} // namespace namplifier
