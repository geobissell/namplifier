#include "Tone3000Client.h"
#include "../dsp/CabDetect.h"

namespace namplifier
{

Tone3000Client::Tone3000Client()
{
  mPublishableKey = NAMPLIFIER_TONE3000_KEY;
  loadTokensFromDisk();
}

void Tone3000Client::setPublishableKey (const juce::String& key)
{
  mPublishableKey = key;
}

juce::File Tone3000Client::getCacheDir() const
{
  auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Namplifier")
               .getChildFile ("cache");
  dir.createDirectory();
  return dir;
}

juce::File Tone3000Client::getTokenFile() const
{
  auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Namplifier");
  dir.createDirectory();
  return dir.getChildFile ("tone3000_tokens.json");
}

void Tone3000Client::loadTokensFromDisk()
{
  auto f = getTokenFile();
  if (! f.existsAsFile())
    return;
  auto v = juce::JSON::parse (f);
  if (auto* o = v.getDynamicObject())
  {
    mAccessToken = o->getProperty ("access_token").toString();
    mRefreshToken = o->getProperty ("refresh_token").toString();
    mExpiresAtMs = (juce::int64) o->getProperty ("expires_at");
  }
}

void Tone3000Client::saveTokens()
{
  auto* o = new juce::DynamicObject();
  o->setProperty ("access_token", mAccessToken);
  o->setProperty ("refresh_token", mRefreshToken);
  o->setProperty ("expires_at", mExpiresAtMs);
  getTokenFile().replaceWithText (juce::JSON::toString (juce::var (o), true));
}

void Tone3000Client::clearTokens()
{
  mAccessToken.clear();
  mRefreshToken.clear();
  mExpiresAtMs = 0;
  getTokenFile().deleteFile();
}

bool Tone3000Client::isSignedIn() const
{
  return mAccessToken.isNotEmpty();
}

juce::String Tone3000Client::buildAuthorizeUrl (const juce::String& redirectUri,
                                                const juce::String& state,
                                                const juce::String& codeChallenge,
                                                const juce::String& prompt)
{
  juce::URL url ("https://www.tone3000.com/api/v1/oauth/authorize");
  url = url.withParameter ("client_id", mPublishableKey)
           .withParameter ("redirect_uri", redirectUri)
           .withParameter ("response_type", "code")
           .withParameter ("code_challenge", codeChallenge)
           .withParameter ("code_challenge_method", "S256")
           .withParameter ("state", state)
           .withParameter ("menubar", "true");
  if (prompt.isNotEmpty())
    url = url.withParameter ("prompt", prompt);
  return url.toString (true);
}

bool Tone3000Client::exchangeCode (const juce::String& code,
                                   const juce::String& codeVerifier,
                                   const juce::String& redirectUri,
                                   juce::String& errorOut)
{
  auto body = "grant_type=authorization_code"
              "&code=" + juce::URL::addEscapeChars (code, true)
              + "&code_verifier=" + juce::URL::addEscapeChars (codeVerifier, true)
              + "&redirect_uri=" + juce::URL::addEscapeChars (redirectUri, true)
              + "&client_id=" + juce::URL::addEscapeChars (mPublishableKey, true);

  juce::URL postUrl ("https://www.tone3000.com/api/v1/oauth/token");
  postUrl = postUrl.withPOSTData (body);
  std::unique_ptr<juce::InputStream> stream (
    postUrl.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                 .withConnectionTimeoutMs (15000)
                                 .withExtraHeaders ("Content-Type: application/x-www-form-urlencoded\n")
                                 .withHttpRequestCmd ("POST")));

  if (stream == nullptr)
  {
    errorOut = "Token exchange failed (network)";
    return false;
  }
  auto text = stream->readEntireStreamAsString();
  auto v = juce::JSON::parse (text);
  auto* o = v.getDynamicObject();
  if (o == nullptr || ! o->hasProperty ("access_token"))
  {
    errorOut = "Token exchange failed: " + text;
    return false;
  }
  mAccessToken = o->getProperty ("access_token").toString();
  mRefreshToken = o->getProperty ("refresh_token").toString();
  const int expiresIn = (int) o->getProperty ("expires_in");
  mExpiresAtMs = juce::Time::currentTimeMillis() + (juce::int64) expiresIn * 1000;
  saveTokens();
  return true;
}

