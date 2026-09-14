#include "PluginEditor.h"
#include "NamplifierUiData.h"
#include "dsp/CabDetect.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cstdint>

namespace
{
juce::String mimeFor (const juce::String& path)
{
  auto ext = path.fromLastOccurrenceOf (".", false, false).toLowerCase();
  if (ext == "html") return "text/html";
  if (ext == "js") return "text/javascript";
  if (ext == "css") return "text/css";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "json") return "application/json";
  if (ext == "woff2") return "font/woff2";
  return "application/octet-stream";
}

std::optional<juce::WebBrowserComponent::Resource> resourceFromBinaryData (const juce::String& path)
{
  const auto leaf = path.fromLastOccurrenceOf ("/", false, false);

  for (int i = 0; i < NamplifierUi::namedResourceListSize; ++i)
  {
    const auto* original = NamplifierUi::originalFilenames[i];
    if (original == nullptr)
      continue;

    const juce::String orig (original);
    if (path != orig && leaf != orig && ! path.endsWithIgnoreCase ("/" + orig))
      continue;

    int size = 0;
    if (auto* data = NamplifierUi::getNamedResource (NamplifierUi::namedResourceList[i], size))
    {
      if (size <= 0)
        continue;
      std::vector<std::byte> bytes ((const std::byte*) data, (const std::byte*) data + (size_t) size);
      return juce::WebBrowserComponent::Resource { std::move (bytes), mimeFor (path.isNotEmpty() ? path : orig) };
    }
  }
  return std::nullopt;
}

juce::String base64Url (const juce::MemoryBlock& mb)
{
  auto s = juce::Base64::toBase64 (mb.getData(), mb.getSize());
  return s.replaceCharacter ('+', '-')
          .replaceCharacter ('/', '_')
          .removeCharacters ("=");
}

juce::String sha256Base64Url (const juce::String& input)
{
  juce::SHA256 hash (input.toRawUTF8(), input.getNumBytesAsUTF8());
  auto hex = hash.toHexString();
  juce::MemoryBlock raw;
  raw.setSize (32, false);
  auto* bytes = static_cast<uint8_t*> (raw.getData());
  for (int i = 0; i < 32; ++i)
    bytes[i] = (uint8_t) hex.substring (i * 2, i * 2 + 2).getHexValue32();
  return base64Url (raw);
}

juce::File findUiDist()
{
  auto tryDir = [] (juce::File f) -> juce::File
  {
    if (f.getChildFile ("index.html").existsAsFile())
      return f;
    return {};
  };

  // Standalone: ui/dist next to the exe.
  // VST3: binary is inside Something.vst3/Contents/<arch>/ — also search up the
  // bundle and the install folder (release zip puts ui/dist beside the .vst3).
  auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
  auto dir = exe.getParentDirectory();
  for (int depth = 0; depth < 8 && dir.exists(); ++depth)
  {
    if (auto d = tryDir (dir.getChildFile ("ui/dist")); d.exists())
      return d;

    if (dir.getFileName().endsWithIgnoreCase (".vst3"))
    {
      if (auto d = tryDir (dir.getSiblingFile ("ui/dist")); d.exists())
        return d;
      if (auto d = tryDir (dir.getParentDirectory().getChildFile ("ui/dist")); d.exists())
        return d;
    }

    auto parent = dir.getParentDirectory();
    if (parent == dir)
      break;
    dir = parent;
  }

  for (auto f : {
         juce::File::getCurrentWorkingDirectory().getChildFile ("ui/dist"),
         juce::File::getCurrentWorkingDirectory().getChildFile ("../ui/dist")
       })
  {
    if (auto d = tryDir (f); d.exists())
      return d;
  }
  return {};
}
} // namespace

NamplifierAudioProcessorEditor::NamplifierAudioProcessorEditor (NamplifierAudioProcessor& p)
  : AudioProcessorEditor (&p), processor (p)
{
  {
    juce::PropertiesFile::Options opt;
    // Keep plugin masters separate from standalone audio-device settings.
    opt.applicationName = processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone
                            ? "Namplifier"
                            : "NamplifierPlugin";
    opt.filenameSuffix = ".settings";
    juce::PropertiesFile props (opt);
    if (props.containsKey ("masterInDb"))
      processor.getGraphEngine().setMasterInDb ((float) props.getDoubleValue ("masterInDb", 0.0));
    if (props.containsKey ("masterOutDb"))
      processor.getGraphEngine().setMasterOutDb ((float) props.getDoubleValue ("masterOutDb", -6.0));
    else if (props.containsKey ("masterDb"))
      processor.getGraphEngine().setMasterOutDb ((float) props.getDoubleValue ("masterDb", -6.0));
  }

  setSize (1280, 760);
  setResizable (true, true);
  setResizeLimits (960, 600, 3840, 2160);

  web = std::make_unique<juce::WebBrowserComponent> (makeOptions());
  addAndMakeVisible (*web);
  layoutWeb();
  web->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

  // WebView2 often stays blank until a layout pass after the HWND exists.
  startTimerHz (20);

  processor.getGraphEngine().onAsyncLoadFinished = [this]
  {
    if (web != nullptr)
      web->emitEventIfBrowserIsVisible ("graphChanged", juce::var());
  };

  processor.onGraphChanged = [this]
  {
    if (web != nullptr)
      web->emitEventIfBrowserIsVisible ("graphChanged", juce::var());
  };
}

NamplifierAudioProcessorEditor::~NamplifierAudioProcessorEditor()
{
  stopTimer();
  oauthLoopback.stop();
  processor.getGraphEngine().onAsyncLoadFinished = nullptr;
  processor.onGraphChanged = nullptr;
  web.reset();
}

void NamplifierAudioProcessorEditor::paint (juce::Graphics& g)
{
  g.fillAll (juce::Colour (0xff0e1114));
}

void NamplifierAudioProcessorEditor::layoutWeb()
{
  if (web != nullptr)
    web->setBounds (getLocalBounds());
}

void NamplifierAudioProcessorEditor::resized()
{
  layoutWeb();
  // WebView2 sometimes keeps a stale viewport after maximize/restore — nudge layout briefly.
  mLayoutPasses = 0;
  startTimerHz (20);
}

void NamplifierAudioProcessorEditor::visibilityChanged()
{
  if (isShowing())
  {
    layoutWeb();
    mLayoutPasses = 0;
    startTimerHz (20);
  }
}

