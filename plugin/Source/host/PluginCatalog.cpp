#include "PluginCatalog.h"

namespace namplifier
{

namespace
{
int mainBusChannels (const juce::AudioProcessor::BusesLayout& layout, bool input)
{
  const auto& set = input ? layout.getMainInputChannelSet() : layout.getMainOutputChannelSet();
  if (set.isDisabled())
    return 0;
  return juce::jlimit (0, 64, set.size());
}
} // namespace

PluginCatalog::PluginCatalog()
{
  ensureFormats();
  mFolders = defaultFolders();
  load();
}

PluginCatalog::~PluginCatalog() = default;

void PluginCatalog::ensureFormats()
{
  if (mFormats.getNumFormats() == 0)
    mFormats.addDefaultFormats();
}

juce::File PluginCatalog::settingsFile()
{
  return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
    .getChildFile ("Namplifier")
    .getChildFile ("plugin_catalog.xml");
}

juce::StringArray PluginCatalog::defaultFolders()
{
  juce::StringArray dirs;
#if JUCE_WINDOWS
  dirs.add ("C:\\Program Files\\Common Files\\VST3");
  dirs.add ("C:\\Program Files\\VST3");
#elif JUCE_MAC
  dirs.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
              .getChildFile ("Library/Audio/Plug-Ins/VST3")
              .getFullPathName());
  dirs.add ("/Library/Audio/Plug-Ins/VST3");
#else
  dirs.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
              .getChildFile (".vst3")
              .getFullPathName());
  dirs.add ("/usr/lib/vst3");
  dirs.add ("/usr/local/lib/vst3");
#endif
  return dirs;
}

void PluginCatalog::setStatus (const juce::String& s)
{
  std::lock_guard lock (mMutex);
  mStatus = s;
}

juce::String PluginCatalog::getScanStatus() const
{
  std::lock_guard lock (mMutex);
  return mStatus;
}

int PluginCatalog::getPluginCount() const
{
  std::lock_guard lock (mMutex);
  return mList.getNumTypes();
}

juce::StringArray PluginCatalog::getFolders() const
{
  std::lock_guard lock (mMutex);
  return mFolders;
}

void PluginCatalog::setFolders (const juce::StringArray& folders)
{
  {
    std::lock_guard lock (mMutex);
    mFolders = folders;
    mFolders.removeEmptyStrings();
    mFolders.removeDuplicates (false);
  }
  save();
}

void PluginCatalog::addFolder (const juce::File& folder)
{
  if (! folder.isDirectory())
    return;
  {
    std::lock_guard lock (mMutex);
    mFolders.addIfNotAlreadyThere (folder.getFullPathName());
  }
  save();
}

void PluginCatalog::removeFolder (const juce::String& path)
{
  {
    std::lock_guard lock (mMutex);
    mFolders.removeString (path);
  }
  save();
}

void PluginCatalog::load()
{
  auto file = settingsFile();
  if (! file.existsAsFile())
    return;

  if (auto xml = juce::parseXML (file))
  {
    std::lock_guard lock (mMutex);
    if (auto* folders = xml->getChildByName ("FOLDERS"))
    {
      mFolders.clear();
      for (auto* c : folders->getChildWithTagNameIterator ("FOLDER"))
        mFolders.addIfNotAlreadyThere (c->getStringAttribute ("path"));
    }
    if (auto* list = xml->getChildByName ("KNOWNPLUGINS"))
      mList.recreateFromXml (*list);
  }
}

void PluginCatalog::save() const
{
  auto root = std::make_unique<juce::XmlElement> ("NAMPLIFIER_PLUGIN_CATALOG");
  auto* folders = root->createNewChildElement ("FOLDERS");
  {
    std::lock_guard lock (mMutex);
    for (auto& f : mFolders)
      folders->createNewChildElement ("FOLDER")->setAttribute ("path", f);
    if (auto listXml = mList.createXml())
      root->addChildElement (listXml.release());
  }

  auto file = settingsFile();
  file.getParentDirectory().createDirectory();
  root->writeTo (file);
}

