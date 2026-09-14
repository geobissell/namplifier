#include "GraphModel.h"

namespace namplifier
{

juce::String nodeTypeToString (NodeType t)
{
  switch (t)
  {
    case NodeType::Input:  return "input";
    case NodeType::Output: return "output";
    case NodeType::Nam:    return "nam";
    case NodeType::Ir:     return "ir";
    case NodeType::Split:  return "split";
    case NodeType::Merge:  return "merge";
    case NodeType::Bypass:    return "bypass";
    case NodeType::Fx:        return "fx";
    case NodeType::MediaFile: return "media";
    case NodeType::YouTube:   return "youtube";
  }
  return "bypass";
}

NodeType nodeTypeFromString (const juce::String& s)
{
  if (s == "input")  return NodeType::Input;
  if (s == "output") return NodeType::Output;
  if (s == "nam")    return NodeType::Nam;
  if (s == "ir")     return NodeType::Ir;
  if (s == "split")  return NodeType::Split;
  if (s == "merge")  return NodeType::Merge;
  if (s == "fx")     return NodeType::Fx;
  if (s == "media" || s == "media_file" || s == "mediafile") return NodeType::MediaFile;
  if (s == "youtube" || s == "yt") return NodeType::YouTube;
  return NodeType::Bypass;
}

static juce::var paramsToVar (const NodeParams& p)
{
  auto* obj = new juce::DynamicObject();
  obj->setProperty ("levelDb", p.levelDb);
  obj->setProperty ("mix", p.mix);
  obj->setProperty ("slim", p.slim);
  obj->setProperty ("bypass", p.bypass);
  obj->setProperty ("filePath", p.filePath);
  obj->setProperty ("displayName", p.displayName);
  obj->setProperty ("toneId", p.toneId);
  obj->setProperty ("modelId", p.modelId);
  obj->setProperty ("imageUrl", p.imageUrl);
  obj->setProperty ("cabIncluded", p.cabIncluded);
  obj->setProperty ("inputChannel", p.inputChannel);
  obj->setProperty ("pan", p.pan);
  obj->setProperty ("outputChannel", p.outputChannel);
  obj->setProperty ("outputChannelR", p.outputChannelR);
  obj->setProperty ("bassDb", p.bassDb);
  obj->setProperty ("midDb", p.midDb);
  obj->setProperty ("trebleDb", p.trebleDb);
  obj->setProperty ("roomSize", p.roomSize);
  obj->setProperty ("damping", p.damping);
  obj->setProperty ("width", p.width);
  obj->setProperty ("delayMs", p.delayMs);
  obj->setProperty ("feedback", p.feedback);
  obj->setProperty ("thresholdDb", p.thresholdDb);
  obj->setProperty ("attackMs", p.attackMs);
  obj->setProperty ("releaseMs", p.releaseMs);
  obj->setProperty ("gateRangeDb", p.gateRangeDb);
  obj->setProperty ("hysteresisDb", p.hysteresisDb);
  obj->setProperty ("holdMs", p.holdMs);
  obj->setProperty ("ratio", p.ratio);
  obj->setProperty ("makeupDb", p.makeupDb);
  obj->setProperty ("rateHz", p.rateHz);
  obj->setProperty ("depth", p.depth);
  obj->setProperty ("fxMode", p.fxMode);
  obj->setProperty ("fxId", p.fxId);
  obj->setProperty ("mediaPlaying", p.mediaPlaying);
  obj->setProperty ("mediaLoop", p.mediaLoop);
  obj->setProperty ("mediaSeekSec", p.mediaSeekSec);
  obj->setProperty ("mediaUrl", p.mediaUrl);
  return juce::var (obj);
}

static NodeParams paramsFromVar (const juce::var& v)
{
  NodeParams p;
  if (auto* o = v.getDynamicObject())
  {
    p.levelDb = (float) o->getProperty ("levelDb");
    p.mix = (float) o->getProperty ("mix");
    if (o->hasProperty ("slim"))
      p.slim = (float) o->getProperty ("slim");
    else
      p.slim = 1.0f;
    p.bypass = (bool) o->getProperty ("bypass");
    p.filePath = o->getProperty ("filePath").toString();
    p.displayName = o->getProperty ("displayName").toString();
    p.toneId = o->getProperty ("toneId").toString();
    p.modelId = o->getProperty ("modelId").toString();
    p.imageUrl = o->getProperty ("imageUrl").toString();
    p.cabIncluded = (bool) o->getProperty ("cabIncluded");
    p.inputChannel = (int) o->getProperty ("inputChannel");
    p.pan = o->hasProperty ("pan") ? (float) o->getProperty ("pan") : 0.0f;
    p.outputChannel = o->hasProperty ("outputChannel") ? (int) o->getProperty ("outputChannel") : 0;
    p.outputChannelR = o->hasProperty ("outputChannelR") ? (int) o->getProperty ("outputChannelR") : -1;
    p.bassDb = (float) o->getProperty ("bassDb");
    p.midDb = (float) o->getProperty ("midDb");
    p.trebleDb = (float) o->getProperty ("trebleDb");
    p.roomSize = o->hasProperty ("roomSize") ? (float) o->getProperty ("roomSize") : 0.45f;
    p.damping = o->hasProperty ("damping") ? (float) o->getProperty ("damping") : 0.4f;
    p.width = o->hasProperty ("width") ? (float) o->getProperty ("width") : 1.0f;
    p.delayMs = o->hasProperty ("delayMs") ? (float) o->getProperty ("delayMs") : 350.0f;
    p.feedback = o->hasProperty ("feedback") ? (float) o->getProperty ("feedback") : 0.35f;
    p.thresholdDb = o->hasProperty ("thresholdDb") ? (float) o->getProperty ("thresholdDb") : -50.0f;
    p.attackMs = o->hasProperty ("attackMs") ? (float) o->getProperty ("attackMs") : 2.0f;
    p.releaseMs = o->hasProperty ("releaseMs") ? (float) o->getProperty ("releaseMs") : 150.0f;
    p.gateRangeDb = o->hasProperty ("gateRangeDb") ? (float) o->getProperty ("gateRangeDb") : -70.0f;
    p.hysteresisDb = o->hasProperty ("hysteresisDb") ? (float) o->getProperty ("hysteresisDb") : 8.0f;
    p.holdMs = o->hasProperty ("holdMs") ? (float) o->getProperty ("holdMs") : 80.0f;
    p.ratio = o->hasProperty ("ratio") ? (float) o->getProperty ("ratio") : 4.0f;
    p.makeupDb = o->hasProperty ("makeupDb") ? (float) o->getProperty ("makeupDb") : 0.0f;
    p.rateHz = o->hasProperty ("rateHz") ? (float) o->getProperty ("rateHz") : 0.8f;
    p.depth = o->hasProperty ("depth") ? (float) o->getProperty ("depth") : 0.35f;
    p.fxMode = o->getProperty ("fxMode").toString();
    p.fxId = o->getProperty ("fxId").toString();
    p.mediaPlaying = o->hasProperty ("mediaPlaying") && (bool) o->getProperty ("mediaPlaying");
    p.mediaLoop = o->hasProperty ("mediaLoop") && (bool) o->getProperty ("mediaLoop");
    p.mediaSeekSec = o->hasProperty ("mediaSeekSec") ? (float) o->getProperty ("mediaSeekSec") : -1.0f;
    p.mediaUrl = o->getProperty ("mediaUrl").toString();
  }
  return p;
}

juce::var GraphDocument::toVar() const
{
  auto* root = new juce::DynamicObject();
  root->setProperty ("version", version);
  root->setProperty ("name", name);

  juce::Array<juce::var> nodeArr;
  for (auto& n : nodes)
  {
    auto* o = new juce::DynamicObject();
    o->setProperty ("id", n.id);
    o->setProperty ("type", nodeTypeToString (n.type));
    o->setProperty ("x", n.x);
    o->setProperty ("y", n.y);
    o->setProperty ("params", paramsToVar (n.params));
    nodeArr.add (juce::var (o));
  }
  root->setProperty ("nodes", juce::var (nodeArr));

  juce::Array<juce::var> edgeArr;
  for (auto& e : edges)
  {
    auto* o = new juce::DynamicObject();
    o->setProperty ("id", e.id);
    o->setProperty ("fromNode", e.fromNode);
    o->setProperty ("fromPort", e.fromPort);
    o->setProperty ("toNode", e.toNode);
    o->setProperty ("toPort", e.toPort);
    edgeArr.add (juce::var (o));
  }
  root->setProperty ("edges", juce::var (edgeArr));
  return juce::var (root);
}

GraphDocument GraphDocument::fromVar (const juce::var& v)
{
  GraphDocument doc;
  auto* root = v.getDynamicObject();
  if (root == nullptr)
    return makeEmptyChain();

  doc.version = (int) root->getProperty ("version");
  doc.name = root->getProperty ("name").toString();
  if (doc.name.isEmpty())
    doc.name = "Untitled";

  if (auto* nodes = root->getProperty ("nodes").getArray())
  {
    for (auto& nv : *nodes)
    {
      if (auto* o = nv.getDynamicObject())
      {
        GraphNode n;
        n.id = o->getProperty ("id").toString();
        n.type = nodeTypeFromString (o->getProperty ("type").toString());
        n.x = (float) o->getProperty ("x");
        n.y = (float) o->getProperty ("y");
        n.params = paramsFromVar (o->getProperty ("params"));
        doc.nodes.push_back (std::move (n));
      }
    }
  }

  if (auto* edges = root->getProperty ("edges").getArray())
  {
    for (auto& ev : *edges)
    {
      if (auto* o = ev.getDynamicObject())
      {
        GraphEdge e;
        e.id = o->getProperty ("id").toString();
        e.fromNode = o->getProperty ("fromNode").toString();
        e.fromPort = (int) o->getProperty ("fromPort");
        e.toNode = o->getProperty ("toNode").toString();
        e.toPort = (int) o->getProperty ("toPort");
        doc.edges.push_back (std::move (e));
      }
    }
  }

  if (doc.nodes.empty())
    return makeEmptyChain();

  return doc;
}

juce::String GraphDocument::toJson() const
{
  return juce::JSON::toString (toVar(), true);
}

GraphDocument GraphDocument::fromJson (const juce::String& json)
{
  auto parsed = juce::JSON::parse (json);
  if (parsed.isVoid())
    return makeEmptyChain();
  return fromVar (parsed);
}

GraphDocument GraphDocument::makeEmptyChain()
{
  GraphDocument doc;
  doc.name = "Empty";
  GraphNode in { "input", NodeType::Input, 80.0f, 200.0f, {} };
  GraphNode out { "output", NodeType::Output, 520.0f, 200.0f, {} };
  doc.nodes = { in, out };
  doc.edges = { { "e1", "input", 0, "output", 0 } };
  return doc;
}

GraphDocument GraphDocument::makeAmpCabStarter()
{
  GraphDocument doc;
  doc.name = "Amp -> Cab";
  GraphNode in { "input", NodeType::Input, 60.0f, 200.0f, {} };
  GraphNode amp { "nam1", NodeType::Nam, 220.0f, 200.0f, {} };
  amp.params.displayName = "NAM Amp";
  GraphNode cab { "ir1", NodeType::Ir, 400.0f, 200.0f, {} };
  cab.params.displayName = "IR Cab";
  GraphNode out { "output", NodeType::Output, 580.0f, 200.0f, {} };
  doc.nodes = { in, amp, cab, out };
  doc.edges = {
    { "e1", "input", 0, "nam1", 0 },
    { "e2", "nam1", 0, "ir1", 0 },
    { "e3", "ir1", 0, "output", 0 }
  };
  return doc;
}

} // namespace namplifier
