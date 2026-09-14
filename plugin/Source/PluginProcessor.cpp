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
  // Prefer the main bus channel counts — not a permissive 2-in/4-out layout some hosts pick.
  const int nIn = getBusCount (true) > 0 ? getChannelCountOfBus (true, 0) : getTotalNumInputChannels();
  const int nOut = getBusCount (false) > 0 ? getChannelCountOfBus (false, 0) : getTotalNumOutputChannels();
  graphEngine.setHostChannelCounts (nIn, nOut);
  graphEngine.prepare (sampleRate, samplesPerBlock);
  setLatencySamples (graphEngine.getLatencySamples());
}

void NamplifierAudioProcessor::releaseResources() {}

bool NamplifierAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  // Keep DAW routing simple. Permitting arbitrary 1–16 layouts let Reaper open us as
  // "2 in 4 out" and confused pin mapping.
  const auto inns = layouts.getMainInputChannelSet();
  const auto outs = layouts.getMainOutputChannelSet();
  const bool inOk = inns == juce::AudioChannelSet::mono()
                    || inns == juce::AudioChannelSet::stereo();
  const bool outOk = outs == juce::AudioChannelSet::mono()
                     || outs == juce::AudioChannelSet::stereo();
  return inOk && outOk && ! inns.isDisabled() && ! outs.isDisabled();
}

void NamplifierAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
  juce::ScopedNoDenormals noDenormals;

  graphEngine.setDawHosted (wrapperType != wrapperType_Standalone);

  auto inBus = getBus (true, 0);
  auto outBus = getBus (false, 0);
  if (outBus == nullptr)
  {
    buffer.clear();
    return;
  }

  auto out = getBusBuffer (buffer, false, 0);
  const int numSamples = buffer.getNumSamples();
  const int outCh = out.getNumChannels();

  // Working buffer sized to main I/O so GraphEngine never sees padded aux channels.
  const int inCh = (inBus != nullptr) ? getBusBuffer (buffer, true, 0).getNumChannels() : 0;
  const int workCh = juce::jmax (1, juce::jmax (inCh, outCh));
  if (mWorkBuffer.getNumChannels() != workCh || mWorkBuffer.getNumSamples() < numSamples)
    mWorkBuffer.setSize (workCh, numSamples, false, false, true);
  mWorkBuffer.clear();

  if (inBus != nullptr)
  {
    auto in = getBusBuffer (buffer, true, 0);
    const int copyCh = juce::jmin (in.getNumChannels(), mWorkBuffer.getNumChannels());
    for (int c = 0; c < copyCh; ++c)
      mWorkBuffer.copyFrom (c, 0, in, c, 0, numSamples);
  }

  graphEngine.setHostChannelCounts (mWorkBuffer.getNumChannels(), mWorkBuffer.getNumChannels());
  graphEngine.process (mWorkBuffer);

  const int copyOut = juce::jmin (outCh, mWorkBuffer.getNumChannels());
  for (int c = 0; c < copyOut; ++c)
    out.copyFrom (c, 0, mWorkBuffer, c, 0, numSamples);
  for (int c = copyOut; c < outCh; ++c)
    out.clear (c, 0, numSamples);
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
