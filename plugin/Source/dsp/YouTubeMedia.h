#pragma once

#include <juce_core/juce_core.h>

namespace namplifier
{

/** Locate yt-dlp on PATH or common install locations. Empty if missing. */
juce::File findYtDlpExecutable();

/** Search YouTube via yt-dlp (ytsearchN:query). Returns array of {id,title,duration,channel,url}. */
juce::Result youtubeSearch (const juce::String& query, int maxResults, juce::Array<juce::var>& resultsOut);

/** Download best audio as WAV into cacheDir / <videoId>.wav. */
juce::Result youtubeDownloadAudio (const juce::String& videoIdOrUrl,
                                   const juce::File& cacheDir,
                                   juce::File& wavOut,
                                   juce::String& titleOut);

juce::File youtubeCacheDir();

} // namespace namplifier
