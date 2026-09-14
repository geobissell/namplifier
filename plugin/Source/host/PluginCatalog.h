#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <functional>
#include <mutex>

namespace namplifier
{

/** Scanned VST3 catalog + custom search folders (persisted under AppData). */
class PluginCatalog
{
public:
  PluginCatalog();
  ~PluginCatalog();

  juce::AudioPluginFormatManager& getFormatManager() { return mFormats; }
  const juce::KnownPluginList& getList() const { return mList; }

  juce::StringArray getFolders() const;
  void setFolders (const juce::StringArray& folders);
  void addFolder (const juce::File& folder);
  void removeFolder (const juce::String& path);

  /** Blocking scan of configured folders (call off the message thread). */
  void scanFolders();
  bool isScanning() const { return mScanning.load(); }
  juce::String getScanStatus() const;
  int getPluginCount() const;

  juce::var getPluginsVar (const juce::String& query = {}) const;
  juce::PluginDescription findByUid (const juce::String& uid) const;
  juce::PluginDescription findByFileAndUid (const juce::String& file, const juce::String& uid);

  std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription& desc,
                                                             double sampleRate,
                                                             int blockSize,
                                                             juce::String& errorOut);

  using InstanceCallback = std::function<void (std::unique_ptr<juce::AudioPluginInstance>,
                                               const juce::String& /*error*/)>;
  void createInstanceAsync (const juce::PluginDescription& desc,
                            double sampleRate,
                            int blockSize,
                            InstanceCallback callback);

  void load();
  void save() const;

  static juce::File settingsFile();
  static juce::StringArray defaultFolders();

private:
  void ensureFormats();
  void setStatus (const juce::String& s);

  juce::AudioPluginFormatManager mFormats;
  juce::KnownPluginList mList;
  juce::StringArray mFolders;
  mutable std::mutex mMutex;
  std::atomic<bool> mScanning { false };
  juce::String mStatus;
};

} // namespace namplifier