void PluginCatalog::scanFolders()
{
  if (mScanning.exchange (true))
    return;

  setStatus ("Scanning…");
  ensureFormats();

  juce::StringArray foldersCopy;
  {
    std::lock_guard lock (mMutex);
    foldersCopy = mFolders;
    mList.clear();
  }

  juce::FileSearchPath path;
  for (auto& f : foldersCopy)
  {
    juce::File dir (f);
    if (dir.isDirectory())
      path.add (dir);
  }

  for (int fi = 0; fi < mFormats.getNumFormats(); ++fi)
  {
    auto* format = mFormats.getFormat (fi);
    if (format == nullptr)
      continue;
    // Prefer VST3; skip VST2 if present.
    if (! format->getName().containsIgnoreCase ("VST3")
#if JUCE_PLUGINHOST_AU
        && ! format->getName().containsIgnoreCase ("AudioUnit")
#endif
    )
      continue;

    juce::PluginDirectoryScanner scanner (mList, *format, path, true, juce::File(), false);
    juce::String name;
    while (scanner.scanNextFile (true, name))
    {
      if (name.containsIgnoreCase ("Namplifier"))
        continue;
      setStatus ("Scanning: " + name);
    }
  }

  // Drop our own plugin if it slipped in.
  {
    std::lock_guard lock (mMutex);
    auto types = mList.getTypes();
    for (int i = types.size(); --i >= 0;)
    {
      const auto& desc = types.getReference (i);
      if (desc.name.containsIgnoreCase ("Namplifier")
          || desc.fileOrIdentifier.containsIgnoreCase ("Namplifier"))
        mList.removeType (desc);
    }
  }

  save();
  setStatus ("Found " + juce::String (getPluginCount()) + " plugins");
  mScanning.store (false);
}

juce::var PluginCatalog::getPluginsVar (const juce::String& query) const
{
  const auto q = query.trim().toLowerCase();
  juce::Array<juce::var> arr;
  std::lock_guard lock (mMutex);
  for (auto& type : mList.getTypes())
  {
    if (q.isNotEmpty())
    {
      const auto hay = (type.name + " " + type.manufacturerName + " " + type.descriptiveName).toLowerCase();
      if (! hay.contains (q))
        continue;
    }

    // Probe channel counts from description when available (often 0 until loaded).
    auto* o = new juce::DynamicObject();
    o->setProperty ("uid", type.createIdentifierString());
    o->setProperty ("name", type.name);
    o->setProperty ("manufacturer", type.manufacturerName);
    o->setProperty ("format", type.pluginFormatName);
    o->setProperty ("path", type.fileOrIdentifier);
    o->setProperty ("category", type.category);
    o->setProperty ("isInstrument", type.isInstrument);
    o->setProperty ("numInputs", type.numInputChannels);
    o->setProperty ("numOutputs", type.numOutputChannels);
    arr.add (juce::var (o));
  }
  return juce::var (arr);
}

juce::PluginDescription PluginCatalog::findByUid (const juce::String& uid) const
{
  std::lock_guard lock (mMutex);
  for (auto& type : mList.getTypes())
    if (type.createIdentifierString() == uid)
      return type;
  return {};
}

juce::PluginDescription PluginCatalog::findByFileAndUid (const juce::String& file, const juce::String& uid)
{
  auto byUid = findByUid (uid);
  if (byUid.name.isNotEmpty())
    return byUid;

  if (file.isEmpty())
    return {};

  ensureFormats();
  juce::OwnedArray<juce::PluginDescription> found;
  for (int i = 0; i < mFormats.getNumFormats(); ++i)
  {
    if (auto* format = mFormats.getFormat (i))
      format->findAllTypesForFile (found, file);
  }
  for (auto* d : found)
  {
    if (d == nullptr)
      continue;
    if (uid.isEmpty() || d->createIdentifierString() == uid || juce::String (d->uniqueId) == uid)
      return *d;
  }
  if (found.size() > 0 && found[0] != nullptr)
    return *found[0];
  return {};
}

std::unique_ptr<juce::AudioPluginInstance> PluginCatalog::createInstance (const juce::PluginDescription& desc,
                                                                          double sampleRate,
                                                                          int blockSize,
                                                                          juce::String& errorOut)
{
  ensureFormats();
  errorOut.clear();
  if (desc.name.isEmpty() && desc.fileOrIdentifier.isEmpty())
  {
    errorOut = "No plugin selected";
    return {};
  }
  auto instance = mFormats.createPluginInstance (desc, sampleRate > 0.0 ? sampleRate : 44100.0,
                                                 blockSize > 0 ? blockSize : 512, errorOut);
  juce::ignoreUnused (mainBusChannels);
  return instance;
}

void PluginCatalog::createInstanceAsync (const juce::PluginDescription& desc,
                                         double sampleRate,
                                         int blockSize,
                                         InstanceCallback callback)
{
  ensureFormats();
  if (callback == nullptr)
    return;

  if (desc.name.isEmpty() && desc.fileOrIdentifier.isEmpty())
  {
    callback (nullptr, "No plugin selected");
    return;
  }

  mFormats.createPluginInstanceAsync (desc,
                                      sampleRate > 0.0 ? sampleRate : 44100.0,
                                      blockSize > 0 ? blockSize : 512,
                                      [cb = std::move (callback)] (std::unique_ptr<juce::AudioPluginInstance> inst,
                                                                   const juce::String& err)
                                      {
                                        cb (std::move (inst), err);
                                      });
}

} // namespace namplifier
