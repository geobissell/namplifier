#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/GraphEngine.h"
#include "tone3000/Tone3000Client.h"
#include "presets/PresetManager.h"
#include "presets/LibraryManager.h"
#include "host/PluginCatalog.h"

class NamplifierAudioProcessor : public juce::AudioProcessor
{
public:
  NamplifierAudioProcessor();
  ~NamplifierAudioProcessor() override;

  void prepareToPlay (double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;
  bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
  void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override { return true; }

  const juce::String getName() const override { return "Namplifier"; }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  bool isMidiEffect() const override { return false; }
  double getTailLengthSeconds() const override { return 2.0; }

  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram (int) override {}
  const juce::String getProgramName (int) override { return {}; }
  void changeProgramName (int, const juce::String&) override {}

  void getStateInformation (juce::MemoryBlock& destData) override;
  void setStateInformation (const void* data, int sizeInBytes) override;

  namplifier::GraphEngine& getGraphEngine() { return graphEngine; }
  namplifier::Tone3000Client& getTone3000() { return tone3000; }
  namplifier::PresetManager& getPresets() { return presets; }
  namplifier::LibraryManager& getLibrary() { return library; }
  namplifier::PluginCatalog& getPluginCatalog() { return pluginCatalog; }
  void syncVstLibraryFromCatalog();

  std::function<void()> onGraphChanged;

private:
  namplifier::GraphEngine graphEngine;
  namplifier::Tone3000Client tone3000;
  namplifier::PresetManager presets;
  namplifier::LibraryManager library;
  namplifier::PluginCatalog pluginCatalog;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NamplifierAudioProcessor)
};
