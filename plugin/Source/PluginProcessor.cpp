#include "PluginProcessor.h"
#include "PluginEditor.h"

NamplifierAudioProcessor::NamplifierAudioProcessor()
  : AudioProcessor (
      wrapperType == wrapperType_Standalone
        ? BusesProperties()
            .withInput ("Input", juce::AudioChannelSet::discreteChannels (8), true)
            .withOutput ("Output", juce::AudioChannelSet::discreteChannels (8), true)
        : BusesProperties()
            .withInput ("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
  graphEngine.setPluginHosted (wrapperType != wrapperType_Standalone);
  graphEngine.setPluginCatalog (&pluginCatalog);
  syncVstLibraryFromCatalog();
}

void NamplifierAudioProcessor::syncVstLibraryFromCatalog()
{
  juce::Array<namplifier::LibraryItem> items;
  for (auto& type : pluginCatalog.getList().getTypes())
  {
    namplifier::LibraryItem item;
    item.id = juce::Uuid().toDashedString();
    item.kind = namplifier::LibraryItemKind::Vst;
    item.name = type.name;
    item.filePath = type.fileOrIdentifier;
    item.modelId = type.createIdentifierString();
    item.format = type.pluginFormatName.isNotEmpty() ? type.pluginFormatName : "vst3";
    item.source = "scan";
    item.notes = type.manufacturerName;
    items.add (item);
  }
  library.syncScannedVsts (items);
}

NamplifierAudioProcessor::~NamplifierAudioProcessor() = default;

void NamplifierAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
  graphEngine.setPluginHosted (wrapperType != wrapperType_Standalone);
  graphEngine.setHostChannelCounts (getTotalNumInputChannels(), getTotalNumOutputChannels());
  graphEngine.prepare (sampleRate, samplesPerBlock);
}

void NamplifierAudioProcessor::releaseResources() {}

bool NamplifierAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  const auto& mainIn = layouts.getMainInputChannelSet();
  const auto& mainOut = layouts.getMainOutputChannelSet();

  if (wrapperType == wrapperType_Standalone)
  {
    if (mainIn.isDisabled() || mainOut.isDisabled())
      return false;
    if (mainIn.size() < 1 || mainIn.size() > 16)
      return false;
    if (mainOut.size() < 1 || mainOut.size() > 16)
      return false;
    return true;
  }

  // Standard JUCE FX: matching mono/stereo main buses.
  if (mainIn != mainOut || mainIn.isDisabled())
    return false;

  return mainIn == juce::AudioChannelSet::mono()
         || mainIn == juce::AudioChannelSet::stereo();
}

void NamplifierAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
  juce::ScopedNoDenormals noDenormals;
  graphEngine.setHostChannelCounts (getTotalNumInputChannels(), getTotalNumOutputChannels());

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
  graphEngine.capturePluginStates();
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