void NamplifierAudioProcessorEditor::parentSizeChanged()
{
  layoutWeb();
  mLayoutPasses = 0;
  startTimerHz (20);
}

void NamplifierAudioProcessorEditor::timerCallback()
{
  layoutWeb();
  if (++mLayoutPasses >= 30) // ~1.5s of layout nudges
    stopTimer();
}

std::optional<juce::WebBrowserComponent::Resource>
NamplifierAudioProcessorEditor::getResource (const juce::String& url) const
{
  auto path = url.fromFirstOccurrenceOf ("http://localhost/", false, false);
  if (path.isEmpty())
    path = url;
  if (path == "/" || path.isEmpty())
    path = "index.html";
  path = path.upToFirstOccurrenceOf ("?", false, false);
  if (path.startsWithChar ('/'))
    path = path.substring (1);

  auto dist = findUiDist();
  if (dist.exists())
  {
    auto file = dist.getChildFile (path);
    if (file.existsAsFile())
    {
      juce::MemoryBlock mb;
      file.loadFileAsData (mb);
      std::vector<std::byte> data ((std::byte*) mb.getData(), (std::byte*) mb.getData() + mb.getSize());
      return juce::WebBrowserComponent::Resource { std::move (data), mimeFor (path) };
    }
  }

  // Embedded ui/dist (index + assets) — needed when the host loads the VST from a
  // folder that does not include the release zip's ui/dist sidecar.
  if (auto embedded = resourceFromBinaryData (path))
    return embedded;

  return std::nullopt;
}

juce::WebBrowserComponent::Options NamplifierAudioProcessorEditor::makeOptions()
{
  auto options = juce::WebBrowserComponent::Options{}
#if JUCE_WINDOWS
    .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
    .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                               .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                      .getChildFile ("NamplifierWebView")))
#endif
    .withNativeIntegrationEnabled()
    .withResourceProvider ([this] (const auto& url) { return getResource (url); });

  auto bind = [this] (const juce::String& name)
  {
    return [this, name] (const juce::Array<juce::var>& args, auto complete)
    {
      juce::var payload = args.isEmpty() ? juce::var() : args[0];
      complete (handleNativeCall (name, payload));
    };
  };

  options = options
    .withNativeFunction ("getGraph", bind ("getGraph"))
    .withNativeFunction ("setGraph", bind ("setGraph"))
    .withNativeFunction ("updateNode", bind ("updateNode"))
    .withNativeFunction ("loadFileOntoNode", bind ("loadFileOntoNode"))
    .withNativeFunction ("pickFileForNode", bind ("pickFileForNode"))
    .withNativeFunction ("getCpu", bind ("getCpu"))
    .withNativeFunction ("getIoInfo", bind ("getIoInfo"))
    .withNativeFunction ("getDspStatus", bind ("getDspStatus"))
    .withNativeFunction ("unmuteInput", bind ("unmuteInput"))
    .withNativeFunction ("toggleFullscreen", bind ("toggleFullscreen"))
    .withNativeFunction ("listPresets", bind ("listPresets"))
    .withNativeFunction ("savePreset", bind ("savePreset"))
    .withNativeFunction ("loadPreset", bind ("loadPreset"))
    .withNativeFunction ("deletePreset", bind ("deletePreset"))
    .withNativeFunction ("toneStatus", bind ("toneStatus"))
    .withNativeFunction ("toneBeginLogin", bind ("toneBeginLogin"))
    .withNativeFunction ("toneCompleteLogin", bind ("toneCompleteLogin"))
    .withNativeFunction ("toneLogout", bind ("toneLogout"))
    .withNativeFunction ("toneSearch", bind ("toneSearch"))
    .withNativeFunction ("toneFavorites", bind ("toneFavorites"))
    .withNativeFunction ("toneCreated", bind ("toneCreated"))
    .withNativeFunction ("toneDownloaded", bind ("toneDownloaded"))
    .withNativeFunction ("toneFavorite", bind ("toneFavorite"))
    .withNativeFunction ("toneGetModels", bind ("toneGetModels"))
    .withNativeFunction ("toneLoadToNode", bind ("toneLoadToNode"))
    .withNativeFunction ("toneSetNodeModel", bind ("toneSetNodeModel"))
    .withNativeFunction ("toneAddToLibrary", bind ("toneAddToLibrary"))
    .withNativeFunction ("toneAddToGraph", bind ("toneAddToGraph"))
    .withNativeFunction ("getLibrary", bind ("getLibrary"))
    .withNativeFunction ("libraryRemove", bind ("libraryRemove"))
    .withNativeFunction ("libraryAddLocal", bind ("libraryAddLocal"))
    .withNativeFunction ("libraryApplyToNode", bind ("libraryApplyToNode"))
    .withNativeFunction ("libraryAddToGraph", bind ("libraryAddToGraph"))
    .withNativeFunction ("openExternal", bind ("openExternal"))
    .withNativeFunction ("getMaster", bind ("getMaster"))
    .withNativeFunction ("setMaster", bind ("setMaster"));

  return options;
}

