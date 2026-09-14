#include "GraphEngine.h"
#include "CabDetect.h"
#include <chrono>
#include <cmath>
#include <set>
#include <juce_events/juce_events.h>

namespace namplifier
{

GraphEngine::GraphEngine()
{
  mDocument = GraphDocument::makeAmpCabStarter();
  rebuildLocked(); // Must build runtime nodes — UI getGraph alone never calls setGraph.
  mWorker = std::thread ([this]
  {
    while (mRunning.load())
    {
      std::function<void()> job;
      {
        std::unique_lock lock (mJobMutex);
        if (mJobs.empty())
        {
          lock.unlock();
          std::this_thread::sleep_for (std::chrono::milliseconds (5));
          continue;
        }
        job = std::move (mJobs.front());
        mJobs.pop();
      }
      if (job)
        job();
    }
  });
}

GraphEngine::~GraphEngine()
{
  mRunning = false;
  if (mWorker.joinable())
    mWorker.join();
}

void GraphEngine::prepare (double sampleRate, int maxBlockSize)
{
  std::vector<std::pair<IrRuntimeNode*, juce::File>> irReloads;
  {
    std::lock_guard lock (mGraphMutex);
    mSampleRate = sampleRate;
    mMaxBlock = maxBlockSize;
    mMono.setSize (1, maxBlockSize);
    mBranchA.setSize (1, maxBlockSize);
    mBranchB.setSize (1, maxBlockSize);
    // Host may call prepare before any setGraph (common in VST). Ensure runtime exists.
    if (mNodes.empty() && ! mDocument.nodes.empty())
      rebuildLocked();
    for (auto& [id, node] : mNodes)
    {
      juce::String irPath;
      if (auto* ir = dynamic_cast<IrRuntimeNode*> (node.get()))
        irPath = ir->filePath;
      node->prepare (mSampleRate, mMaxBlock);
      if (irPath.isNotEmpty())
        if (auto* ir = dynamic_cast<IrRuntimeNode*> (node.get()))
          irReloads.push_back ({ ir, juce::File (irPath) });
    }
  }
  for (auto& [ir, file] : irReloads)
    loadIrAsync (ir, file);
}

void GraphEngine::reset()
{
  std::lock_guard lock (mGraphMutex);
  for (auto& [id, node] : mNodes)
    node->prepare (mSampleRate, mMaxBlock);
}

void GraphEngine::setGraph (const GraphDocument& doc)
{
  std::lock_guard lock (mGraphMutex);
  mDocument = doc;
  rebuildLocked();
}

GraphDocument GraphEngine::getGraph() const
{
  std::lock_guard lock (mGraphMutex);
  return mDocument;
}

void GraphEngine::setHostChannelCounts (int numIns, int numOuts)
{
  mHostIns = juce::jmax (1, numIns);
  mHostOuts = juce::jmax (1, numOuts);
}

void GraphEngine::setDawHosted (bool dawHosted)
{
  mDawHosted = dawHosted;
}

int GraphEngine::getLatencySamples() const
{
  int latency = 0;
  std::lock_guard lock (mGraphMutex);
  for (auto& [id, node] : mNodes)
  {
    juce::ignoreUnused (id);
    if (auto* nam = dynamic_cast<NamRuntimeNode*> (node.get()))
      latency = juce::jmax (latency, nam->getLatency());
  }
  return latency;
}

int GraphEngine::getActiveInputChannel() const
{
  return mInputChannel;
}

void GraphEngine::updateNodeParams (const juce::String& nodeId, const NodeParams& params)
{
  std::lock_guard lock (mGraphMutex);
  for (auto& n : mDocument.nodes)
  {
    if (n.id == nodeId)
    {
      n.params = params;
      if (n.type == NodeType::Input)
        mInputChannel = juce::jmax (0, params.inputChannel);
      if (n.type == NodeType::Output)
      {
        mOutputPan = juce::jlimit (-1.0f, 1.0f, params.pan);
        mOutputGain = juce::Decibels::decibelsToGain (params.levelDb);
        mOutputChannel = juce::jmax (0, params.outputChannel);
      }
      break;
    }
  }
  if (auto it = mNodes.find (nodeId); it != mNodes.end())
    it->second->setParams (params);
}

void GraphEngine::loadFileOntoNode (const juce::String& nodeId, const juce::File& file)
{
  RuntimeNode* raw = nullptr;
  NodeType type = NodeType::Bypass;
  {
    std::lock_guard lock (mGraphMutex);
    // UI can show mDocument while runtime was never built (getGraph without setGraph).
    if (mNodes.empty() && ! mDocument.nodes.empty())
      rebuildLocked();

    auto it = mNodes.find (nodeId);
    if (it == mNodes.end())
      return;
    raw = it->second.get();
    type = raw->type();

    if (type == NodeType::Nam && ! file.hasFileExtension (".nam"))
    {
      if (auto* nam = dynamic_cast<NamRuntimeNode*> (raw))
        nam->lastError = "NAM nodes only accept .nam amp models";
      return;
    }
    if (type == NodeType::Ir
        && ! (file.hasFileExtension (".wav") || file.hasFileExtension (".aif")
              || file.hasFileExtension (".aiff")))
    {
      if (auto* ir = dynamic_cast<IrRuntimeNode*> (raw))
        ir->lastError = "IR nodes only accept .wav / .aiff cab files — not amp models";
      return;
    }

    for (auto& n : mDocument.nodes)
    {
      if (n.id == nodeId)
      {
        n.params.filePath = file.getFullPathName();
        // Keep library/Tone3000 title + artwork; filename is often just a numeric model id.
        if (n.params.displayName.isEmpty())
          n.params.displayName = file.getFileNameWithoutExtension();
        break;
      }
    }
  }

  if (type == NodeType::Nam)
    loadNamAsync (dynamic_cast<NamRuntimeNode*> (raw), file);
  else if (type == NodeType::Ir)
    loadIrAsync (dynamic_cast<IrRuntimeNode*> (raw), file);
}

void GraphEngine::loadNamAsync (NamRuntimeNode* node, juce::File file)
{
  if (node == nullptr)
    return;

  const juce::String nodeId = node->id;
  node->lastError.clear();
  const double sr = mSampleRate > 0.0 ? mSampleRate : 48000.0;
  const int mb = mMaxBlock > 0 ? mMaxBlock : 512;

  std::lock_guard lock (mJobMutex);
  mJobs.push ([this, nodeId, file, sr, mb]
  {
    juce::String error;
    std::unique_ptr<ResamplingNAM> wrapped;
    try
    {
      if (! file.existsAsFile())
      {
        error = "File not found: " + file.getFullPathName();
      }
      else
      {
        nam::ScopedPrewarmOnResetDefault scoped (false);
        const auto utf8 = file.getFullPathName().toStdString();
        auto path = std::filesystem::u8path (utf8);
        auto model = nam::get_dsp (path);
        if (model == nullptr)
        {
          error = "NAM loader returned null";
        }
        else
        {
          const int nIn = model->NumInputChannels();
          const int nOut = model->NumOutputChannels();
          // Most NAM profiles are mono; accept unset (0) as mono too.
          if ((nIn != 1 && nIn != 0) || (nOut != 1 && nOut != 0))
          {
            error = "Unsupported NAM channel layout (" + juce::String (nIn) + " in / "
                    + juce::String (nOut) + " out)";
          }
          else
          {
            wrapped = std::make_unique<ResamplingNAM> (std::move (model), sr);
            wrapped->SetPrewarmOnReset (true);
            wrapped->Reset (sr, mb);
          }
        }
      }
    }
    catch (const std::exception& e)
    {
      error = e.what();
    }
    catch (...)
    {
      error = "Failed to load NAM (unknown error)";
    }

    {
      std::lock_guard graphLock (mGraphMutex);
      auto it = mNodes.find (nodeId);
      if (it == mNodes.end())
        return;
      auto* nam = dynamic_cast<NamRuntimeNode*> (it->second.get());
      if (nam == nullptr)
        return;
      nam->lastError = error;
      if (wrapped)
      {
        nam->stageModel (std::move (wrapped));
        nam->filePath = file.getFullPathName();
        for (auto& n : mDocument.nodes)
          if (n.id == nodeId)
          {
            n.params.filePath = nam->filePath;
            if (n.params.displayName.isEmpty())
              n.params.displayName = file.getFileNameWithoutExtension();
            nam->displayName = n.params.displayName;
            n.params.cabIncluded = detectCabIncludedFromNamFile (file) || n.params.cabIncluded;
            break;
          }
      }
    }

    if (onAsyncLoadFinished)
      juce::MessageManager::callAsync ([cb = onAsyncLoadFinished] { if (cb) cb(); });
  });
}

void GraphEngine::loadIrAsync (IrRuntimeNode* node, juce::File file)
{
  if (node == nullptr)
    return;

  const juce::String nodeId = node->id;
  node->lastError.clear();
  const double sr = mSampleRate > 0.0 ? mSampleRate : 48000.0;
  const int mb = mMaxBlock > 0 ? mMaxBlock : 512;

  // Same pattern as loadNamAsync: only take mJobMutex on the caller so this is safe
  // while rebuildLocked holds mGraphMutex (library IR drag used to deadlock here).
  std::lock_guard lock (mJobMutex);
  mJobs.push ([this, nodeId, file, sr, mb]
  {
    juce::String error;
    std::unique_ptr<juce::dsp::Convolution> conv;

    if (! file.existsAsFile())
    {
      error = "File not found: " + file.getFullPathName();
    }
    else if (file.getSize() < 256)
    {
      error = "IR file is corrupt or too small (" + juce::String (file.getSize())
              + " bytes) — re-download the cab";
    }
    else
    {
      try
      {
        conv = std::make_unique<juce::dsp::Convolution>();
        juce::dsp::ProcessSpec spec { sr, (juce::uint32) mb, 1 };
        conv->prepare (spec);
        juce::String loadErr;
        if (! loadIrFileIntoConvolution (*conv, file, loadErr))
        {
          error = loadErr;
          conv.reset();
        }
      }
      catch (const std::exception& e)
      {
        error = e.what();
        conv.reset();
      }
      catch (...)
      {
        error = "Failed to load IR";
        conv.reset();
      }
    }

    {
      std::lock_guard graphLock (mGraphMutex);
      auto it = mNodes.find (nodeId);
      if (it == mNodes.end())
        return;
      auto* ir = dynamic_cast<IrRuntimeNode*> (it->second.get());
      if (ir == nullptr)
        return;

      ir->lastError = error;
      if (conv)
      {
        const auto path = file.getFullPathName();
        ir->stageReadyConvolution (std::move (conv), path);
        ir->filePath = path;
        for (auto& n : mDocument.nodes)
          if (n.id == nodeId)
          {
            n.params.filePath = path;
            if (n.params.displayName.isEmpty())
              n.params.displayName = file.getFileNameWithoutExtension();
            ir->displayName = n.params.displayName.isNotEmpty()
                                ? n.params.displayName
                                : file.getFileNameWithoutExtension();
            break;
          }
      }
    }

    if (onAsyncLoadFinished)
      juce::MessageManager::callAsync ([cb = onAsyncLoadFinished] { if (cb) cb(); });
  });
}

juce::var GraphEngine::getDspStatus() const
{
  std::lock_guard lock (mGraphMutex);
  juce::Array<juce::var> arr;
  for (auto& desc : mDocument.nodes)
  {
    auto* o = new juce::DynamicObject();
    o->setProperty ("id", desc.id);
    o->setProperty ("type", nodeTypeToString (desc.type));
    o->setProperty ("filePath", desc.params.filePath);

    bool loaded = false;
    juce::String err;
    if (auto it = mNodes.find (desc.id); it != mNodes.end())
    {
      if (auto* nam = dynamic_cast<NamRuntimeNode*> (it->second.get()))
      {
        loaded = nam->hasModel();
        err = nam->lastError;
      }
      else if (auto* ir = dynamic_cast<IrRuntimeNode*> (it->second.get()))
      {
        loaded = ir->hasIr();
        err = ir->lastError;
      }
    }
    o->setProperty ("loaded", loaded);
    o->setProperty ("error", err);
    arr.add (juce::var (o));
  }
  return juce::var (arr);
}

std::vector<juce::String> GraphEngine::topologicalOrder (const GraphDocument& doc) const
{
  std::unordered_map<juce::String, int> indeg;
  std::unordered_map<juce::String, std::vector<juce::String>> adj;
  for (auto& n : doc.nodes)
    indeg[n.id] = 0;
  for (auto& e : doc.edges)
  {
    adj[e.fromNode].push_back (e.toNode);
    indeg[e.toNode]++;
  }
  std::vector<juce::String> q;
  for (auto& [id, d] : indeg)
    if (d == 0)
      q.push_back (id);

  std::vector<juce::String> order;
  size_t qi = 0;
  while (qi < q.size())
  {
    auto u = q[qi++];
    order.push_back (u);
    for (auto& v : adj[u])
    {
      if (--indeg[v] == 0)
        q.push_back (v);
    }
  }
  return order;
}

void GraphEngine::rebuildLocked()
{
  mOutgoing.clear();
  mIncoming.clear();
  for (auto& e : mDocument.edges)
  {
    mOutgoing[e.fromNode].push_back (e);
    mIncoming[e.toNode].push_back (e);
  }

  std::unordered_map<juce::String, std::unique_ptr<RuntimeNode>> next;
  for (auto& desc : mDocument.nodes)
  {
    if (auto it = mNodes.find (desc.id); it != mNodes.end() && it->second->type() == desc.type)
    {
      auto* ir = dynamic_cast<IrRuntimeNode*> (it->second.get());
      const juce::String prevIr = ir != nullptr ? ir->filePath : juce::String();
      it->second->setParams (desc.params);
      next[desc.id] = std::move (it->second);
      // Reload IR when the path changes on an existing node (setParams no longer loads).
      if (ir != nullptr && desc.params.filePath.isNotEmpty()
          && desc.params.filePath != prevIr)
        loadIrAsync (ir, juce::File (desc.params.filePath));
    }
    else
    {
      auto node = createRuntimeNode (desc);
      node->prepare (mSampleRate, mMaxBlock);
      if (desc.type == NodeType::Nam && desc.params.filePath.isNotEmpty())
      {
        auto* nam = dynamic_cast<NamRuntimeNode*> (node.get());
        loadNamAsync (nam, juce::File (desc.params.filePath));
      }
      if (desc.type == NodeType::Ir && desc.params.filePath.isNotEmpty())
      {
        auto* ir = dynamic_cast<IrRuntimeNode*> (node.get());
        loadIrAsync (ir, juce::File (desc.params.filePath));
      }
      next[desc.id] = std::move (node);
    }
  }
  mNodes = std::move (next);
  mOrder = topologicalOrder (mDocument);

  mInputChannel = 0;
  mOutputChannel = 0;
  mOutputPan = 0.0f;
  mOutputGain = 1.0f;
  for (auto& desc : mDocument.nodes)
  {
    if (desc.type == NodeType::Input)
      mInputChannel = juce::jmax (0, desc.params.inputChannel);
    if (desc.type == NodeType::Output)
    {
      mOutputPan = juce::jlimit (-1.0f, 1.0f, desc.params.pan);
      mOutputGain = juce::Decibels::decibelsToGain (desc.params.levelDb);
      mOutputChannel = juce::jmax (0, desc.params.outputChannel);
    }
  }
}

void GraphEngine::process (juce::AudioBuffer<float>& buffer)
{
  const auto start = std::chrono::steady_clock::now();
  const int numSamples = buffer.getNumSamples();
  const int numChans = buffer.getNumChannels();
  if (numSamples <= 0 || numChans <= 0)
    return;

  std::lock_guard lock (mGraphMutex);

  // Never wipe the host buffer if the runtime graph isn't built — that produced
  // dead silence / noise-floor hiss in VST until the UI happened to call setGraph.
  if (mNodes.empty() || mOrder.empty())
  {
    if (! mDocument.nodes.empty())
      rebuildLocked();
    if (mNodes.empty() || mOrder.empty())
      return; // leave host buffer alone (passthrough)
  }

  auto channelPeak = [&] (int ch) -> float
  {
    if (ch < 0 || ch >= numChans)
      return 0.0f;
    float peak = 0.0f;
    const float* p = buffer.getReadPointer (ch);
    for (int i = 0; i < numSamples; ++i)
      peak = juce::jmax (peak, std::abs (p[i]));
    return peak;
  };

  auto smoothPeak = [] (std::atomic<float>& slot, float peak)
  {
    slot.store (peak * 0.35f + slot.load() * 0.65f);
  };

  // Meter host signal before input trim so VU reflects what the DAW is actually sending.
  const float hostInL = channelPeak (0);
  const float hostInR = channelPeak (numChans > 1 ? 1 : 0);
  const float hostInPeak = juce::jmax (hostInL, hostInR);
  smoothPeak (mInputPeakL, hostInL);
  smoothPeak (mInputPeakR, hostInR);

  // Keep a dry copy for failover if the graph somehow kills a live input.
  juce::AudioBuffer<float> dryCopy (numChans, numSamples);
  for (int c = 0; c < numChans; ++c)
    dryCopy.copyFrom (c, 0, buffer, c, 0, numSamples);

  // Global input trim — pull down hot interfaces before NAM
  const float inG = juce::Decibels::decibelsToGain (mMasterInDb.load());
  if (inG != 1.0f)
    buffer.applyGain (inG);

  const int inCh = juce::jlimit (0, numChans - 1, mInputChannel);
  if (mMono.getNumChannels() < 1 || mMono.getNumSamples() < numSamples)
    mMono.setSize (1, numSamples, false, false, true);
  mMono.copyFrom (0, 0, buffer, inCh, 0, numSamples);
  smoothPeak (mInputPeak, channelPeak (inCh));

  struct StereoSig
  {
    std::vector<float> L, R;
  };
  std::unordered_map<juce::String, StereoSig> signals;
  signals.reserve (mNodes.size());

  auto ensure = [&] (const juce::String& id) -> StereoSig&
  {
    auto& v = signals[id];
    if ((int) v.L.size() != numSamples)
    {
      v.L.assign ((size_t) numSamples, 0.0f);
      v.R.assign ((size_t) numSamples, 0.0f);
    }
    return v;
  };

  auto splitBranchGain = [] (float bal, int branchIndex, int numBranches) -> float
  {
    if (numBranches <= 1)
      return 1.0f;
    if (numBranches == 2)
    {
      return branchIndex == 0 ? juce::jmin (1.0f, (1.0f - bal) * 2.0f)
                              : juce::jmin (1.0f, bal * 2.0f);
    }
    return 1.0f;
  };

  for (auto& id : mOrder)
  {
    auto nit = mNodes.find (id);
    if (nit == mNodes.end())
      continue;
    auto* node = nit->second.get();
    auto& outSig = ensure (id);

    auto findDesc = [&]() -> const GraphNode*
    {
      for (auto& d : mDocument.nodes)
        if (d.id == id)
          return &d;
      return nullptr;
    };

    auto isStereoFxNode = [] (const GraphNode* d) -> bool
    {
      if (d == nullptr || d->type != NodeType::Fx)
        return false;
      return d->params.fxId.equalsIgnoreCase ("reverb")
             || d->params.fxId.equalsIgnoreCase ("delay")
             || d->params.fxId.equalsIgnoreCase ("pingpong")
             || d->params.fxId.equalsIgnoreCase ("chorus")
             || d->params.displayName.containsIgnoreCase ("reverb")
             || d->params.displayName.containsIgnoreCase ("delay")
             || d->params.displayName.containsIgnoreCase ("ping")
             || d->params.displayName.containsIgnoreCase ("chorus");
    };

    auto numOutPorts = [&] (const GraphNode* d) -> int
    {
      if (d == nullptr) return 1;
      if (d->type == NodeType::Output) return 0;
      if (d->type == NodeType::Split) return 2;
      if (isStereoFxNode (d)) return 2;
      return 1;
    };

    auto numInPorts = [&] (const GraphNode* d) -> int
    {
      if (d == nullptr) return 1;
      if (d->type == NodeType::Input) return 0;
      if (d->type == NodeType::Merge) return 2;
      if (d->type == NodeType::Output) return 1; // one stereo bus in
      if (isStereoFxNode (d)) return 2;
      return 1; // Split, NAM, IR, gate: single in
    };

    auto pickChannel = [] (const StereoSig& s, int port, int numPorts) -> const std::vector<float>&
    {
      if (numPorts <= 1)
        return s.L;
      return port <= 0 ? s.L : s.R;
    };

    if (node->type() == NodeType::Input)
    {
      int ch = 0;
      if (mDawHosted)
      {
        // DAW owns routing. Mix every host input channel to mono so mono-on-L,
        // mono-on-R, or odd pin maps still feed the amp (Reaper pin connector).
        mInputChannel = 0;
        if (numChans <= 1)
        {
          juce::FloatVectorOperations::copy (outSig.L.data(), buffer.getReadPointer (0), numSamples);
        }
        else
        {
          const float scale = 1.0f / (float) numChans;
          for (int i = 0; i < numSamples; ++i)
          {
            float sum = 0.0f;
            for (int c = 0; c < numChans; ++c)
              sum += buffer.getSample (c, i);
            outSig.L[(size_t) i] = sum * scale;
          }
        }
        juce::FloatVectorOperations::copy (outSig.R.data(), outSig.L.data(), numSamples);
        continue;
      }

      if (auto* d = findDesc())
        ch = d->params.inputChannel;
      ch = juce::jlimit (0, numChans - 1, ch);

      // Standalone: if the picked device channel is empty but another isn't,
      // use the loudest (avoids silent NAM from a wrong interface channel).
      if (numChans > 1)
      {
        auto blockPeak = [&] (int c) -> float
        {
          float peak = 0.0f;
          const float* p = buffer.getReadPointer (c);
          for (int i = 0; i < numSamples; ++i)
            peak = juce::jmax (peak, std::abs (p[i]));
          return peak;
        };

        const float selPeak = blockPeak (ch);
        int bestCh = ch;
        float bestPeak = selPeak;
        for (int c = 0; c < numChans; ++c)
        {
          const float pk = blockPeak (c);
          if (pk > bestPeak)
          {
            bestPeak = pk;
            bestCh = c;
          }
        }
        if (bestPeak > 1.0e-4f && selPeak < bestPeak * 0.05f)
          ch = bestCh;
      }

      mInputChannel = ch;
      juce::FloatVectorOperations::copy (outSig.L.data(), buffer.getReadPointer (ch), numSamples);
      juce::FloatVectorOperations::copy (outSig.R.data(), outSig.L.data(), numSamples);
      continue;
    }

    const GraphNode* desc = findDesc();
    const int inPorts = numInPorts (desc);
    auto inEdges = mIncoming[id];

    // Gather inputs into work buffers (port-aware)
    StereoSig work;
    work.L.assign ((size_t) numSamples, 0.0f);
    work.R.assign ((size_t) numSamples, 0.0f);
    bool gotL = false, gotR = false;

    if (node->type() == NodeType::Merge && inPorts >= 2)
    {
      const GraphEdge* edgeA = nullptr;
      const GraphEdge* edgeB = nullptr;
      for (auto& e : inEdges)
      {
        if (e.toPort <= 0 && edgeA == nullptr) edgeA = &e;
        else if (e.toPort >= 1 && edgeB == nullptr) edgeB = &e;
      }
      // Fallback: first two edges by order if ports not set
      if (edgeA == nullptr && ! inEdges.empty()) edgeA = &inEdges[0];
      if (edgeB == nullptr && inEdges.size() >= 2) edgeB = &inEdges[1];

      float bal = 0.5f;
      if (auto* merge = dynamic_cast<MergeRuntimeNode*> (node))
        bal = merge->bypass ? 0.5f : merge->balance();

      StereoSig zeros;
      zeros.L.assign ((size_t) numSamples, 0.0f);
      zeros.R.assign ((size_t) numSamples, 0.0f);
      auto& a = edgeA ? ensure (edgeA->fromNode) : zeros;
      auto& b = edgeB ? ensure (edgeB->fromNode) : zeros;
      // Take stereo bus from each source (mono sources have L==R)
      for (int i = 0; i < numSamples; ++i)
      {
        work.L[(size_t) i] = a.L[(size_t) i] * (1.0f - bal) + b.L[(size_t) i] * bal;
        work.R[(size_t) i] = a.R[(size_t) i] * (1.0f - bal) + b.R[(size_t) i] * bal;
      }
      gotL = gotR = true;
    }
    else if (inPorts >= 2)
    {
      // Stereo FX: each in-port is a mono channel (L or R); one cable → mono upmix
      for (auto& e : inEdges)
      {
        auto& src = ensure (e.fromNode);
        int srcOuts = 1;
        for (auto& d : mDocument.nodes)
          if (d.id == e.fromNode) { srcOuts = numOutPorts (&d); break; }
        const auto& ch = pickChannel (src, e.fromPort, srcOuts);
        auto& dest = e.toPort <= 0 ? work.L : work.R;
        juce::FloatVectorOperations::copy (dest.data(), ch.data(), numSamples);
        if (e.toPort <= 0) gotL = true;
        else gotR = true;
      }
      if (gotL && ! gotR)
      {
        juce::FloatVectorOperations::copy (work.R.data(), work.L.data(), numSamples);
        gotR = true;
      }
      else if (gotR && ! gotL)
      {
        juce::FloatVectorOperations::copy (work.L.data(), work.R.data(), numSamples);
        gotL = true;
      }
    }
    else if (! inEdges.empty())
    {
      // Single in-port: take full stereo bus from source (Output, Split, NAM, gate, …)
      const auto& e = inEdges[0];
      auto& src = ensure (e.fromNode);
      int srcOuts = 1;
      bool srcIsSplit = false;
      for (auto& d : mDocument.nodes)
        if (d.id == e.fromNode)
        {
          srcOuts = numOutPorts (&d);
          srcIsSplit = d.type == NodeType::Split;
          break;
        }

      if (srcIsSplit && srcOuts >= 2)
      {
        // Split A/B: one mono branch into both channels
        const auto& ch = pickChannel (src, e.fromPort, srcOuts);
        juce::FloatVectorOperations::copy (work.L.data(), ch.data(), numSamples);
        juce::FloatVectorOperations::copy (work.R.data(), ch.data(), numSamples);

        if (auto sit = mNodes.find (e.fromNode); sit != mNodes.end())
        {
          if (auto* split = dynamic_cast<SplitRuntimeNode*> (sit->second.get()))
          {
            const int branchIdx = juce::jlimit (0, 1, e.fromPort);
            const float g = split->bypass ? 1.0f
                                          : splitBranchGain (split->balance(), branchIdx, 2);
            if (g != 1.0f)
            {
              juce::FloatVectorOperations::multiply (work.L.data(), g, numSamples);
              juce::FloatVectorOperations::multiply (work.R.data(), g, numSamples);
            }
          }
        }
      }
      else
      {
        // Full stereo bus (NAM/IR/FX/Merge). Stereo FX L/R outs still carry the bus
        // when feeding a single-in destination so Output stays one cable.
        juce::FloatVectorOperations::copy (work.L.data(), src.L.data(), numSamples);
        juce::FloatVectorOperations::copy (work.R.data(), src.R.data(), numSamples);
      }
      gotL = gotR = true;
    }

    juce::FloatVectorOperations::copy (outSig.L.data(), work.L.data(), numSamples);
    juce::FloatVectorOperations::copy (outSig.R.data(), work.R.data(), numSamples);

    if (node->type() == NodeType::Split)
      continue; // fan-out handled at consumers via fromPort

    if (node->type() == NodeType::Output)
      continue; // summed later from collected signals

    node->process (outSig.L.data(), outSig.R.data(), numSamples);
  }

  // Sum every Output node — each keeps its own pan / level / host channel pair.
  // Parallel Outputs into the same channels are power-compensated (1/√N) so dual-amp
  // graphs don't instantly clip. Master + soft-clip still apply after.
  buffer.clear();

  std::vector<int> writers ((size_t) numChans, 0);
  for (auto& desc : mDocument.nodes)
  {
    if (desc.type != NodeType::Output)
      continue;
    if (signals.find (desc.id) == signals.end())
      continue;

    const bool hostOut = ! mDawHosted
                         && (desc.params.fxId.equalsIgnoreCase ("host_out")
                             || desc.params.displayName.containsIgnoreCase ("host"));
    int hostL = 0;
    int hostR = numChans > 1 ? 1 : 0;
    if (hostOut)
    {
      hostL = juce::jlimit (0, numChans - 1, juce::jmax (0, desc.params.outputChannel));
      const int rawR = desc.params.outputChannelR >= 0 ? desc.params.outputChannelR
                                                       : desc.params.outputChannel + 1;
      hostR = juce::jlimit (0, numChans - 1, rawR);
    }
    ++writers[(size_t) hostL];
    if (numChans > 1)
      ++writers[(size_t) hostR];
  }

  for (auto& desc : mDocument.nodes)
  {
    if (desc.type != NodeType::Output)
      continue;

    auto it = signals.find (desc.id);
    if (it == signals.end())
      continue;

    const float pan = juce::jlimit (-1.0f, 1.0f, desc.params.pan);
    const float level = juce::Decibels::decibelsToGain (desc.params.levelDb);
    const float balL = pan > 0.0f ? (1.0f - pan) : 1.0f;
    const float balR = pan < 0.0f ? (1.0f + pan) : 1.0f;

    const bool hostOut = ! mDawHosted
                         && (desc.params.fxId.equalsIgnoreCase ("host_out")
                             || desc.params.displayName.containsIgnoreCase ("host"));
    int hostL = 0;
    int hostR = numChans > 1 ? 1 : 0;
    if (hostOut)
    {
      hostL = juce::jlimit (0, numChans - 1, juce::jmax (0, desc.params.outputChannel));
      const int rawR = desc.params.outputChannelR >= 0 ? desc.params.outputChannelR
                                                       : desc.params.outputChannel + 1;
      hostR = juce::jlimit (0, numChans - 1, rawR);
    }

    const float compL = 1.0f / std::sqrt ((float) juce::jmax (1, writers[(size_t) hostL]));
    const float compR = 1.0f / std::sqrt ((float) juce::jmax (1, writers[(size_t) hostR]));
    const float gainL = balL * level * compL;
    const float gainR = balR * level * compR;

    for (int i = 0; i < numSamples; ++i)
    {
      const float l = it->second.L[(size_t) i] * gainL;
      const float r = it->second.R[(size_t) i] * gainR;
      if (numChans == 1)
      {
        buffer.addSample (0, i, 0.5f * (l + r));
      }
      else
      {
        buffer.addSample (hostL, i, l);
        buffer.addSample (hostR, i, r);
      }
    }
  }

  // Master trim. Leave signal linear at/under full-scale — the old 0.8 knee
  // constantly bent peaks into HF hash. Only clamp true overs.
  const float masterG = juce::Decibels::decibelsToGain (mMasterOutDb.load());
  bool clipped = false;

  float meterL = 0.0f, meterR = 0.0f;
  float outPeak = 0.0f;
  for (int ch = 0; ch < numChans; ++ch)
  {
    float* p = buffer.getWritePointer (ch);
    for (int i = 0; i < numSamples; ++i)
    {
      float s = p[i] * masterG;
      if (s > 1.0f) { s = 1.0f; clipped = true; }
      else if (s < -1.0f) { s = -1.0f; clipped = true; }
      p[i] = s;
      const float a = std::abs (s);
      outPeak = juce::jmax (outPeak, a);
      if (ch == 0) meterL = juce::jmax (meterL, a);
      if (ch == (numChans > 1 ? 1 : 0)) meterR = juce::jmax (meterR, a);
    }
  }

  // If the DAW sent real audio but we produced near-silence (broken bus / empty
  // graph path), fall back to dry host audio so the track isn't dead.
  if (hostInPeak > 1.0e-4f && outPeak < 1.0e-5f)
  {
    for (int c = 0; c < numChans; ++c)
      buffer.copyFrom (c, 0, dryCopy, c, 0, numSamples);
    meterL = hostInL;
    meterR = hostInR;
    outPeak = hostInPeak;
  }

  smoothPeak (mOutputPeakL, meterL);
  smoothPeak (mOutputPeakR, meterR);
  smoothPeak (mOutputPeak, outPeak);
  mWasClipping.store (clipped);

  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double blockSec = (double) numSamples / mSampleRate;
  const double used = std::chrono::duration<double> (elapsed).count();
  if (blockSec > 0.0)
    mCpuLoad.store ((float) (used / blockSec));
}

void GraphEngine::setMasterInDb (float db)
{
  mMasterInDb.store (juce::jlimit (-60.0f, 24.0f, db));
}

void GraphEngine::setMasterOutDb (float db)
{
  mMasterOutDb.store (juce::jlimit (-60.0f, 12.0f, db));
}

} // namespace namplifier
