#include "LibraryManager.h"
#include "../dsp/CabDetect.h"

namespace namplifier
{

juce::String libraryKindToString (LibraryItemKind k)
{
  switch (k)
  {
    case LibraryItemKind::NamProfile: return "nam";
    case LibraryItemKind::Ir: return "ir";
    case LibraryItemKind::Fx: return "fx";
    case LibraryItemKind::Routing: return "routing";
    case LibraryItemKind::Folder: return "folder";
  }
  return "nam";
}

LibraryItemKind libraryKindFromString (const juce::String& s)
{
  if (s == "ir") return LibraryItemKind::Ir;
  if (s == "fx") return LibraryItemKind::Fx;
  if (s == "routing") return LibraryItemKind::Routing;
  if (s == "folder") return LibraryItemKind::Folder;
  return LibraryItemKind::NamProfile;
}

juce::var LibraryItem::toVar() const
{
  auto* o = new juce::DynamicObject();
  o->setProperty ("id", id);
  o->setProperty ("kind", libraryKindToString (kind));
  o->setProperty ("name", name);
  o->setProperty ("filePath", filePath);
  o->setProperty ("toneId", toneId);
  o->setProperty ("modelId", modelId);
  o->setProperty ("format", format);
  o->setProperty ("source", source);
  o->setProperty ("notes", notes);
  o->setProperty ("imageUrl", imageUrl);
  o->setProperty ("cabIncluded", cabIncluded);
  return juce::var (o);
}

LibraryItem LibraryItem::fromVar (const juce::var& v)
{
  LibraryItem item;
  if (auto* o = v.getDynamicObject())
  {
    item.id = o->getProperty ("id").toString();
    item.kind = libraryKindFromString (o->getProperty ("kind").toString());
    item.name = o->getProperty ("name").toString();
    item.filePath = o->getProperty ("filePath").toString();
    item.toneId = o->getProperty ("toneId").toString();
    item.modelId = o->getProperty ("modelId").toString();
    item.format = o->getProperty ("format").toString();
    item.source = o->getProperty ("source").toString();
    item.notes = o->getProperty ("notes").toString();
    item.imageUrl = o->getProperty ("imageUrl").toString();
    item.cabIncluded = (bool) o->getProperty ("cabIncluded");
  }
  return item;
}

LibraryManager::LibraryManager()
{
  reload();
  ensureSeed();
}

juce::File LibraryManager::getLibraryFile() const
{
  auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("Namplifier");
  dir.createDirectory();
  return dir.getChildFile ("library.json");
}

void LibraryManager::reload()
{
  mItems.clear();
  auto f = getLibraryFile();
  if (! f.existsAsFile())
    return;
  auto parsed = juce::JSON::parse (f.loadFileAsString());
  if (auto* arr = parsed.getArray())
  {
    for (auto& v : *arr)
      mItems.add (LibraryItem::fromVar (v));
  }
  else if (auto* o = parsed.getDynamicObject())
  {
    if (auto* arr2 = o->getProperty ("items").getArray())
      for (auto& v : *arr2)
        mItems.add (LibraryItem::fromVar (v));
  }

  bool dirty = false;
  for (auto& item : mItems)
  {
    if (item.kind == LibraryItemKind::NamProfile && ! item.cabIncluded
        && juce::File (item.filePath).existsAsFile())
    {
      if (detectCabIncludedFromNamFile (juce::File (item.filePath))
          || textSuggestsCabIncluded (item.name))
      {
        item.cabIncluded = true;
        if (item.notes.isEmpty())
          item.notes = "Cab included";
        dirty = true;
      }
    }
  }
  if (dirty)
    save();
}

void LibraryManager::save()
{
  juce::Array<juce::var> arr;
  for (auto& i : mItems)
    arr.add (i.toVar());
  auto* root = new juce::DynamicObject();
  root->setProperty ("version", 1);
  root->setProperty ("items", juce::var (arr));
  getLibraryFile().replaceWithText (juce::JSON::toString (juce::var (root), true));
}

void LibraryManager::ensureSeed()
{
  bool hasReverb = false;
  bool hasDelay = false;
  bool hasPingPong = false;
  bool hasGate = false;
  bool hasChorus = false;
  bool hasCompressor = false;
  bool dirty = false;
  for (auto& i : mItems)
  {
    if (i.kind != LibraryItemKind::Fx)
      continue;

    // Drop placeholder / unused FX
    if (i.name.containsIgnoreCase ("flanger")
        || i.name.containsIgnoreCase ("(soon)")
        || (i.name.containsIgnoreCase ("chorus") && i.notes.containsIgnoreCase ("coming")))
    {
      continue; // handled in purge pass below
    }

    if (i.name.containsIgnoreCase ("reverb"))
    {
      if (i.format != "reverb" || i.name.containsIgnoreCase ("soon"))
      {
        i.name = "Reverb";
        i.format = "reverb";
        i.notes = "Room / plate / spring";
        i.source = i.source.isEmpty() ? "factory" : i.source;
        dirty = true;
      }
      hasReverb = true;
    }
    else if (i.name.containsIgnoreCase ("ping"))
    {
      if (i.format != "pingpong")
      {
        i.name = "Ping Pong Delay";
        i.format = "pingpong";
        i.notes = "Repeats bounce L↔R";
        i.source = i.source.isEmpty() ? "factory" : i.source;
        dirty = true;
      }
      hasPingPong = true;
    }
    else if (i.name.containsIgnoreCase ("delay"))
    {
      if (i.format != "delay" || i.name.containsIgnoreCase ("soon") || i.notes.containsIgnoreCase ("ping-pong"))
      {
        i.name = "Delay";
        i.format = "delay";
        i.notes = "Digital / analogue / tape";
        i.source = i.source.isEmpty() ? "factory" : i.source;
        dirty = true;
      }
      hasDelay = true;
    }
    else if (i.name.containsIgnoreCase ("gate"))
    {
      if (i.format != "gate" || i.name.containsIgnoreCase ("soon"))
      {
        i.name = "Noise Gate";
        i.format = "gate";
        i.notes = "Cut noise between notes";
        i.source = i.source.isEmpty() ? "factory" : i.source;
        dirty = true;
      }
      hasGate = true;
    }
    else if (i.name.containsIgnoreCase ("chorus"))
    {
      i.name = "Chorus";
      i.format = "chorus";
      i.notes = "Stereo chorus";
      i.source = i.source.isEmpty() ? "factory" : i.source;
      hasChorus = true;
      dirty = true;
    }
    else if (i.name.containsIgnoreCase ("compress"))
    {
      i.name = "Compressor";
      i.format = "compressor";
      i.notes = "Stereo compressor";
      i.source = i.source.isEmpty() ? "factory" : i.source;
      hasCompressor = true;
      dirty = true;
    }
  }

  // Purge flanger / soon placeholders
  for (int idx = mItems.size(); --idx >= 0;)
  {
    auto& i = mItems.getReference (idx);
    if (i.kind != LibraryItemKind::Fx)
      continue;
    if (i.name.containsIgnoreCase ("flanger") || i.name.containsIgnoreCase ("(soon)"))
    {
      mItems.remove (idx);
      dirty = true;
    }
  }

  if (! hasReverb)
  {
    LibraryItem reverb;
    reverb.id = juce::Uuid().toDashedString();
    reverb.kind = LibraryItemKind::Fx;
    reverb.name = "Reverb";
    reverb.format = "reverb";
    reverb.source = "factory";
    reverb.notes = "Room / plate / spring";
    mItems.add (reverb);
    dirty = true;
  }

  if (! hasDelay)
  {
    LibraryItem delay;
    delay.id = juce::Uuid().toDashedString();
    delay.kind = LibraryItemKind::Fx;
    delay.name = "Delay";
    delay.format = "delay";
    delay.source = "factory";
    delay.notes = "Digital / analogue / tape";
    mItems.add (delay);
    dirty = true;
  }

  if (! hasPingPong)
  {
    LibraryItem pp;
    pp.id = juce::Uuid().toDashedString();
    pp.kind = LibraryItemKind::Fx;
    pp.name = "Ping Pong Delay";
    pp.format = "pingpong";
    pp.source = "factory";
    pp.notes = "Repeats bounce L↔R";
    mItems.add (pp);
    dirty = true;
  }

  if (! hasGate)
  {
    LibraryItem gate;
    gate.id = juce::Uuid().toDashedString();
    gate.kind = LibraryItemKind::Fx;
    gate.name = "Noise Gate";
    gate.format = "gate";
    gate.source = "factory";
    gate.notes = "Cut noise between notes";
    mItems.add (gate);
    dirty = true;
  }

  if (! hasChorus)
  {
    LibraryItem chorus;
    chorus.id = juce::Uuid().toDashedString();
    chorus.kind = LibraryItemKind::Fx;
    chorus.name = "Chorus";
    chorus.format = "chorus";
    chorus.source = "factory";
    chorus.notes = "Stereo chorus";
    mItems.add (chorus);
    dirty = true;
  }

  if (! hasCompressor)
  {
    LibraryItem comp;
    comp.id = juce::Uuid().toDashedString();
    comp.kind = LibraryItemKind::Fx;
    comp.name = "Compressor";
    comp.format = "compressor";
    comp.source = "factory";
    comp.notes = "Stereo compressor";
    mItems.add (comp);
    dirty = true;
  }

  const struct
  {
    const char* name;
    const char* format;
    const char* notes;
  } routingSeed[] = {
    { "Input", "input", "Audio input — pick host channel in properties" },
    { "Split", "split", "One in → A/B outs for dual-amp / parallel paths" },
    { "Merge", "merge", "Blend two paths back together" },
    { "Output", "output", "Main stereo out (channels 1–2)" },
    { "Host Output", "host_output", "Route to any interface outs (standalone)" },
  };

  for (auto& seed : routingSeed)
  {
    bool found = false;
    for (auto& i : mItems)
      if (i.kind == LibraryItemKind::Routing && i.format.equalsIgnoreCase (seed.format))
      {
        found = true;
        break;
      }
    if (found)
      continue;
    LibraryItem item;
    item.id = juce::Uuid().toDashedString();
    item.kind = LibraryItemKind::Routing;
    item.name = seed.name;
    item.format = seed.format;
    item.source = "factory";
    item.notes = seed.notes;
    mItems.add (item);
    dirty = true;
  }

  if (dirty)
    save();
}

juce::Array<LibraryItem> LibraryManager::getItems() const
{
  return mItems;
}

juce::var LibraryManager::getItemsVar() const
{
  juce::Array<juce::var> arr;
  for (auto& i : mItems)
    arr.add (i.toVar());
  return juce::var (arr);
}

bool LibraryManager::addItem (const LibraryItem& item, juce::String& errorOut)
{
  if (item.name.isEmpty())
  {
    errorOut = "Name required";
    return false;
  }

  // Dedupe by toneId+filePath when present
  for (auto& existing : mItems)
  {
    if (item.toneId.isNotEmpty() && existing.toneId == item.toneId)
    {
      errorOut = "Already in library";
      return false;
    }
    if (item.filePath.isNotEmpty() && existing.filePath == item.filePath
        && existing.kind == item.kind)
    {
      errorOut = "Already in library";
      return false;
    }
  }

  auto copy = item;
  if (copy.id.isEmpty())
    copy.id = juce::Uuid().toDashedString();
  mItems.add (copy);
  save();
  return true;
}

bool LibraryManager::removeItem (const juce::String& id, juce::String& errorOut)
{
  for (int i = 0; i < mItems.size(); ++i)
  {
    if (mItems[i].id == id)
    {
      if (mItems[i].source == "factory"
          && (mItems[i].kind == LibraryItemKind::Fx || mItems[i].kind == LibraryItemKind::Routing))
      {
        errorOut = "Cannot remove factory library items";
        return false;
      }
      mItems.remove (i);
      save();
      return true;
    }
  }
  errorOut = "Not found";
  return false;
}

bool LibraryManager::renameItem (const juce::String& id, const juce::String& name, juce::String& errorOut)
{
  for (auto& item : mItems)
  {
    if (item.id == id)
    {
      item.name = name;
      save();
      return true;
    }
  }
  errorOut = "Not found";
  return false;
}

} // namespace namplifier