juce::String NamplifierAudioProcessorEditor::handleNativeCall (const juce::String& method, const juce::var& args)
{
  auto ok = [] (juce::var data = {})
  {
    auto* o = new juce::DynamicObject();
    o->setProperty ("ok", true);
    o->setProperty ("data", data);
    return juce::JSON::toString (juce::var (o));
  };
  auto fail = [] (const juce::String& err)
  {
    auto* o = new juce::DynamicObject();
    o->setProperty ("ok", false);
    o->setProperty ("error", err);
    return juce::JSON::toString (juce::var (o));
  };

  try
  {
    if (method == "getGraph")
      return ok (processor.getGraphEngine().getGraph().toVar());

    if (method == "setGraph")
    {
      auto doc = namplifier::GraphDocument::fromVar (args);
      processor.getGraphEngine().setGraph (doc);
      return ok();
    }

    if (method == "updateNode")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      auto id = o->getProperty ("id").toString();
      namplifier::NodeParams p;
      if (auto* params = o->getProperty ("params").getDynamicObject())
      {
        p.levelDb = (float) params->getProperty ("levelDb");
        p.mix = (float) params->getProperty ("mix");
        p.slim = params->hasProperty ("slim") ? (float) params->getProperty ("slim") : 1.0f;
        p.bypass = (bool) params->getProperty ("bypass");
        p.filePath = params->getProperty ("filePath").toString();
        p.displayName = params->getProperty ("displayName").toString();
        p.toneId = params->getProperty ("toneId").toString();
        p.modelId = params->getProperty ("modelId").toString();
        p.imageUrl = params->getProperty ("imageUrl").toString();
        p.cabIncluded = (bool) params->getProperty ("cabIncluded");
        p.inputChannel = (int) params->getProperty ("inputChannel");
        p.pan = params->hasProperty ("pan") ? (float) params->getProperty ("pan") : 0.0f;
        p.outputChannel = params->hasProperty ("outputChannel") ? (int) params->getProperty ("outputChannel") : 0;
        p.outputChannelR = params->hasProperty ("outputChannelR") ? (int) params->getProperty ("outputChannelR") : -1;
        p.bassDb = (float) params->getProperty ("bassDb");
        p.midDb = (float) params->getProperty ("midDb");
        p.trebleDb = (float) params->getProperty ("trebleDb");
        p.roomSize = params->hasProperty ("roomSize") ? (float) params->getProperty ("roomSize") : 0.45f;
        p.damping = params->hasProperty ("damping") ? (float) params->getProperty ("damping") : 0.4f;
        p.width = params->hasProperty ("width") ? (float) params->getProperty ("width") : 1.0f;
        p.delayMs = params->hasProperty ("delayMs") ? (float) params->getProperty ("delayMs") : 350.0f;
        p.feedback = params->hasProperty ("feedback") ? (float) params->getProperty ("feedback") : 0.35f;
        p.thresholdDb = params->hasProperty ("thresholdDb") ? (float) params->getProperty ("thresholdDb") : -50.0f;
        p.attackMs = params->hasProperty ("attackMs") ? (float) params->getProperty ("attackMs") : 2.0f;
        p.releaseMs = params->hasProperty ("releaseMs") ? (float) params->getProperty ("releaseMs") : 150.0f;
        p.gateRangeDb = params->hasProperty ("gateRangeDb") ? (float) params->getProperty ("gateRangeDb") : -70.0f;
        p.hysteresisDb = params->hasProperty ("hysteresisDb") ? (float) params->getProperty ("hysteresisDb") : 8.0f;
        p.holdMs = params->hasProperty ("holdMs") ? (float) params->getProperty ("holdMs") : 80.0f;
        p.ratio = params->hasProperty ("ratio") ? (float) params->getProperty ("ratio") : 4.0f;
        p.makeupDb = params->hasProperty ("makeupDb") ? (float) params->getProperty ("makeupDb") : 0.0f;
        p.rateHz = params->hasProperty ("rateHz") ? (float) params->getProperty ("rateHz") : 0.8f;
        p.depth = params->hasProperty ("depth") ? (float) params->getProperty ("depth") : 0.35f;
        p.fxMode = params->getProperty ("fxMode").toString();
        p.fxId = params->getProperty ("fxId").toString();
      }
      processor.getGraphEngine().updateNodeParams (id, p);
      return ok();
    }

    if (method == "loadFileOntoNode")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      processor.getGraphEngine().loadFileOntoNode (o->getProperty ("id").toString(),
                                                    juce::File (o->getProperty ("path").toString()));
      return ok (processor.getGraphEngine().getGraph().toVar());
    }

    if (method == "pickFileForNode")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      auto id = o->getProperty ("id").toString();
      auto type = o->getProperty ("type").toString();
      auto chooser = std::make_shared<juce::FileChooser> (
        "Load file", juce::File(), type == "ir" ? "*.wav" : "*.nam");
      chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                            [this, id, chooser] (const juce::FileChooser& fc)
                            {
                              auto f = fc.getResult();
                              if (f.existsAsFile())
                                processor.getGraphEngine().loadFileOntoNode (id, f);
                              if (web != nullptr)
                                web->emitEventIfBrowserIsVisible ("graphChanged", juce::var());
                            });
      return ok();
    }

    if (method == "getCpu")
      return ok (processor.getGraphEngine().getCpuLoad());

    if (method == "getIoInfo")
    {
      auto* o = new juce::DynamicObject();
      o->setProperty ("numInputs", processor.getTotalNumInputChannels());
      o->setProperty ("numOutputs", processor.getTotalNumOutputChannels());
      o->setProperty ("inputPeak", processor.getGraphEngine().getInputPeak());
      o->setProperty ("outputPeak", processor.getGraphEngine().getOutputPeak());
      o->setProperty ("inputPeakL", processor.getGraphEngine().getInputPeakL());
      o->setProperty ("inputPeakR", processor.getGraphEngine().getInputPeakR());
      o->setProperty ("outputPeakL", processor.getGraphEngine().getOutputPeakL());
      o->setProperty ("outputPeakR", processor.getGraphEngine().getOutputPeakR());
      o->setProperty ("activeInputChannel", processor.getGraphEngine().getActiveInputChannel());
      o->setProperty ("activeOutputChannel", processor.getGraphEngine().getActiveOutputChannel());
      o->setProperty ("masterDb", processor.getGraphEngine().getMasterOutDb());
      o->setProperty ("masterInDb", processor.getGraphEngine().getMasterInDb());
      o->setProperty ("masterOutDb", processor.getGraphEngine().getMasterOutDb());
      o->setProperty ("clipping", processor.getGraphEngine().wasClipping());
      const bool standalone = (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone);
      bool muted = false;
      if (standalone)
      {
        juce::PropertiesFile::Options opt;
        opt.applicationName = "Namplifier";
        opt.filenameSuffix = ".settings";
        juce::PropertiesFile props (opt);
        muted = props.getBoolValue ("shouldMuteInput", false);
      }
      o->setProperty ("inputMuted", muted);
      o->setProperty ("isStandalone", standalone);
     #ifdef NAMPLIFIER_BUILD_ID
      o->setProperty ("buildId", NAMPLIFIER_BUILD_ID);
     #endif
      return ok (juce::var (o));
    }

    if (method == "getMaster")
    {
      auto* o = new juce::DynamicObject();
      o->setProperty ("inputDb", processor.getGraphEngine().getMasterInDb());
      o->setProperty ("outputDb", processor.getGraphEngine().getMasterOutDb());
      o->setProperty ("clipping", processor.getGraphEngine().wasClipping());
      return ok (juce::var (o));
    }

    if (method == "setMaster")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      if (o->hasProperty ("inputDb"))
        processor.getGraphEngine().setMasterInDb ((float) o->getProperty ("inputDb"));
      if (o->hasProperty ("outputDb"))
        processor.getGraphEngine().setMasterOutDb ((float) o->getProperty ("outputDb"));
      // Legacy single-knob
      if (o->hasProperty ("db") && ! o->hasProperty ("outputDb"))
        processor.getGraphEngine().setMasterOutDb ((float) o->getProperty ("db"));
      {
        juce::PropertiesFile::Options opt;
        opt.applicationName = processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone
                                ? "Namplifier"
                                : "NamplifierPlugin";
        opt.filenameSuffix = ".settings";
        juce::PropertiesFile props (opt);
        props.setValue ("masterInDb", processor.getGraphEngine().getMasterInDb());
        props.setValue ("masterOutDb", processor.getGraphEngine().getMasterOutDb());
        props.setValue ("masterDb", processor.getGraphEngine().getMasterOutDb()); // legacy
        props.saveIfNeeded();
      }
      auto* out = new juce::DynamicObject();
      out->setProperty ("inputDb", processor.getGraphEngine().getMasterInDb());
      out->setProperty ("outputDb", processor.getGraphEngine().getMasterOutDb());
      return ok (juce::var (out));
    }

    if (method == "getDspStatus")
      return ok (processor.getGraphEngine().getDspStatus());

    if (method == "unmuteInput")
    {
      juce::PropertiesFile::Options opt;
      opt.applicationName = "Namplifier";
      opt.filenameSuffix = ".settings";
      juce::PropertiesFile props (opt);
      props.setValue ("shouldMuteInput", false);
      props.saveIfNeeded();
      return ok (juce::var ("restart"));
    }

    if (method == "toggleFullscreen")
    {
      if (auto* top = findParentComponentOfClass<juce::DocumentWindow>())
      {
        top->setFullScreen (! top->isFullScreen());
        return ok (top->isFullScreen());
      }
      if (auto* peer = getPeer())
      {
        // Fallback: maximize via peer bounds
        auto displays = juce::Desktop::getInstance().getDisplays();
        if (auto* main = displays.getPrimaryDisplay())
        {
          auto area = main->userArea;
          auto* tl = getTopLevelComponent();
          if (tl != nullptr)
          {
            if (tl->getBounds().getWidth() >= area.getWidth() - 8)
              tl->centreWithSize (1280, 760);
            else
              tl->setBounds (area);
            return ok (true);
          }
        }
      }
      return fail ("Fullscreen only in Standalone");
    }

    if (method == "listPresets")
    {
      juce::Array<juce::var> arr;
      for (auto& n : processor.getPresets().listPresets())
        arr.add (n);
      return ok (juce::var (arr));
    }

    if (method == "savePreset")
    {
      auto* o = args.getDynamicObject();
      juce::String err;
      auto name = o ? o->getProperty ("name").toString() : juce::String();
      if (! processor.getPresets().savePreset (name, processor.getGraphEngine().getGraph(), err))
        return fail (err);
      return ok();
    }

    if (method == "loadPreset")
    {
      auto* o = args.getDynamicObject();
      juce::String err;
      namplifier::GraphDocument doc;
      if (! processor.getPresets().loadPreset (o->getProperty ("name").toString(), doc, err))
        return fail (err);
      processor.getGraphEngine().setGraph (doc);
      return ok (doc.toVar());
    }

    if (method == "deletePreset")
    {
      auto* o = args.getDynamicObject();
      juce::String err;
      if (! processor.getPresets().deletePreset (o->getProperty ("name").toString(), err))
        return fail (err);
      return ok();
    }

    if (method == "toneStatus")
    {
      auto* o = new juce::DynamicObject();
      o->setProperty ("signedIn", processor.getTone3000().isSignedIn());
      o->setProperty ("hasKey", processor.getTone3000().getPublishableKey().isNotEmpty());
      return ok (juce::var (o));
    }

    if (method == "toneBeginLogin")
    {
      if (processor.getTone3000().getPublishableKey().isEmpty())
        return fail ("Set Namplifier_TONE3000_KEY CMake/env publishable key");

      mPkceVerifier = juce::Uuid().toDashedString().removeCharacters ("-")
                      + juce::Uuid().toDashedString().removeCharacters ("-");
      mPkceState = juce::Uuid().toDashedString();

      if (! oauthLoopback.start ([this] (juce::String code, juce::String state, juce::String error)
                                 {
                                   finishOAuth (code, state, error);
                                 }))
        return fail ("Could not start local OAuth listener");

      mOAuthRedirect = oauthLoopback.redirectUri();
      auto challenge = sha256Base64Url (mPkceVerifier);
      auto url = processor.getTone3000().buildAuthorizeUrl (mOAuthRedirect, mPkceState, challenge);
      auto* o = new juce::DynamicObject();
      o->setProperty ("url", url);
      o->setProperty ("state", mPkceState);
      o->setProperty ("redirectUri", mOAuthRedirect);
      o->setProperty ("waiting", true);
      juce::URL (url).launchInDefaultBrowser();
      return ok (juce::var (o));
    }

    if (method == "toneCompleteLogin")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      auto code = o->getProperty ("code").toString();
      auto state = o->getProperty ("state").toString();
      finishOAuth (code, state, {});
      if (! processor.getTone3000().isSignedIn())
        return fail ("Login failed");
      return ok();
    }

    if (method == "toneLogout")
    {
      processor.getTone3000().clearTokens();
      return ok();
    }

    if (method == "toneSearch")
    {
      auto* o = args.getDynamicObject();
      namplifier::ToneSearchParams params;
      params.query = o ? o->getProperty ("query").toString() : juce::String();
      params.format = o ? o->getProperty ("format").toString() : juce::String ("nam");
      params.architecture = o ? o->getProperty ("architecture").toString() : juce::String ("2");
      params.sort = o ? o->getProperty ("sort").toString() : juce::String();
      params.gears = o ? o->getProperty ("gears").toString() : juce::String();
      params.sizes = o ? o->getProperty ("sizes").toString() : juce::String();
      params.calibrated = o && (bool) o->getProperty ("calibrated");
      params.page = o ? (int) o->getProperty ("page") : 1;
      params.pageSize = o ? (int) o->getProperty ("pageSize") : 20;
      if (params.page <= 0) params.page = 1;
      if (params.pageSize <= 0) params.pageSize = 20;

      juce::Array<namplifier::ToneSummary> tones;
      juce::String err;
      if (! processor.getTone3000().searchTones (params, tones, err))
        return fail (err);

      juce::Array<juce::var> arr;
      for (auto& t : tones)
      {
        auto* x = new juce::DynamicObject();
        x->setProperty ("id", t.id);
        x->setProperty ("name", t.name);
        x->setProperty ("creator", t.creator);
        x->setProperty ("creatorAvatar", t.creatorAvatar);
        x->setProperty ("format", t.format);
        x->setProperty ("gear", t.gear);
        x->setProperty ("description", t.description);
        x->setProperty ("imageUrl", t.imageUrl);
        x->setProperty ("gearType", t.gearType);
        x->setProperty ("cabIncluded", t.cabIncluded);
        x->setProperty ("url", t.url);
        x->setProperty ("downloads", t.downloads);
        x->setProperty ("favorites", t.favorites);
        x->setProperty ("modelsCount", t.modelsCount);
        arr.add (juce::var (x));
      }
      return ok (juce::var (arr));
    }

    auto tonesToVar = [] (const juce::Array<namplifier::ToneSummary>& tones, bool markFavorited) -> juce::var
    {
      juce::Array<juce::var> arr;
      for (auto& t : tones)
      {
        auto* x = new juce::DynamicObject();
        x->setProperty ("id", t.id);
        x->setProperty ("name", t.name);
        x->setProperty ("creator", t.creator);
        x->setProperty ("creatorAvatar", t.creatorAvatar);
        x->setProperty ("format", t.format);
        x->setProperty ("gear", t.gear);
        x->setProperty ("description", t.description);
        x->setProperty ("imageUrl", t.imageUrl);
        x->setProperty ("gearType", t.gearType);
        x->setProperty ("cabIncluded", t.cabIncluded);
        x->setProperty ("url", t.url);
        x->setProperty ("downloads", t.downloads);
        x->setProperty ("favorites", t.favorites);
        x->setProperty ("modelsCount", t.modelsCount);
        if (markFavorited)
          x->setProperty ("favorited", true);
        arr.add (juce::var (x));
      }
      return juce::var (arr);
    };

    if (method == "toneFavorites")
    {
      auto* o = args.getDynamicObject();
      const int page = o ? juce::jmax (1, (int) o->getProperty ("page")) : 1;
      const int pageSize = o ? juce::jmax (1, (int) o->getProperty ("pageSize")) : 25;
      juce::Array<namplifier::ToneSummary> tones;
      juce::String err;
      if (! processor.getTone3000().getFavorited (page, pageSize, tones, err))
        return fail (err);
      return ok (tonesToVar (tones, true));
    }

    if (method == "toneCreated")
    {
      auto* o = args.getDynamicObject();
      const int page = o ? juce::jmax (1, (int) o->getProperty ("page")) : 1;
      const int pageSize = o ? juce::jmax (1, (int) o->getProperty ("pageSize")) : 25;
      juce::Array<namplifier::ToneSummary> tones;
      juce::String err;
      if (! processor.getTone3000().getCreated (page, pageSize, tones, err))
        return fail (err);
      return ok (tonesToVar (tones, false));
    }

    if (method == "toneDownloaded")
    {
      auto* o = args.getDynamicObject();
      const int page = o ? juce::jmax (1, (int) o->getProperty ("page")) : 1;
      const int pageSize = o ? juce::jmax (1, (int) o->getProperty ("pageSize")) : 25;
      juce::Array<namplifier::ToneSummary> tones;
      juce::String err;
      if (! processor.getTone3000().getDownloaded (page, pageSize, tones, err))
        return fail (err);
      return ok (tonesToVar (tones, false));
    }

    if (method == "toneFavorite")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      auto toneId = o->getProperty ("toneId").toString();
      const bool favorite = (bool) o->getProperty ("favorite");
      juce::String err;
      if (! processor.getTone3000().setFavorite (toneId, favorite, err))
        return fail (err);
      return ok();
    }

    if (method == "toneGetModels")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      auto toneId = o->getProperty ("toneId").toString();
      auto format = o->getProperty ("format").toString();
      juce::Array<namplifier::ModelSummary> models;
      juce::String err;
      const juce::String arch = format.equalsIgnoreCase ("ir") ? juce::String() : "2";
      if (! processor.getTone3000().getModelsForTone (toneId, arch, models, err))
        return fail (err);
      juce::Array<juce::var> arr;
      for (auto& m : models)
      {
        auto* x = new juce::DynamicObject();
        x->setProperty ("id", m.id);
        x->setProperty ("name", m.name);
        x->setProperty ("architecture", m.architecture);
        x->setProperty ("size", m.size);
        arr.add (juce::var (x));
      }
      return ok (juce::var (arr));
    }

    auto pickModel = [] (const juce::Array<namplifier::ModelSummary>& models,
                         const juce::String& modelId) -> namplifier::ModelSummary
    {
      if (modelId.isNotEmpty())
        for (auto& m : models)
          if (m.id == modelId)
            return m;
      namplifier::ModelSummary chosen = models[0];
      for (auto& m : models)
        if (m.architecture == "2" || m.architecture.contains ("2") || m.name.containsIgnoreCase ("a2"))
        {
          chosen = m;
          break;
        }
      return chosen;
    };

    auto ensureLibraryItem = [&] (const juce::String& toneId,
                                  const juce::String& toneName,
                                  const juce::String& format,
                                  const juce::String& imageUrl,
                                  bool cabFromUi,
                                  const namplifier::ModelSummary& chosen,
                                  const juce::File& file,
                                  juce::String& err) -> namplifier::LibraryItem
    {
      // Reuse existing library row for same tone+model
      for (auto& existing : processor.getLibrary().getItems())
        if (existing.toneId == toneId && existing.modelId == chosen.id)
          return existing;

      namplifier::LibraryItem item;
      item.id = juce::Uuid().toDashedString();
      const bool isIr = format.equalsIgnoreCase ("ir") || file.hasFileExtension (".wav");
      item.kind = isIr ? namplifier::LibraryItemKind::Ir : namplifier::LibraryItemKind::NamProfile;
      item.name = toneName.isNotEmpty() ? toneName : file.getFileNameWithoutExtension();
      item.filePath = file.getFullPathName();
      item.toneId = toneId;
      item.modelId = chosen.id;
      item.format = isIr ? "ir" : "nam";
      item.source = "tone3000";
      item.imageUrl = imageUrl;
      item.cabIncluded = ! isIr
                           && (cabFromUi || namplifier::detectCabIncludedFromNamFile (file)
                               || namplifier::textSuggestsCabIncluded (item.name));
      if (item.cabIncluded)
        item.notes = "Cab included";
      if (! processor.getLibrary().addItem (item, err))
        return {};
      return item;
    };

    if (method == "toneLoadToNode" || method == "toneSetNodeModel")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      auto nodeId = o->getProperty ("nodeId").toString();
      auto toneId = o->getProperty ("toneId").toString();
      auto format = o->getProperty ("format").toString();
      auto modelId = o->getProperty ("modelId").toString();
      auto toneName = o->getProperty ("name").toString();
      auto imageUrl = o->getProperty ("imageUrl").toString();
      const bool cabFromUi = (bool) o->getProperty ("cabIncluded");

      juce::Array<namplifier::ModelSummary> models;
      juce::String err;
      const juce::String arch = format.equalsIgnoreCase ("ir") ? juce::String() : "2";
      if (! processor.getTone3000().getModelsForTone (toneId, arch, models, err))
        return fail (err);
      if (models.isEmpty())
        return fail ("No models for tone");

      auto chosen = pickModel (models, modelId);
      auto file = processor.getTone3000().downloadModelToCache (chosen, err);
      if (! file.existsAsFile())
        return fail (err);

      // Keep Library in sync when loading from Tone3000
      (void) ensureLibraryItem (toneId, toneName, format, imageUrl, cabFromUi, chosen, file, err);

      processor.getGraphEngine().loadFileOntoNode (nodeId, file);
      auto graph = processor.getGraphEngine().getGraph();
      for (auto& n : graph.nodes)
      {
        if (n.id == nodeId)
        {
          if (toneName.isNotEmpty())
            n.params.displayName = toneName;
          else if (chosen.name.isNotEmpty())
            n.params.displayName = chosen.name;
          else if (n.params.displayName.isEmpty())
            n.params.displayName = file.getFileNameWithoutExtension();
          n.params.toneId = toneId;
          n.params.modelId = chosen.id;
          n.params.filePath = file.getFullPathName();
          if (imageUrl.isNotEmpty())
            n.params.imageUrl = imageUrl;
          n.params.cabIncluded = cabFromUi || namplifier::detectCabIncludedFromNamFile (file)
                                 || namplifier::textSuggestsCabIncluded (n.params.displayName);
          processor.getGraphEngine().updateNodeParams (nodeId, n.params);
          break;
        }
      }
      auto* root = new juce::DynamicObject();
      root->setProperty ("graph", processor.getGraphEngine().getGraph().toVar());
      root->setProperty ("library", processor.getLibrary().getItemsVar());
      return ok (juce::var (root));
    }

    if (method == "toneAddToLibrary")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      auto toneId = o->getProperty ("toneId").toString();
      auto toneName = o->getProperty ("name").toString();
      auto format = o->getProperty ("format").toString();
      auto imageUrl = o->getProperty ("imageUrl").toString();
      auto modelId = o->getProperty ("modelId").toString();
      const bool cabFromUi = (bool) o->getProperty ("cabIncluded");
      juce::Array<namplifier::ModelSummary> models;
      juce::String err;
      const juce::String arch = format.equalsIgnoreCase ("ir") ? juce::String() : "2";
      if (! processor.getTone3000().getModelsForTone (toneId, arch, models, err))
        return fail (err);
      if (models.isEmpty())
        return fail ("No models for tone (try another architecture or re-search)");

      auto chosen = pickModel (models, modelId);
      auto file = processor.getTone3000().downloadModelToCache (chosen, err);
      if (! file.existsAsFile())
        return fail (err);

      auto item = ensureLibraryItem (toneId, toneName, format, imageUrl, cabFromUi, chosen, file, err);
      if (item.id.isEmpty())
        return fail (err.isNotEmpty() ? err : "Failed to add to library");
      return ok (processor.getLibrary().getItemsVar());
    }

    if (method == "toneAddToGraph")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      auto toneId = o->getProperty ("toneId").toString();
      auto toneName = o->getProperty ("name").toString();
      auto format = o->getProperty ("format").toString();
      auto imageUrl = o->getProperty ("imageUrl").toString();
      auto modelId = o->getProperty ("modelId").toString();
      const bool cabFromUi = (bool) o->getProperty ("cabIncluded");
      const float dropX = o->hasProperty ("x") ? (float) o->getProperty ("x") : -1.0e6f;
      const float dropY = o->hasProperty ("y") ? (float) o->getProperty ("y") : -1.0e6f;

      juce::Array<namplifier::ModelSummary> models;
      juce::String err;
      const juce::String arch = format.equalsIgnoreCase ("ir") ? juce::String() : "2";
      if (! processor.getTone3000().getModelsForTone (toneId, arch, models, err))
        return fail (err);
      if (models.isEmpty())
        return fail ("No models for tone");

      auto chosen = pickModel (models, modelId);
      auto file = processor.getTone3000().downloadModelToCache (chosen, err);
      if (! file.existsAsFile())
        return fail (err);

      auto item = ensureLibraryItem (toneId, toneName, format, imageUrl, cabFromUi, chosen, file, err);
      if (item.id.isEmpty())
        return fail (err.isNotEmpty() ? err : "Failed to add to library");

      // Reuse libraryAddToGraph path by building node directly
      auto doc = processor.getGraphEngine().getGraph();
      namplifier::GraphNode node;
      node.id = juce::Uuid().toDashedString();
      node.x = dropX > -1.0e5f ? dropX : (240.0f + (float) doc.nodes.size() * 12.0f);
      node.y = dropY > -1.0e5f ? dropY : 180.0f;
      node.params.displayName = item.name;
      node.params.filePath = item.filePath;
      node.params.toneId = item.toneId;
      node.params.modelId = item.modelId;
      node.params.imageUrl = item.imageUrl;
      node.params.mix = 1.0f;
      node.params.slim = 1.0f;
      node.params.cabIncluded = item.cabIncluded;
      node.type = item.kind == namplifier::LibraryItemKind::Ir ? namplifier::NodeType::Ir
                                                              : namplifier::NodeType::Nam;
      doc.nodes.push_back (node);
      processor.getGraphEngine().setGraph (doc);
      processor.getGraphEngine().loadFileOntoNode (node.id, juce::File (item.filePath));
      // Restore display metadata after load
      {
        auto g2 = processor.getGraphEngine().getGraph();
        for (auto& n : g2.nodes)
          if (n.id == node.id)
          {
            n.params.displayName = item.name;
            n.params.imageUrl = item.imageUrl;
            n.params.toneId = item.toneId;
            n.params.modelId = item.modelId;
            n.params.cabIncluded = item.cabIncluded;
            processor.getGraphEngine().updateNodeParams (node.id, n.params);
            break;
          }
      }
      auto* root = new juce::DynamicObject();
      root->setProperty ("graph", processor.getGraphEngine().getGraph().toVar());
      root->setProperty ("library", processor.getLibrary().getItemsVar());
      return ok (juce::var (root));
    }

    if (method == "getLibrary")
      return ok (processor.getLibrary().getItemsVar());

    if (method == "libraryRemove")
    {
      auto* o = args.getDynamicObject();
      juce::String err;
      if (! processor.getLibrary().removeItem (o->getProperty ("id").toString(), err))
        return fail (err);
      return ok (processor.getLibrary().getItemsVar());
    }

    if (method == "libraryAddLocal")
    {
      auto kind = args.getDynamicObject() ? args.getDynamicObject()->getProperty ("kind").toString() : "nam";
      auto chooser = std::make_shared<juce::FileChooser> (
        "Add to library", juce::File(), kind == "ir" ? "*.wav" : "*.nam");
      chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                            [this, chooser, kind] (const juce::FileChooser& fc)
                            {
                              auto f = fc.getResult();
                              if (! f.existsAsFile())
                                return;
                              namplifier::LibraryItem item;
                              item.id = juce::Uuid().toDashedString();
                              item.kind = kind == "ir" ? namplifier::LibraryItemKind::Ir
                                                       : namplifier::LibraryItemKind::NamProfile;
                              item.name = f.getFileNameWithoutExtension();
                              item.filePath = f.getFullPathName();
                              item.format = kind == "ir" ? "ir" : "nam";
                              item.source = "local";
                              juce::String err;
                              processor.getLibrary().addItem (item, err);
                              if (web != nullptr)
                                web->emitEventIfBrowserIsVisible ("libraryChanged", juce::var());
                            });
      return ok();
    }

    if (method == "libraryApplyToNode")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      auto nodeId = o->getProperty ("nodeId").toString();
      auto libId = o->getProperty ("libraryId").toString();
      namplifier::LibraryItem found;
      bool okFind = false;
      for (auto& item : processor.getLibrary().getItems())
        if (item.id == libId)
        {
          found = item;
          okFind = true;
          break;
        }
      if (! okFind) return fail ("Library item not found");
      if (found.kind == namplifier::LibraryItemKind::Fx)
      {
        // Allow applying reverb onto an empty fx node later; for now reject apply-to-selection for FX
        return fail ("Drag Reverb or Delay onto the graph (not onto an IR/NAM block)");
      }
      if (found.kind == namplifier::LibraryItemKind::NamProfile)
      {
        // Find node type
        auto graph = processor.getGraphEngine().getGraph();
        for (auto& n : graph.nodes)
          if (n.id == nodeId)
          {
            if (n.type == namplifier::NodeType::Ir)
              return fail ("Can't load an amp into an IR cab node — drop it on the graph or a NAM block");
            if (n.type != namplifier::NodeType::Nam)
              return fail ("Amps only load into NAM nodes");
            break;
          }
      }
      if (found.kind == namplifier::LibraryItemKind::Ir)
      {
        auto graph = processor.getGraphEngine().getGraph();
        for (auto& n : graph.nodes)
          if (n.id == nodeId)
          {
            if (n.type == namplifier::NodeType::Nam)
              return fail ("Can't load an IR into a NAM amp node");
            if (n.type != namplifier::NodeType::Ir)
              return fail ("IRs only load into IR nodes");
            break;
          }
      }
      if (! juce::File (found.filePath).existsAsFile() && found.kind != namplifier::LibraryItemKind::Fx)
        return fail ("Cached file missing");

      processor.getGraphEngine().loadFileOntoNode (nodeId, juce::File (found.filePath));
      auto graph = processor.getGraphEngine().getGraph();
      for (auto& n : graph.nodes)
      {
        if (n.id == nodeId)
        {
          n.params.displayName = found.name;
          n.params.filePath = found.filePath;
          n.params.toneId = found.toneId;
          n.params.modelId = found.modelId;
          n.params.imageUrl = found.imageUrl;
          n.params.cabIncluded = found.cabIncluded
                                 || namplifier::detectCabIncludedFromNamFile (juce::File (found.filePath));
          if (n.params.mix <= 0.0f)
            n.params.mix = 1.0f;
          if (n.params.slim <= 0.0f)
            n.params.slim = 1.0f;
          processor.getGraphEngine().updateNodeParams (nodeId, n.params);
          break;
        }
      }
      return ok (processor.getGraphEngine().getGraph().toVar());
    }

    if (method == "libraryAddToGraph")
    {
      auto* o = args.getDynamicObject();
      if (o == nullptr) return fail ("bad args");
      auto libId = o->getProperty ("libraryId").toString();
      const float dropX = o->hasProperty ("x") ? (float) o->getProperty ("x") : -1.0e6f;
      const float dropY = o->hasProperty ("y") ? (float) o->getProperty ("y") : -1.0e6f;
      namplifier::LibraryItem found;
      bool okFind = false;
      for (auto& item : processor.getLibrary().getItems())
        if (item.id == libId)
        {
          found = item;
          okFind = true;
          break;
        }
      if (! okFind) return fail ("Library item not found");

      auto doc = processor.getGraphEngine().getGraph();
      namplifier::GraphNode node;
      node.id = juce::Uuid().toDashedString();
      node.x = dropX > -1.0e5f ? dropX : (240.0f + (float) doc.nodes.size() * 12.0f);
      node.y = dropY > -1.0e5f ? dropY : 180.0f;
      node.params.displayName = found.name;
      node.params.filePath = found.filePath;
      node.params.toneId = found.toneId;
      node.params.modelId = found.modelId;
      node.params.imageUrl = found.imageUrl;
      node.params.mix = 1.0f;
      node.params.slim = 1.0f;

      if (found.kind == namplifier::LibraryItemKind::Fx)
      {
        const bool isReverb = found.format.equalsIgnoreCase ("reverb")
                              || found.name.equalsIgnoreCase ("reverb");
        const bool isPingPong = found.format.equalsIgnoreCase ("pingpong")
                                || found.name.containsIgnoreCase ("ping");
        const bool isDelay = (! isPingPong)
                             && (found.format.equalsIgnoreCase ("delay")
                                 || found.name.equalsIgnoreCase ("delay"));
        const bool isGate = found.format.equalsIgnoreCase ("gate")
                            || found.name.containsIgnoreCase ("gate");
        const bool isChorus = found.format.equalsIgnoreCase ("chorus")
                              || found.name.containsIgnoreCase ("chorus");
        const bool isComp = found.format.equalsIgnoreCase ("compressor")
                            || found.format.equalsIgnoreCase ("comp")
                            || found.name.containsIgnoreCase ("compress");
        if (! isReverb && ! isDelay && ! isPingPong && ! isGate && ! isChorus && ! isComp)
          return fail ("Unknown FX type");
        node.type = namplifier::NodeType::Fx;
        if (isPingPong)
        {
          node.params.fxId = "pingpong";
          node.params.mix = 0.35f;
          node.params.delayMs = 380.0f;
          node.params.feedback = 0.4f;
          node.params.fxMode = "digital";
          node.params.displayName = "Ping Pong Delay";
        }
        else if (isDelay)
        {
          node.params.fxId = "delay";
          node.params.mix = 0.35f;
          node.params.delayMs = 350.0f;
          node.params.feedback = 0.35f;
          node.params.fxMode = "digital";
          node.params.displayName = "Delay";
        }
        else if (isGate)
        {
          node.params.fxId = "gate";
          node.params.mix = 1.0f;
          node.params.thresholdDb = -50.0f;
          node.params.attackMs = 2.0f;
          node.params.releaseMs = 150.0f;
          node.params.gateRangeDb = -70.0f;
          node.params.hysteresisDb = 8.0f;
          node.params.holdMs = 80.0f;
          node.params.displayName = "Noise Gate";
        }
        else if (isChorus)
        {
          node.params.fxId = "chorus";
          node.params.mix = 0.4f;
          node.params.rateHz = 0.8f;
          node.params.depth = 0.35f;
          node.params.feedback = 0.12f;
          node.params.displayName = "Chorus";
        }
        else if (isComp)
        {
          node.params.fxId = "compressor";
          node.params.mix = 1.0f;
          node.params.thresholdDb = -18.0f;
          node.params.ratio = 4.0f;
          node.params.attackMs = 10.0f;
          node.params.releaseMs = 100.0f;
          node.params.makeupDb = 2.0f;
          node.params.displayName = "Compressor";
        }
        else
        {
          node.params.fxId = "reverb";
          node.params.mix = 0.35f;
          node.params.roomSize = 0.45f;
          node.params.damping = 0.4f;
          node.params.width = 1.0f;
          node.params.fxMode = "room";
          node.params.displayName = "Reverb";
        }
      }
      else if (found.kind == namplifier::LibraryItemKind::Routing)
      {
        const auto fmt = found.format.toLowerCase();
        if (fmt == "input")
        {
          node.type = namplifier::NodeType::Input;
          node.params.displayName = "Input";
        }
        else if (fmt == "output")
        {
          node.type = namplifier::NodeType::Output;
          node.params.displayName = "Output";
          node.params.pan = 0.0f;
          node.params.outputChannel = 0;
          node.params.outputChannelR = 1;
          node.params.fxId = {};
        }
        else if (fmt == "host_output")
        {
          node.type = namplifier::NodeType::Output;
          node.params.displayName = "Host Output";
          node.params.fxId = "host_out";
          node.params.pan = 0.0f;
          node.params.outputChannel = 0;
          node.params.outputChannelR = 1;
        }
        else if (fmt == "split")
        {
          node.type = namplifier::NodeType::Split;
          node.params.displayName = "Split";
          node.params.mix = 0.5f;
        }
        else if (fmt == "merge")
        {
          node.type = namplifier::NodeType::Merge;
          node.params.displayName = "Merge";
          node.params.mix = 0.5f;
        }
        else
          return fail ("Unknown routing block");
      }
      else if (found.kind == namplifier::LibraryItemKind::Ir)
      {
        node.type = namplifier::NodeType::Ir;
      }
      else
      {
        node.type = namplifier::NodeType::Nam;
        node.params.cabIncluded = found.cabIncluded
                                  || namplifier::detectCabIncludedFromNamFile (juce::File (found.filePath));
      }

      doc.nodes.push_back (node);
      processor.getGraphEngine().setGraph (doc);
      if ((node.type == namplifier::NodeType::Nam || node.type == namplifier::NodeType::Ir)
          && juce::File (found.filePath).existsAsFile())
        processor.getGraphEngine().loadFileOntoNode (node.id, juce::File (found.filePath));
      return ok (processor.getGraphEngine().getGraph().toVar());
    }

    if (method == "openExternal")
    {
      auto url = args.toString();
      if (auto* o = args.getDynamicObject())
        url = o->getProperty ("url").toString();
      juce::URL (url).launchInDefaultBrowser();
      return ok();
    }
  }
  catch (const std::exception& e)
  {
    return fail (e.what());
  }

  return fail ("unknown method " + method);
}