bool Tone3000Client::refreshIfNeeded (juce::String& errorOut)
{
  if (mAccessToken.isEmpty())
  {
    errorOut = "Not signed in";
    return false;
  }
  if (juce::Time::currentTimeMillis() < mExpiresAtMs - 30000)
    return true;
  if (mRefreshToken.isEmpty())
  {
    errorOut = "Refresh token missing";
    return false;
  }

  auto body = "grant_type=refresh_token"
              "&refresh_token=" + juce::URL::addEscapeChars (mRefreshToken, true)
              + "&client_id=" + juce::URL::addEscapeChars (mPublishableKey, true);
  juce::URL postUrl ("https://www.tone3000.com/api/v1/oauth/token");
  postUrl = postUrl.withPOSTData (body);
  std::unique_ptr<juce::InputStream> stream (
    postUrl.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                 .withConnectionTimeoutMs (15000)
                                 .withExtraHeaders ("Content-Type: application/x-www-form-urlencoded\n")
                                 .withHttpRequestCmd ("POST")));
  if (stream == nullptr)
  {
    errorOut = "Refresh failed";
    return false;
  }
  auto text = stream->readEntireStreamAsString();
  auto v = juce::JSON::parse (text);
  auto* o = v.getDynamicObject();
  if (o == nullptr || ! o->hasProperty ("access_token"))
  {
    clearTokens();
    errorOut = "Refresh invalid; sign in again";
    return false;
  }
  mAccessToken = o->getProperty ("access_token").toString();
  if (o->hasProperty ("refresh_token"))
    mRefreshToken = o->getProperty ("refresh_token").toString();
  mExpiresAtMs = juce::Time::currentTimeMillis() + (juce::int64) (int) o->getProperty ("expires_in") * 1000;
  saveTokens();
  return true;
}

bool Tone3000Client::apiGet (const juce::String& path, juce::var& result, juce::String& errorOut)
{
  return apiRequest ("GET", path, result, errorOut);
}

bool Tone3000Client::apiRequest (const juce::String& method,
                                 const juce::String& path,
                                 juce::var& result,
                                 juce::String& errorOut)
{
  if (! refreshIfNeeded (errorOut))
    return false;

  juce::URL url ("https://www.tone3000.com" + path);
  auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs (15000)
                .withHttpRequestCmd (method)
                .withExtraHeaders ("Authorization: Bearer " + mAccessToken + "\n"
                                  "Content-Type: application/json\n");
  std::unique_ptr<juce::InputStream> stream (url.createInputStream (opts));
  if (stream == nullptr)
  {
    errorOut = method + " failed: " + path;
    return false;
  }
  auto text = stream->readEntireStreamAsString().trim();
  if (text.isEmpty())
  {
    result = juce::var();
    return true; // e.g. 204 No Content
  }
  result = juce::JSON::parse (text);
  if (result.isVoid() && ! method.equalsIgnoreCase ("DELETE"))
  {
    errorOut = "Bad JSON";
    return false;
  }
  return true;
}

bool Tone3000Client::setFavorite (const juce::String& toneId, bool favorite, juce::String& errorOut)
{
  if (toneId.isEmpty())
  {
    errorOut = "Missing tone id";
    return false;
  }
  juce::var result;
  const auto path = "/api/v1/tones/" + toneId + "/favorite";
  return apiRequest (favorite ? "PUT" : "DELETE", path, result, errorOut);
}

juce::String Tone3000Client::firstImageUrl (const juce::var& imagesVar)
{
  if (auto* arr = imagesVar.getArray())
  {
    if (! arr->isEmpty())
      return (*arr)[0].toString();
  }
  return imagesVar.toString();
}

