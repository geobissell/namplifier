#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <cstring>

namespace namplifier
{

/** Tiny localhost HTTP listener for OAuth redirect capture. */
class OAuthLoopback : private juce::Thread
{
public:
  using ResultFn = std::function<void (juce::String code, juce::String state, juce::String error)>;

  OAuthLoopback() : juce::Thread ("NamplifierOAuth") {}
  ~OAuthLoopback() override { stop(); }

  bool start (ResultFn onResult)
  {
    stop();
    mOnResult = std::move (onResult);

    for (int port = 18765; port < 18865; ++port)
    {
      if (mListener.createListener (port, "127.0.0.1"))
      {
        mPort = port;
        startThread (juce::Thread::Priority::normal);
        return true;
      }
    }
    return false;
  }

  void stop()
  {
    signalThreadShouldExit();
    mListener.close();
    stopThread (2000);
    mPort = 0;
  }

  juce::String redirectUri() const
  {
    return "http://127.0.0.1:" + juce::String (mPort) + "/callback";
  }

  int port() const { return mPort; }

private:
  void run() override
  {
    while (! threadShouldExit())
    {
      if (mListener.waitUntilReady (true, 250) < 1)
        continue;

      std::unique_ptr<juce::StreamingSocket> client (mListener.waitForNextConnection());
      if (client == nullptr)
        continue;

      juce::String request;
      char chunk[1024];
      for (int i = 0; i < 50; ++i)
      {
        if (client->waitUntilReady (true, 500) < 1)
          break;
        const int n = client->read (chunk, (int) sizeof (chunk), false);
        if (n <= 0)
          break;
        request += juce::String::fromUTF8 (chunk, n);
        if (request.contains ("\r\n\r\n") || request.contains ("\n\n"))
          break;
      }

      const auto firstLine = request.upToFirstOccurrenceOf ("\n", false, false).trim();
      auto pathAndQuery = firstLine.fromFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf (" ", false, false);
      juce::String code, state, error;
      const auto q = pathAndQuery.fromFirstOccurrenceOf ("?", false, false);
      if (q.isNotEmpty())
      {
        juce::URL fake ("http://127.0.0.1/x?" + q);
        const auto& names = fake.getParameterNames();
        const auto& values = fake.getParameterValues();
        for (int i = 0; i < names.size(); ++i)
        {
          if (names[i] == "code") code = values[i];
          else if (names[i] == "state") state = values[i];
          else if (names[i] == "error") error = values[i];
        }
      }

      const juce::String body =
        "<!DOCTYPE html><html><body style=\"font-family:sans-serif;background:#0c1014;color:#e8eef4;padding:2rem\">"
        "<h2>Namplifier</h2><p>Signed in — you can close this tab and return to the plugin.</p></body></html>";
      const juce::String response =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: "
        + juce::String (body.getNumBytesAsUTF8()) + "\r\n\r\n" + body;
      auto raw = response.toRawUTF8();
      client->write (raw, (int) std::strlen (raw));
      client->close();

      if (mOnResult)
      {
        juce::MessageManager::callAsync ([cb = mOnResult, code, state, error]
        {
          cb (code, state, error);
        });
      }

      signalThreadShouldExit();
      break;
    }
  }

  juce::StreamingSocket mListener;
  ResultFn mOnResult;
  int mPort = 0;
};

} // namespace namplifier
