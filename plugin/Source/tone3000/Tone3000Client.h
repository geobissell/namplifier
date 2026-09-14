#pragma once

#include <juce_core/juce_core.h>
#include <functional>

namespace namplifier
{

struct ToneSummary
{
  juce::String id;
  juce::String name;       // API field: title
  juce::String creator;
  juce::String creatorAvatar;
  juce::String format;
  juce::String gear;
  juce::String description;
  juce::String imageUrl;
  juce::String url;
  juce::String gearType; // e.g. amp, amp_cab, pedals
  bool cabIncluded = false;
  int downloads = 0;
  int favorites = 0;
  int modelsCount = 0;
};

struct ModelSummary
{
  juce::String id;
  juce::String name;
  juce::String modelUrl;
  juce::String architecture;
  juce::String size;
};

struct ToneSearchParams
{
  juce::String query;
  juce::String format;       // nam | ir | empty = all
  juce::String architecture; // 1 | 2 | custom | empty
  juce::String sort;         // trending | newest | oldest | best-match | downloads-all-time
  juce::String gears;        // amp, amp-cab, … (underscore-separated)
  juce::String sizes;        // standard, lite, … (underscore-separated)
  bool calibrated = false;
  int page = 1;
  int pageSize = 20;
};

class Tone3000Client
{
public:
  Tone3000Client();

  void setPublishableKey (const juce::String& key);
  juce::String getPublishableKey() const { return mPublishableKey; }

  bool isSignedIn() const;
  void loadTokensFromDisk();
  void clearTokens();

  juce::String buildAuthorizeUrl (const juce::String& redirectUri,
                                  const juce::String& state,
                                  const juce::String& codeChallenge,
                                  const juce::String& prompt = {});

  bool exchangeCode (const juce::String& code,
                     const juce::String& codeVerifier,
                     const juce::String& redirectUri,
                     juce::String& errorOut);

  bool refreshIfNeeded (juce::String& errorOut);

  bool searchTones (const ToneSearchParams& params,
                    juce::Array<ToneSummary>& out,
                    juce::String& errorOut);

  bool getFavorited (int page, int pageSize, juce::Array<ToneSummary>& out, juce::String& errorOut);
  bool getCreated (int page, int pageSize, juce::Array<ToneSummary>& out, juce::String& errorOut);
  bool getDownloaded (int page, int pageSize, juce::Array<ToneSummary>& out, juce::String& errorOut);

  bool setFavorite (const juce::String& toneId, bool favorite, juce::String& errorOut);

  bool getModelsForTone (const juce::String& toneId,
                         const juce::String& architecture,
                         juce::Array<ModelSummary>& out,
                         juce::String& errorOut);

  juce::File downloadModelToCache (const ModelSummary& model, juce::String& errorOut);
  juce::File getCacheDir() const;
  juce::File getTokenFile() const;

  juce::String getAccessToken() const { return mAccessToken; }

private:
  bool apiGet (const juce::String& path, juce::var& result, juce::String& errorOut);
  bool apiRequest (const juce::String& method, const juce::String& path, juce::var& result, juce::String& errorOut);
  void saveTokens();
  juce::Array<ToneSummary> parseToneArray (const juce::var& data);
  bool listTonesPath (const juce::String& path, juce::Array<ToneSummary>& out, juce::String& errorOut);
  static juce::String firstImageUrl (const juce::var& imagesVar);

  juce::String mPublishableKey;
  juce::String mAccessToken;
  juce::String mRefreshToken;
  juce::int64 mExpiresAtMs = 0;
};

} // namespace namplifier