juce::Array<ToneSummary> Tone3000Client::parseToneArray (const juce::var& data)
{
  juce::Array<ToneSummary> out;
  const juce::Array<juce::var>* arr = nullptr;
  if (auto* o = data.getDynamicObject())
    arr = o->getProperty ("data").getArray();
  else
    arr = data.getArray();

  if (arr == nullptr)
    return out;

  for (auto& item : *arr)
  {
    if (auto* t = item.getDynamicObject())
    {
      ToneSummary s;
      s.id = t->getProperty ("id").toString();
      if (s.id.isEmpty())
        s.id = juce::String ((int) t->getProperty ("id"));

      // API field is `title` (not name)
      s.name = t->getProperty ("title").toString();
      if (s.name.isEmpty())
        s.name = t->getProperty ("name").toString();

      s.description = t->getProperty ("description").toString();
      s.format = t->getProperty ("format").toString();
      if (s.format.isEmpty())
        s.format = t->getProperty ("platform").toString();
      s.gear = t->getProperty ("gear").toString();
      s.gearType = t->getProperty ("gear_type").toString();
      if (s.gearType.isEmpty())
        s.gearType = t->getProperty ("tone_type").toString();
      s.url = t->getProperty ("url").toString();
      s.downloads = (int) t->getProperty ("downloads_count");
      s.favorites = (int) t->getProperty ("favorites_count");
      s.modelsCount = (int) t->getProperty ("models_count");
      s.imageUrl = firstImageUrl (t->getProperty ("images"));
      if (s.imageUrl.isEmpty())
        s.imageUrl = t->getProperty ("image_url").toString();

      if (auto* user = t->getProperty ("user").getDynamicObject())
      {
        s.creator = user->getProperty ("username").toString();
        s.creatorAvatar = user->getProperty ("avatar_url").toString();
      }

      s.cabIncluded = detectCabIncludedFromTone (s.name, s.gear, s.gearType, s.description);

      if (s.name.isEmpty())
      {
        juce::String makeName;
        if (auto* makes = t->getProperty ("makes").getArray(); makes != nullptr && ! makes->isEmpty())
          if (auto* m0 = (*makes)[0].getDynamicObject())
            makeName = m0->getProperty ("name").toString();
        s.name = makeName.isNotEmpty() ? makeName : (s.creator.isNotEmpty() ? s.creator + " tone" : "Untitled");
      }

      out.add (s);
    }
  }
  return out;
}

bool Tone3000Client::searchTones (const ToneSearchParams& params,
                                  juce::Array<ToneSummary>& out,
                                  juce::String& errorOut)
{
  const int page = juce::jmax (1, params.page);
  const int pageSize = juce::jlimit (1, 25, params.pageSize > 0 ? params.pageSize : 20);

  juce::String sort = params.sort.trim();
  if (sort.isEmpty())
    sort = params.query.isNotEmpty() ? "best-match" : "trending";

  juce::String path = "/api/v1/tones/search?page=" + juce::String (page)
                      + "&page_size=" + juce::String (pageSize)
                      + "&sort=" + juce::URL::addEscapeChars (sort, true);

  if (params.query.isNotEmpty())
    path += "&query=" + juce::URL::addEscapeChars (params.query, true);
  if (params.format.isNotEmpty())
    path += "&format=" + juce::URL::addEscapeChars (params.format, true);
  if (params.architecture.isNotEmpty())
    path += "&architecture=" + juce::URL::addEscapeChars (params.architecture, true);
  if (params.gears.isNotEmpty())
    path += "&gears=" + juce::URL::addEscapeChars (params.gears, true);
  if (params.sizes.isNotEmpty())
    path += "&sizes=" + juce::URL::addEscapeChars (params.sizes, true);
  if (params.calibrated)
    path += "&calibrated=true";

  return listTonesPath (path, out, errorOut);
}

bool Tone3000Client::listTonesPath (const juce::String& path,
                                    juce::Array<ToneSummary>& out,
                                    juce::String& errorOut)
{
  juce::var result;
  if (! apiGet (path, result, errorOut))
    return false;
  out = parseToneArray (result);
  return true;
}

bool Tone3000Client::getFavorited (int page, int pageSize, juce::Array<ToneSummary>& out, juce::String& errorOut)
{
  const int p = juce::jmax (1, page);
  const int ps = juce::jlimit (1, 50, pageSize > 0 ? pageSize : 25);
  return listTonesPath ("/api/v1/tones/favorited?page=" + juce::String (p)
                          + "&page_size=" + juce::String (ps),
                        out, errorOut);
}

bool Tone3000Client::getCreated (int page, int pageSize, juce::Array<ToneSummary>& out, juce::String& errorOut)
{
  const int p = juce::jmax (1, page);
  const int ps = juce::jlimit (1, 50, pageSize > 0 ? pageSize : 25);
  return listTonesPath ("/api/v1/tones/created?page=" + juce::String (p)
                          + "&page_size=" + juce::String (ps),
                        out, errorOut);
}