void NamplifierAudioProcessorEditor::finishOAuth (const juce::String& code,
                                                   const juce::String& state,
                                                   const juce::String& error)
{
  oauthLoopback.stop();

  auto* payload = new juce::DynamicObject();
  if (error.isNotEmpty())
  {
    payload->setProperty ("ok", false);
    payload->setProperty ("error", error);
    if (web != nullptr)
      web->emitEventIfBrowserIsVisible ("toneAuthFinished", juce::var (payload));
    return;
  }

  if (state != mPkceState)
  {
    payload->setProperty ("ok", false);
    payload->setProperty ("error", "state mismatch");
    if (web != nullptr)
      web->emitEventIfBrowserIsVisible ("toneAuthFinished", juce::var (payload));
    return;
  }

  if (code.isEmpty())
  {
    payload->setProperty ("ok", false);
    payload->setProperty ("error", "missing code");
    if (web != nullptr)
      web->emitEventIfBrowserIsVisible ("toneAuthFinished", juce::var (payload));
    return;
  }

  juce::String err;
  const bool okLogin = processor.getTone3000().exchangeCode (code, mPkceVerifier, mOAuthRedirect, err);
  payload->setProperty ("ok", okLogin);
  if (! okLogin)
    payload->setProperty ("error", err);
  if (web != nullptr)
    web->emitEventIfBrowserIsVisible ("toneAuthFinished", juce::var (payload));
}
