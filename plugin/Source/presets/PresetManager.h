#pragma once

#include <juce_core/juce_core.h>
#include "../dsp/GraphModel.h"

namespace namplifier
{

class PresetManager
{
public:
  PresetManager();

  juce::File getPresetsDir() const;
  juce::StringArray listPresets() const;
  bool savePreset (const juce::String& name, const GraphDocument& doc, juce::String& errorOut);
  bool loadPreset (const juce::String& name, GraphDocument& doc, juce::String& errorOut);
  bool deletePreset (const juce::String& name, juce::String& errorOut);
  void ensureFactoryPresets();

private:
  juce::File presetFile (const juce::String& name) const;
};

} // namespace namplifier
