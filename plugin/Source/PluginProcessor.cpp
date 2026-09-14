#include "PluginProcessor.h"
#include "PluginEditor.h"

NamplifierAudioProcessor::NamplifierAudioProcessor()
  : AudioProcessor (BusesProperties()
                      .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
  graphEngine.setDawHosted (wrapperType != wrapperType_Standalone);
}

NamplifierAudioProcessor::~NamplifierAudioProcessor() = default;

void NamplifierAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
  graphEngine.setDawHosted (wrapperType != wrapperType_Standalone);
  graphEngine.setHostChannelCounts (getTotalNumInputChannels(), getTotalNumOutputChannels());
  graphEngine.prepare (sampleRate, samplesPerBlock);
  setLatencySamples (graphEngine.getLatencySamples());
}

void NamplifierAudioProcessor::releaseResources() {}

bool NamplifierAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  const auto inns = layouts.getMainInputChannelSet();
  const auto outs = layouts.getMainOutputChannelSet();

  // Never accept a disabled main bus as a valid layout. Doing so lets some hosts
  // "activate" us with 0 input channels → silence into high-gain NAM → hiss, and
  // the processBlock clear-loop then wipes the entire buffer when numIns == 0.
  if (inns.isDisabled() || outs.isDisabled())
    return false;

  const bool inOk = inns == juce::AudioChannelSet::mono()
                    || inns == juce::AudioChannelSet::stereo();
  const bool outOk = outs == juce::AudioChannelSet::mono()
                     || outs == juce::AudioChannelSet::stereo();
  return inOk && outOk;
}

void NamplifierAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
  juce::ScopedNoDenormals noDenormals;
  graphEngine.setDawHosted (wrapperType != wrapperType_Standalone);
  graphEngine.setHostChannelCounts (getTotalNumInputChannels(), getTotalNumOutputChannels());

  // Clear only extra output channels; keep host input intact for the graph.
  for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
    buffer.clear (i, 0, buffer.getNumSamples());

  graphEngine.process (buffer);
}

juce::AudioProcessorEditor* NamplifierAudioProcessor::createEditor()
{
  return new NamplifierAudioProcessorEditor (*this);
}

void NamplifierAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
  auto doc = graphEngine.getGraph();
  auto json = doc.toJson();
  destData.replaceAll (json.toRawUTF8(), (size_t) json.getNumBytesAsUTF8());
}

void NamplifierAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
  if (data == nullptr || sizeInBytes <= 0)
    return;

  auto json = juce::String::fromUTF8 (static_cast<const char*> (data), sizeInBytes);
  if (! json.containsChar ('{') || ! json.contains ("nodes"))
    return;

  auto doc = namplifier::GraphDocument::fromJson (json);
  if (doc.nodes.empty())
    return;

  graphEngine.setGraph (doc);
  if (onGraphChanged)
    onGraphChanged();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
  return new NamplifierAudioProcessor();
}
