#include "PresetManager.h"

namespace namplifier
{

PresetManager::PresetManager()
{
  ensureFactoryPresets();
}

juce::File PresetManager::getPresetsDir() const
{
  auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("Namplifier")
               .getChildFile ("Presets");
  dir.createDirectory();
  return dir;
}

juce::File PresetManager::presetFile (const juce::String& name) const
{
  auto safe = name.replaceCharacter ('/', '_').replaceCharacter ('\\', '_');
  return getPresetsDir().getChildFile (safe + ".namplifier.json");
}

void PresetManager::ensureFactoryPresets()
{
  auto empty = presetFile ("Empty");
  if (! empty.existsAsFile())
    empty.replaceWithText (GraphDocument::makeEmptyChain().toJson());

  auto ampCab = presetFile ("Amp Cab");
  if (! ampCab.existsAsFile())
    ampCab.replaceWithText (GraphDocument::makeAmpCabStarter().toJson());
}

juce::StringArray PresetManager::listPresets() const
{
  juce::StringArray names;
  for (auto& f : getPresetsDir().findChildFiles (juce::File::findFiles, false, "*.json"))
  {
    auto n = f.getFileName();
    if (n.endsWithIgnoreCase (".namplifier.json"))
      names.add (n.dropLastCharacters (16)); // .namplifier.json
  }
  names.sort (true);
  return names;
}

bool PresetManager::savePreset (const juce::String& name, const GraphDocument& doc, juce::String& errorOut)
{
  if (name.isEmpty())
  {
    errorOut = "Name required";
    return false;
  }
  auto copy = doc;
  copy.name = name;
  if (! presetFile (name).replaceWithText (copy.toJson()))
  {
    errorOut = "Write failed";
    return false;
  }
  return true;
}

bool PresetManager::loadPreset (const juce::String& name, GraphDocument& doc, juce::String& errorOut)
{
  auto f = presetFile (name);
  if (! f.existsAsFile())
  {
    errorOut = "Preset not found";
    return false;
  }
  doc = GraphDocument::fromJson (f.loadFileAsString());
  return true;
}

bool PresetManager::deletePreset (const juce::String& name, juce::String& errorOut)
{
  auto f = presetFile (name);
  if (! f.existsAsFile())
  {
    errorOut = "Preset not found";
    return false;
  }
  if (! f.deleteFile())
  {
    errorOut = "Delete failed";
    return false;
  }
  return true;
}

} // namespace namplifier
