#include "YouTubeMedia.h"

namespace namplifier
{
namespace
{

juce::String quoteArg (const juce::String& s)
{
  // Minimal shell-safe quoting for ChildProcess argument lists (we pass argv-style on Windows via string).
  if (! s.containsAnyOf (" \t\"'&|<>^"))
    return s;
  return "\"" + s.replace ("\"", "\\\"") + "\"";
}

bool runProcess (const juce::String& exe, const juce::StringArray& args,
                 juce::String& stdoutOut, juce::String& stderrOut, int timeoutMs = 120000)
{
  juce::ChildProcess proc;
  juce::StringArray cmd;
  cmd.add (exe);
  cmd.addArray (args);
  if (! proc.start (cmd))
    return false;

  stdoutOut = proc.readAllProcessOutput();
  // readAllProcessOutput waits until exit; still call wait in case of empty output.
  proc.waitForProcessToFinish (timeoutMs);
  stderrOut.clear();
  juce::ignoreUnused (stderrOut);
  return proc.getExitCode() == 0 || stdoutOut.isNotEmpty();
}

} // namespace

juce::File findYtDlpExecutable()
{
  const char* names[] = { "yt-dlp.exe", "yt-dlp", "youtube-dl.exe", "youtube-dl" };

  auto tryNamesInDir = [&] (const juce::File& dir) -> juce::File
  {
    if (! dir.isDirectory())
      return {};
    for (auto* name : names)
    {
      auto f = dir.getChildFile (name);
      if (f.existsAsFile())
        return f;
    }
    return {};
  };

  if (auto beside = tryNamesInDir (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory());
      beside.existsAsFile())
    return beside;

  const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
#if JUCE_WINDOWS
  // pip --user / python.org installs
  const juce::File candidates[] = {
    home.getChildFile ("AppData/Local/Programs/Python"),
    home.getChildFile ("AppData/Roaming/Python"),
    home.getChildFile ("AppData/Local/Microsoft/WindowsApps"),
    juce::File ("C:/Program Files/yt-dlp"),
    juce::File ("C:/yt-dlp"),
  };

  for (auto& root : candidates)
  {
    if (auto hit = tryNamesInDir (root); hit.existsAsFile())
      return hit;

    if (! root.isDirectory())
      continue;

    // Python3xx/Scripts/yt-dlp.exe
    for (auto& sub : root.findChildFiles (juce::File::findDirectories, false, "*"))
    {
      if (auto hit = tryNamesInDir (sub.getChildFile ("Scripts")); hit.existsAsFile())
        return hit;
      if (auto hit = tryNamesInDir (sub); hit.existsAsFile())
        return hit;
    }
  }

  // LocalAppData\Programs\Python\Python3xx\Scripts
  {
    auto pyRoot = home.getChildFile ("AppData/Local/Programs/Python");
    if (pyRoot.isDirectory())
    {
      for (auto& ver : pyRoot.findChildFiles (juce::File::findDirectories, false, "Python*"))
        if (auto hit = tryNamesInDir (ver.getChildFile ("Scripts")); hit.existsAsFile())
          return hit;
    }
  }
#else
  const juce::File unixCandidates[] = {
    home.getChildFile (".local/bin"),
    juce::File ("/usr/local/bin"),
    juce::File ("/opt/homebrew/bin"),
    juce::File ("/usr/bin"),
  };
  for (auto& dir : unixCandidates)
    if (auto hit = tryNamesInDir (dir); hit.existsAsFile())
      return hit;
#endif

  // PATH lookup via `where` / `which` (also try yt-dlp.exe on Windows)
  auto lookupOnPath = [] (const juce::String& tool) -> juce::File
  {
    juce::ChildProcess which;
#if JUCE_WINDOWS
    if (! which.start ("cmd.exe /C where " + tool))
      return {};
#else
    if (! which.start ("which " + tool))
      return {};
#endif
    auto out = which.readAllProcessOutput().trim().replaceCharacter ('\r', '\n');
    const auto first = out.upToFirstOccurrenceOf ("\n", false, false).trim();
    if (first.isNotEmpty() && juce::File (first).existsAsFile())
      return juce::File (first);
    return {};
  };

#if JUCE_WINDOWS
  if (auto hit = lookupOnPath ("yt-dlp.exe"); hit.existsAsFile())
    return hit;
  if (auto hit = lookupOnPath ("yt-dlp"); hit.existsAsFile())
    return hit;
  if (auto hit = lookupOnPath ("youtube-dl.exe"); hit.existsAsFile())
    return hit;
#else
  if (auto hit = lookupOnPath ("yt-dlp"); hit.existsAsFile())
    return hit;
  if (auto hit = lookupOnPath ("youtube-dl"); hit.existsAsFile())
    return hit;
#endif

  return {};
}

juce::File findFfmpegExecutable()
{
  const char* names[] = { "ffmpeg.exe", "ffmpeg" };

  auto tryNamesInDir = [&] (const juce::File& dir) -> juce::File
  {
    if (! dir.isDirectory())
      return {};
    for (auto* name : names)
    {
      auto f = dir.getChildFile (name);
      if (f.existsAsFile())
        return f;
    }
    return {};
  };

  if (auto beside = tryNamesInDir (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory());
      beside.existsAsFile())
    return beside;

  const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
#if JUCE_WINDOWS
  const juce::File wingetPkgs = home.getChildFile ("AppData/Local/Microsoft/WinGet/Packages");
  if (wingetPkgs.isDirectory())
  {
    auto matches = wingetPkgs.findChildFiles (juce::File::findFiles, true, "ffmpeg.exe");
    if (! matches.isEmpty())
      return matches[0];
  }

  const juce::File progFiles[] = {
    juce::File ("C:/ffmpeg/bin"),
    juce::File ("C:/Program Files/ffmpeg/bin"),
  };
  for (auto& dir : progFiles)
    if (auto hit = tryNamesInDir (dir); hit.existsAsFile())
      return hit;

  juce::ChildProcess which;
  if (which.start ("cmd.exe /C where ffmpeg.exe"))
  {
    auto out = which.readAllProcessOutput().trim().replaceCharacter ('\r', '\n');
    const auto first = out.upToFirstOccurrenceOf ("\n", false, false).trim();
    if (first.isNotEmpty() && juce::File (first).existsAsFile())
      return juce::File (first);
  }
#else
  const juce::File unixCandidates[] = {
    home.getChildFile (".local/bin"),
    juce::File ("/usr/local/bin"),
    juce::File ("/opt/homebrew/bin"),
    juce::File ("/usr/bin"),
  };
  for (auto& dir : unixCandidates)
    if (auto hit = tryNamesInDir (dir); hit.existsAsFile())
      return hit;
#endif

  return {};
}

juce::File youtubeCacheDir()
{
  auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Namplifier")
               .getChildFile ("youtube_cache");
  dir.createDirectory();
  return dir;
}

juce::Result youtubeSearch (const juce::String& query, int maxResults, juce::Array<juce::var>& resultsOut)
{
  resultsOut.clear();
  const auto exe = findYtDlpExecutable();
  if (! exe.existsAsFile())
    return juce::Result::fail ("YouTube search isn’t available on this machine.");

  const int n = juce::jlimit (1, 20, maxResults);
  const juce::String searchSpec = "ytsearch" + juce::String (n) + ":" + query.trim();

  juce::StringArray args;
  args.add ("--flat-playlist");
  args.add ("--no-warnings");
  args.add ("-j");
  args.add (searchSpec);

  juce::String out, err;
  if (! runProcess (exe.getFullPathName(), args, out, err, 60000))
    return juce::Result::fail ("YouTube search failed.");

  auto lines = juce::StringArray::fromLines (out);
  for (auto& line : lines)
  {
    line = line.trim();
    if (line.isEmpty())
      continue;
    auto parsed = juce::JSON::parse (line);
    auto* o = parsed.getDynamicObject();
    if (o == nullptr)
      continue;

    auto* row = new juce::DynamicObject();
    const auto id = o->getProperty ("id").toString();
    row->setProperty ("id", id);
    row->setProperty ("title", o->getProperty ("title").toString());
    row->setProperty ("channel", o->getProperty ("channel").toString().isNotEmpty()
                                   ? o->getProperty ("channel").toString()
                                   : o->getProperty ("uploader").toString());
    row->setProperty ("duration", (double) o->getProperty ("duration"));
    row->setProperty ("url", id.isNotEmpty() ? ("https://www.youtube.com/watch?v=" + id)
                                             : o->getProperty ("url").toString());
    resultsOut.add (juce::var (row));
  }

  if (resultsOut.isEmpty())
    return juce::Result::fail ("No YouTube results for that search.");

  juce::ignoreUnused (quoteArg);
  return juce::Result::ok();
}

juce::Result youtubeDownloadAudio (const juce::String& videoIdOrUrl,
                                   const juce::File& cacheDir,
                                   juce::File& wavOut,
                                   juce::String& titleOut)
{
  const auto exe = findYtDlpExecutable();
  if (! exe.existsAsFile())
    return juce::Result::fail ("YouTube playback isn’t available on this machine.");

  juce::String id = videoIdOrUrl.trim();
  juce::String url = id;
  if (! id.containsIgnoreCase ("youtube.com") && ! id.containsIgnoreCase ("youtu.be"))
  {
    // bare video id
    if (id.length() >= 8 && id.length() <= 20 && ! id.containsChar (' '))
      url = "https://www.youtube.com/watch?v=" + id;
  }
  else
  {
    // extract id for cache filename if possible
    if (id.contains ("v="))
      id = id.fromFirstOccurrenceOf ("v=", false, false).upToFirstOccurrenceOf ("&", false, false);
    else if (id.contains ("youtu.be/"))
      id = id.fromFirstOccurrenceOf ("youtu.be/", false, false).upToFirstOccurrenceOf ("?", false, false);
  }

  if (id.isEmpty())
    id = juce::String::toHexString (url.hashCode64());

  wavOut = cacheDir.getChildFile (id + ".wav");
  if (wavOut.existsAsFile() && wavOut.getSize() > 1024)
  {
    titleOut = id;
    return juce::Result::ok();
  }

  const auto outTemplate = cacheDir.getChildFile (id + ".%(ext)s").getFullPathName();

  juce::StringArray args;
  args.add ("-x");
  args.add ("--audio-format");
  args.add ("wav");
  args.add ("--audio-quality");
  args.add ("0");
  args.add ("--no-playlist");
  args.add ("--no-warnings");
  if (auto ffmpeg = findFfmpegExecutable(); ffmpeg.existsAsFile())
  {
    args.add ("--ffmpeg-location");
    args.add (ffmpeg.getParentDirectory().getFullPathName());
  }
  args.add ("-o");
  args.add (outTemplate);
  args.add ("--print");
  args.add ("after_move:title");
  args.add (url);

  juce::String out, err;
  if (! runProcess (exe.getFullPathName(), args, out, err, 300000))
    return juce::Result::fail ("Couldn’t download that video’s audio.");

  titleOut = out.trim().upToFirstOccurrenceOf ("\n", false, false).trim();
  if (! wavOut.existsAsFile())
  {
    // yt-dlp may leave a different extension briefly; scan
    auto matches = cacheDir.findChildFiles (juce::File::findFiles, false, id + ".*");
    for (auto& f : matches)
    {
      if (f.hasFileExtension ("wav"))
      {
        wavOut = f;
        break;
      }
    }
  }

  if (! wavOut.existsAsFile())
    return juce::Result::fail ("Couldn’t prepare the audio file.");

  return juce::Result::ok();
}

} // namespace namplifier
