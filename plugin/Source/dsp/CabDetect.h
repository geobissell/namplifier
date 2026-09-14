#pragma once

#include <juce_core/juce_core.h>

namespace namplifier
{

inline bool textSuggestsCabIncluded (const juce::String& text)
{
  const auto t = text.toLowerCase();
  if (t.contains ("amp_cab") || t.contains ("amp+cab") || t.contains ("amp + cab"))
    return true;
  if (t.contains ("with cab") || t.contains ("w/ cab") || t.contains ("cab included"))
    return true;
  // Tone3000 / pack naming often embeds CAB when the capture includes a cab
  if (t.contains (" cab ") || t.endsWithIgnoreCase (" cab") || t.contains ("-cab") || t.contains ("_cab"))
    return true;
  if (t.contains ("cab free") || t.contains ("fat cab")) // e.g. "FAT CAB FREE" still means cab-in-amp pack
    return true;
  return false;
}

inline bool detectCabIncludedFromNamFile (const juce::File& file)
{
  if (! file.existsAsFile() || ! file.hasFileExtension (".nam"))
    return false;

  auto parsed = juce::JSON::parse (file);
  auto* root = parsed.getDynamicObject();
  if (root == nullptr)
    return false;

  if (auto* meta = root->getProperty ("metadata").getDynamicObject())
  {
    const auto gearType = meta->getProperty ("gear_type").toString();
    if (gearType.equalsIgnoreCase ("amp_cab") || gearType.equalsIgnoreCase ("amp+cab"))
      return true;
    if (textSuggestsCabIncluded (meta->getProperty ("name").toString()))
      return true;
    if (textSuggestsCabIncluded (meta->getProperty ("gear_make").toString() + " "
                                 + meta->getProperty ("gear_model").toString()))
      return true;
  }

  return textSuggestsCabIncluded (file.getFileNameWithoutExtension());
}

inline bool detectCabIncludedFromTone (const juce::String& name,
                                       const juce::String& gear,
                                       const juce::String& gearType,
                                       const juce::String& description)
{
  if (gearType.equalsIgnoreCase ("amp_cab") || gearType.equalsIgnoreCase ("amp+cab"))
    return true;
  return textSuggestsCabIncluded (name) || textSuggestsCabIncluded (gear)
         || textSuggestsCabIncluded (description);
}

} // namespace namplifier
