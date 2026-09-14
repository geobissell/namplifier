#pragma once

#include <juce_core/juce_core.h>

namespace namplifier
{

enum class LibraryItemKind
{
  NamProfile,
  Ir,
  Fx, // reserved for future non-NAM effects
  Routing, // input / split / merge / output
  Folder
};

struct LibraryItem
{
  juce::String id;
  LibraryItemKind kind = LibraryItemKind::NamProfile;
  juce::String name;
  juce::String filePath;
  juce::String toneId;
  juce::String modelId;
  juce::String format;   // nam / ir / fx-id
  juce::String source;   // tone3000 / local / factory
  juce::String notes;
  juce::String imageUrl;
  bool cabIncluded = false;

  juce::var toVar() const;
  static LibraryItem fromVar (const juce::var& v);
};

class LibraryManager
{
public:
  LibraryManager();

  juce::File getLibraryFile() const;
  juce::Array<LibraryItem> getItems() const;
  juce::var getItemsVar() const;

  bool addItem (const LibraryItem& item, juce::String& errorOut);
  bool removeItem (const juce::String& id, juce::String& errorOut);
  bool renameItem (const juce::String& id, const juce::String& name, juce::String& errorOut);
  void reload();
  void ensureSeed();

private:
  void save();
  juce::Array<LibraryItem> mItems;
};

juce::String libraryKindToString (LibraryItemKind k);
LibraryItemKind libraryKindFromString (const juce::String& s);

} // namespace namplifier
