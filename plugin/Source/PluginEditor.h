#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "tone3000/OAuthLoopback.h"

class NamplifierAudioProcessorEditor : public juce::AudioProcessorEditor,
                                       private juce::Timer
{
public:
  explicit NamplifierAudioProcessorEditor (NamplifierAudioProcessor&);
  ~NamplifierAudioProcessorEditor() override;

  void paint (juce::Graphics&) override;
  void resized() override;
  void visibilityChanged() override;
  void parentSizeChanged() override;

private:
  void timerCallback() override;
  void layoutWeb();

  NamplifierAudioProcessor& processor;
  std::unique_ptr<juce::WebBrowserComponent> web;
  namplifier::OAuthLoopback oauthLoopback;
  int mLayoutPasses = 0;

  std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url) const;
  juce::WebBrowserComponent::Options makeOptions();

  juce::String handleNativeCall (const juce::String& method, const juce::var& args);
  void finishOAuth (const juce::String& code, const juce::String& state, const juce::String& error);

  juce::String mPkceVerifier;
  juce::String mPkceState;
  juce::String mOAuthRedirect;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NamplifierAudioProcessorEditor)
};
