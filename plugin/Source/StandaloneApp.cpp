#include <JuceHeader.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

/** Standalone window with Windows-style min / fullscreen / close title-bar buttons. */
class NamplifierStandaloneWindow final : public juce::StandaloneFilterWindow
{
public:
  NamplifierStandaloneWindow (const juce::String& title,
                              juce::Colour backgroundColour,
                              std::unique_ptr<juce::StandalonePluginHolder> pluginHolderIn)
    : juce::StandaloneFilterWindow (title, backgroundColour, std::move (pluginHolderIn))
  {
   #if ! (JUCE_IOS || JUCE_ANDROID)
    // Parent only enables minimise + close; add the maximise slot (toggles fullscreen).
    setTitleBarButtonsRequired (juce::DocumentWindow::minimiseButton
                                  | juce::DocumentWindow::maximiseButton
                                  | juce::DocumentWindow::closeButton,
                                false);
   #endif
  }
};

class NamplifierStandaloneApp final : public juce::JUCEApplication
{
public:
  NamplifierStandaloneApp()
  {
    juce::PropertiesFile::Options options;
    options.applicationName = juce::CharPointer_UTF8 (JucePlugin_Name);
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = {};
    appProperties.setStorageParameters (options);

    if (auto* settings = appProperties.getUserSettings())
    {
      settings->setValue ("shouldMuteInput", false);

      // Restoring filterState while ASIO opens has caused hard BEX crashes (0xC0000409).
      // Pull it out of settings for holder construction, then re-apply after the window is up.
      deferredFilterState = settings->getValue ("filterState");
      if (deferredFilterState.isNotEmpty())
        settings->removeValue ("filterState");

      // Avoid 0,0 which can leave the window unusable after a bad maximize/restore.
      if (settings->getIntValue ("windowX", 100) <= 0)
        settings->setValue ("windowX", 120);
      if (settings->getIntValue ("windowY", 100) <= 0)
        settings->setValue ("windowY", 80);
    }
  }

  const juce::String getApplicationName() override { return juce::CharPointer_UTF8 (JucePlugin_Name); }
  const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
  bool moreThanOneInstanceAllowed() override { return true; }
  void anotherInstanceStarted (const juce::String&) override {}

  juce::StandaloneFilterWindow* createWindow()
  {
    if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
      return nullptr;

    return new NamplifierStandaloneWindow (
      getApplicationName(),
      juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
      createPluginHolder());
  }

  std::unique_ptr<juce::StandalonePluginHolder> createPluginHolder()
  {
    // Prefer multi-channel I/O so Host Output can address interface outs.
    juce::Array<juce::StandalonePluginHolder::PluginInOuts> channelConfig;
    channelConfig.add ({ 8, 8 });
    return std::make_unique<juce::StandalonePluginHolder> (appProperties.getUserSettings(),
                                                           false,
                                                           juce::String{},
                                                           nullptr,
                                                           channelConfig,
                                                           false);
  }

  void applyDeferredFilterState()
  {
    if (deferredFilterState.isEmpty())
      return;

    auto* holder = mainWindow != nullptr ? mainWindow->pluginHolder.get() : pluginHolder.get();
    if (holder == nullptr)
      return;

    if (auto* proc = holder->processor.get())
    {
      juce::MemoryOutputStream mos;
      // JUCE stores MemoryBlock as its own base64 encoding in PropertiesFile.
      juce::MemoryBlock mb;
      if (mb.fromBase64Encoding (deferredFilterState) && mb.getSize() > 0)
        proc->setStateInformation (mb.getData(), (int) mb.getSize());
    }

    if (auto* settings = appProperties.getUserSettings())
      settings->setValue ("filterState", deferredFilterState);

    deferredFilterState.clear();
  }

  void initialise (const juce::String&) override
  {
    mainWindow.reset (createWindow());
    if (mainWindow != nullptr)
    {
      if (mainWindow->pluginHolder != nullptr)
        mainWindow->pluginHolder->getMuteInputValue() = false;
      mainWindow->setVisible (true);
    }
    else
    {
      pluginHolder = createPluginHolder();
      if (pluginHolder != nullptr)
        pluginHolder->getMuteInputValue() = false;
    }

    // Let audio device + WebView finish coming up before restoring the graph.
    juce::Timer::callAfterDelay (400, [this]
    {
      applyDeferredFilterState();
    });
  }

  void shutdown() override
  {
    pluginHolder = nullptr;
    mainWindow = nullptr;
    appProperties.saveIfNeeded();
  }

  void systemRequestedQuit() override
  {
    if (pluginHolder != nullptr)
      pluginHolder->savePluginState();
    if (mainWindow != nullptr && mainWindow->pluginHolder != nullptr)
      mainWindow->pluginHolder->savePluginState();

    if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
    {
      juce::Timer::callAfterDelay (100, []
      {
        if (auto* app = juce::JUCEApplicationBase::getInstance())
          app->systemRequestedQuit();
      });
    }
    else
    {
      quit();
    }
  }

private:
  juce::ApplicationProperties appProperties;
  std::unique_ptr<juce::StandaloneFilterWindow> mainWindow;
  std::unique_ptr<juce::StandalonePluginHolder> pluginHolder;
  juce::String deferredFilterState;
};

juce::JUCEApplicationBase* juce_CreateApplication()
{
  return new NamplifierStandaloneApp();
}
