#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace namplifier
{

enum class NodeType
{
  Input,
  Output,
  Nam,
  Ir,
  Split,
  Merge,
  Bypass,
  Fx,
  MediaFile,
  YouTube,
  Vst
};

struct NodeParams
{
  float levelDb = 0.0f;
  float mix = 1.0f;
  float slim = 1.0f;
  bool bypass = false;
  juce::String filePath;
  juce::String displayName;
  juce::String toneId;
  juce::String modelId;
  juce::String imageUrl;
  bool cabIncluded = false; // amp+cab capture (skip separate IR)

  // Input
  int inputChannel = 0; // 0-based device/bus channel

  // Output
  float pan = 0.0f; // -1 L .. +1 R
  int outputChannel = 0; // L host channel (main Output always uses 0/1)
  int outputChannelR = -1; // Host Output R; -1 → outputChannel + 1

  // Simple amp tone stack (NAM) / reverb colouring
  float bassDb = 0.0f;
  float midDb = 0.0f;
  float trebleDb = 0.0f;

  // Reverb (Fx)
  float roomSize = 0.45f;
  float damping = 0.4f;
  float width = 1.0f;

  // Delay (Fx)
  float delayMs = 350.0f;
  float feedback = 0.35f;

  // Noise gate / compressor (Fx)
  float thresholdDb = -50.0f;
  float attackMs = 2.0f;
  float releaseMs = 150.0f;
  float gateRangeDb = -70.0f; // attenuation when closed
  float hysteresisDb = 8.0f;  // close threshold = open - hysteresis
  float holdMs = 80.0f;       // stay open this long after signal drops
  float ratio = 4.0f;         // compressor
  float makeupDb = 0.0f;      // compressor makeup

  // Chorus (Fx)
  float rateHz = 0.8f;
  float depth = 0.35f;

  // Character / algorithm: delay digital|analogue|tape · reverb room|plate|spring
  juce::String fxMode;

  juce::String fxId; // "reverb" | "delay" | "pingpong" | "gate" | "chorus" | "compressor" | "host_out"

  // MediaFile / YouTube transport (sources — no graph inputs)
  bool mediaPlaying = false;
  bool mediaLoop = false;
  float mediaSeekSec = -1.0f; // >= 0 requests a seek; runtime clears after apply
  juce::String mediaUrl;      // YouTube watch URL or video id

  // Hosted VST3
  int vstIns = 2;  // graph in-ports 1–2
  int vstOuts = 2; // graph out-ports 1–2
  juce::String pluginState; // base64 AudioProcessor state
};

struct GraphNode
{
  juce::String id;
  NodeType type = NodeType::Bypass;
  float x = 0.0f;
  float y = 0.0f;
  NodeParams params;
};

struct GraphEdge
{
  juce::String id;
  juce::String fromNode;
  int fromPort = 0;
  juce::String toNode;
  int toPort = 0;
};

struct GraphDocument
{
  int version = 1;
  juce::String name { "Untitled" };
  std::vector<GraphNode> nodes;
  std::vector<GraphEdge> edges;

  juce::var toVar() const;
  static GraphDocument fromVar (const juce::var& v);
  juce::String toJson() const;
  static GraphDocument fromJson (const juce::String& json);

  static GraphDocument makeEmptyChain();
  static GraphDocument makeAmpCabStarter();
};

juce::String nodeTypeToString (NodeType t);
NodeType nodeTypeFromString (const juce::String& s);

} // namespace namplifier