bool Tone3000Client::getDownloaded (int page, int pageSize, juce::Array<ToneSummary>& out, juce::String& errorOut)
{
  const int p = juce::jmax (1, page);
  const int ps = juce::jlimit (1, 50, pageSize > 0 ? pageSize : 25);
  return listTonesPath ("/api/v1/tones/downloaded?page=" + juce::String (p)
                          + "&page_size=" + juce::String (ps),
                        out, errorOut);
}

bool Tone3000Client::getModelsForTone (const juce::String& toneId,
                                       const juce::String& architecture,
                                       juce::Array<ModelSummary>& out,
                                       juce::String& errorOut)
{
  auto fetchOnce = [&] (const juce::String& arch) -> bool
  {
    juce::String path = "/api/v1/models?tone_id=" + toneId + "&page=1&page_size=50";
    if (arch.isNotEmpty())
      path += "&architecture=" + arch;

    juce::var result;
    if (! apiGet (path, result, errorOut))
      return false;

    const juce::Array<juce::var>* arr = nullptr;
    if (auto* o = result.getDynamicObject())
      arr = o->getProperty ("data").getArray();
    if (arr == nullptr)
      return true;

    for (auto& item : *arr)
    {
      if (auto* m = item.getDynamicObject())
      {
        ModelSummary s;
        s.id = m->getProperty ("id").toString();
        if (s.id.isEmpty())
          s.id = juce::String ((int) m->getProperty ("id"));
        s.name = m->getProperty ("name").toString();
        s.modelUrl = m->getProperty ("model_url").toString();
        s.architecture = m->getProperty ("architecture_version").toString();
        if (s.architecture.isEmpty())
          s.architecture = m->getProperty ("architecture").toString();
        s.size = m->getProperty ("size").toString();
        out.add (s);
      }
    }
    return true;
  };

  out.clear();
  // Prefer requested architecture (A2 search must list with architecture=2 —
  // omitting defaults to A1+custom and returns empty for A2-only tones).
  if (architecture.isNotEmpty())
  {
    if (! fetchOnce (architecture))
      return false;
    if (! out.isEmpty())
      return true;
  }

  if (! fetchOnce ("2"))
    return false;
  if (! out.isEmpty())
    return true;

  if (! fetchOnce ("1"))
    return false;
  if (! out.isEmpty())
    return true;

  return fetchOnce ({});
}

juce::File Tone3000Client::downloadModelToCache (const ModelSummary& model, juce::String& errorOut)
{
  if (model.modelUrl.isEmpty())
  {
    errorOut = "No model URL";
    return {};
  }

  if (! refreshIfNeeded (errorOut))
    return {};

  juce::URL url (model.modelUrl);
  // model_url requires Bearer auth
  std::unique_ptr<juce::InputStream> stream (
    url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                             .withConnectionTimeoutMs (60000)
                             .withExtraHeaders ("Authorization: Bearer " + mAccessToken + "\n")));
  if (stream == nullptr)
  {
    errorOut = "Download failed (auth/network)";
    return {};
  }

  auto ext = model.modelUrl.containsIgnoreCase (".wav") ? ".wav" : ".nam";
  auto safeId = model.id.isNotEmpty() ? model.id : juce::Uuid().toDashedString();
  auto outFile = getCacheDir().getChildFile (safeId + ext);
  juce::MemoryBlock mb;
  stream->readIntoMemoryBlock (mb);
  if (mb.getSize() < 64)
  {
    errorOut = "Download too small (" + juce::String ((int) mb.getSize())
               + " bytes) — check auth / model URL";
    return {};
  }
  // Reject HTML/JSON error pages saved as .nam/.wav
  const auto* bytes = static_cast<const char*> (mb.getData());
  const juce::String head = juce::String::fromUTF8 (bytes, (int) juce::jmin ((size_t) 32, mb.getSize())).trimStart();
  if (head.startsWithIgnoreCase ("<!DOCTYPE") || head.startsWithIgnoreCase ("<html")
      || head.startsWithIgnoreCase ("{\"error") || head.startsWithIgnoreCase ("{\"message"))
  {
    errorOut = "Download looked like an error page, not a model file";
    return {};
  }
  if (! outFile.replaceWithData (mb.getData(), mb.getSize()))
  {
    errorOut = "Failed to write cache";
    return {};
  }
  return outFile;
}

} // namespace namplifier
