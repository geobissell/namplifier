#pragma once

#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "GraphModel.h"
#include "ResamplingNAM.h"

namespace namplifier
{

/** Load a cab IR on a worker thread into a prepared Convolution engine. */
bool loadIrFileIntoConvolution (juce::dsp::Convolution& conv, const juce::File& file, juce::String& errorOut);

class RuntimeNode
{
public:
  virtual ~RuntimeNode() = default;
  virtual void prepare (double sampleRate, int maxBlockSize) = 0;
  /** Stereo process. Default mono nodes implement processMono and duplicate L→R. */
  virtual void process (float* left, float* right, int numSamples)
  {
    processMono (left, numSamples);
    if (right != nullptr && right != left)
      juce::FloatVectorOperations::copy (right, left, numSamples);
  }
  virtual void processMono (float* buffer, int numSamples) { juce::ignoreUnused (buffer, numSamples); }
  virtual void setParams (const NodeParams& p) = 0;
  virtual NodeType type() const = 0;
  virtual bool isStereoProcessor() const { return false; }
  juce::String id;
  bool bypass = false;
};

class PassThroughNode : public RuntimeNode
{
public:
  explicit PassThroughNode (NodeType t) : mType (t) {}
  void prepare (double, int) override {}
  void processMono (float*, int) override {}
  void setParams (const NodeParams& p) override { bypass = p.bypass; }
  NodeType type() const override { return mType; }
private:
  NodeType mType;
};

class GainNode : public RuntimeNode
{
public:
  void prepare (double, int) override {}
  void processMono (float* buffer, int numSamples) override
  {
    if (bypass) return;
    juce::FloatVectorOperations::multiply (buffer, mGain, numSamples);
  }
  void setParams (const NodeParams& p) override
  {
    bypass = p.bypass;
    mGain = juce::Decibels::decibelsToGain (p.levelDb);
  }
  NodeType type() const override { return NodeType::Bypass; }
private:
  float mGain = 1.0f;
};

class NamRuntimeNode : public RuntimeNode
{
public:
  void prepare (double sampleRate, int maxBlockSize) override;
  void processMono (float* buffer, int numSamples) override;
  void setParams (const NodeParams& p) override;
  NodeType type() const override { return NodeType::Nam; }

  void stageModel (std::unique_ptr<ResamplingNAM> model);
  void applyStaging();
  bool hasModel() const { return mModel != nullptr || mHasStaged.load(); }
  int getLatency() const { return mModel != nullptr ? mModel->getLatency() : 0; }
  juce::String filePath;
  juce::String displayName;
  juce::String lastError;

private:
  void refreshNormGain();

  std::unique_ptr<ResamplingNAM> mModel;
  std::unique_ptr<ResamplingNAM> mStaged;
  std::atomic<bool> mHasStaged { false };
  double mSampleRate = 44100.0;
  int mMaxBlock = 512;
  float mGain = 1.0f;
  float mNormGain = 1.0f; // loudness normalize: attenuate hot models only (→ −18 dBFS)
  float mSlim = 1.0f;
  float mBassDb = 0.0f;
  float mMidDb = 0.0f;
  float mTrebleDb = 0.0f;
  juce::dsp::IIR::Filter<float> mBass;
  juce::dsp::IIR::Filter<float> mMid;
  juce::dsp::IIR::Filter<float> mTreble;
  bool mEqActive = false;
  std::vector<float> mScratch;
  NAM_SAMPLE* mInPtr[1] {};
  NAM_SAMPLE* mOutPtr[1] {};
};

class IrRuntimeNode : public RuntimeNode
{
public:
  void prepare (double sampleRate, int maxBlockSize) override;
  void processMono (float* buffer, int numSamples) override;
  void setParams (const NodeParams& p) override;
  NodeType type() const override { return NodeType::Ir; }

  void stageIr (const juce::File& wavFile);
  void stageReadyConvolution (std::unique_ptr<juce::dsp::Convolution> conv, const juce::String& path);
  void applyStaging();
  bool hasIr() const { return mLoadedPath.isNotEmpty(); }
  juce::String filePath;
  juce::String displayName;
  juce::String lastError;

private:
  std::unique_ptr<juce::dsp::Convolution> mConv;
  std::unique_ptr<juce::dsp::Convolution> mStagedConv;
  double mSampleRate = 44100.0;
  int mMaxBlock = 512;
  float mMix = 1.0f;
  float mGain = 1.0f;
  juce::AudioBuffer<float> mDry;
  juce::String mLoadedPath;
  juce::String mStagedPath;
  std::atomic<bool> mHasStaged { false };
};

class SplitRuntimeNode : public RuntimeNode
{
public:
  void prepare (double, int) override {}
  void processMono (float*, int) override {}
  void setParams (const NodeParams& p) override
  {
    bypass = p.bypass;
    mBalance = juce::jlimit (0.0f, 1.0f, p.mix);
  }
  NodeType type() const override { return NodeType::Split; }
  float balance() const { return mBalance; }
private:
  float mBalance = 0.5f;
};

class MergeRuntimeNode : public RuntimeNode
{
public:
  void prepare (double, int) override {}
  void processMono (float*, int) override {}
  void setParams (const NodeParams& p) override
  {
    bypass = p.bypass;
    mBalance = juce::jlimit (0.0f, 1.0f, p.mix);
  }
  NodeType type() const override { return NodeType::Merge; }
  float balance() const { return mBalance; }
private:
  float mBalance = 0.5f;
};

class ReverbRuntimeNode : public RuntimeNode
{
public:
  void prepare (double sampleRate, int maxBlockSize) override
  {
    mSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    juce::dsp::ProcessSpec spec { mSampleRate, (juce::uint32) maxBlockSize, 2 };
    mReverb.prepare (spec);
    mReverb.reset();
    mDry.setSize (2, maxBlockSize);
    mWet.setSize (2, maxBlockSize);
    mToneLpL = mToneLpR = 0.0f;
    mToneHpL = mToneHpR = 0.0f;
    applyReverbParams();
  }

  bool isStereoProcessor() const override { return true; }

  void process (float* left, float* right, int numSamples) override
  {
    if (bypass || left == nullptr || right == nullptr)
      return;

    mDry.copyFrom (0, 0, left, numSamples);
    mDry.copyFrom (1, 0, right, numSamples);
    mWet.makeCopyOf (mDry);
    float* channels[] = { mWet.getWritePointer (0), mWet.getWritePointer (1) };
    juce::dsp::AudioBlock<float> block (channels, 2, (size_t) numSamples);
    juce::dsp::ProcessContextReplacing<float> ctx (block);
    mReverb.process (ctx);

    // Post-colour wet by type
    for (int i = 0; i < numSamples; ++i)
    {
      float wL = mWet.getSample (0, i);
      float wR = mWet.getSample (1, i);
      if (mMode == Mode::Plate)
      {
        // Brighter plate: gentle high emphasis via light highpass + less darkening
        mToneHpL += 0.04f * (wL - mToneHpL);
        mToneHpR += 0.04f * (wR - mToneHpR);
        wL = wL + 0.35f * (wL - mToneHpL);
        wR = wR + 0.35f * (wR - mToneHpR);
      }
      else if (mMode == Mode::Spring)
      {
        // Narrower, twangier: band-ish emphasis + drip
        mToneLpL += 0.18f * (wL - mToneLpL);
        mToneLpR += 0.18f * (wR - mToneLpR);
        mToneHpL += 0.08f * (wL - mToneHpL);
        mToneHpR += 0.08f * (wR - mToneHpR);
        const float bandL = mToneLpL - mToneHpL;
        const float bandR = mToneLpR - mToneHpR;
        wL = 0.55f * wL + 0.9f * bandL;
        wR = 0.55f * wR + 0.9f * bandR;
        wL = std::tanh (wL * 1.15f);
        wR = std::tanh (wR * 1.15f);
      }
      mWet.setSample (0, i, wL);
      mWet.setSample (1, i, wR);
    }

    const float dryG = 1.0f - mMix;
    const float wetG = mMix * mGain;
    for (int i = 0; i < numSamples; ++i)
    {
      left[i] = mDry.getSample (0, i) * dryG + mWet.getSample (0, i) * wetG;
      right[i] = mDry.getSample (1, i) * dryG + mWet.getSample (1, i) * wetG;
    }
  }

  void setParams (const NodeParams& p) override
  {
    bypass = p.bypass;
    mMix = juce::jlimit (0.0f, 1.0f, p.mix);
    mGain = juce::Decibels::decibelsToGain (p.levelDb);
    mRoom = juce::jlimit (0.0f, 1.0f, p.roomSize);
    mDamp = juce::jlimit (0.0f, 1.0f, p.damping);
    mWidth = juce::jlimit (0.0f, 1.0f, p.width);
    const auto mode = p.fxMode.toLowerCase();
    if (mode.contains ("plate"))
      mMode = Mode::Plate;
    else if (mode.contains ("spring"))
      mMode = Mode::Spring;
    else
      mMode = Mode::Room;
    applyReverbParams();
  }

  NodeType type() const override { return NodeType::Fx; }

private:
  enum class Mode { Room, Plate, Spring };

  void applyReverbParams()
  {
    juce::dsp::Reverb::Parameters rp;
    float room = mRoom, damp = mDamp, width = mWidth;
    if (mMode == Mode::Plate)
    {
      room = juce::jlimit (0.0f, 1.0f, mRoom * 0.85f + 0.25f);
      damp = juce::jlimit (0.0f, 1.0f, mDamp * 0.45f);
      width = juce::jmax (mWidth, 0.85f);
    }
    else if (mMode == Mode::Spring)
    {
      room = juce::jlimit (0.0f, 1.0f, mRoom * 0.55f + 0.15f);
      damp = juce::jlimit (0.0f, 1.0f, mDamp * 0.7f + 0.25f);
      width = juce::jlimit (0.0f, 1.0f, mWidth * 0.45f);
    }
    rp.roomSize = room;
    rp.damping = damp;
    rp.wetLevel = 1.0f;
    rp.dryLevel = 0.0f;
    rp.width = width;
    rp.freezeMode = 0.0f;
    mReverb.setParameters (rp);
  }

  juce::dsp::Reverb mReverb;
  juce::AudioBuffer<float> mDry, mWet;
  double mSampleRate = 44100.0;
  float mMix = 0.35f;
  float mGain = 1.0f;
  float mRoom = 0.45f;
  float mDamp = 0.4f;
  float mWidth = 1.0f;
  float mToneLpL = 0.0f, mToneLpR = 0.0f;
  float mToneHpL = 0.0f, mToneHpR = 0.0f;
  Mode mMode = Mode::Room;
};

class DelayRuntimeNode : public RuntimeNode
{
public:
  void prepare (double sampleRate, int maxBlockSize) override
  {
    juce::ignoreUnused (maxBlockSize);
    mSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int maxSamples = juce::jmax (8, (int) std::ceil (mSampleRate * 2.5));
    mBufL.assign ((size_t) maxSamples, 0.0f);
    mBufR.assign ((size_t) maxSamples, 0.0f);
    mWrite = 0;
    mFilterL = mFilterR = 0.0f;
    mLfoPhase = 0.0;
  }

  bool isStereoProcessor() const override { return true; }

  void process (float* left, float* right, int numSamples) override
  {
    if (bypass || mBufL.empty() || left == nullptr || right == nullptr)
      return;

    const int maxDelay = (int) mBufL.size() - 4;
    const float dryG = 1.0f - mMix;
    const float wetG = mMix * mGain;
    const double twoPi = juce::MathConstants<double>::twoPi;
    const double lfoInc = twoPi * (double) mWowHz / mSampleRate;

    for (int i = 0; i < numSamples; ++i)
    {
      float delayMs = mDelayMs;
      if (mMode == Mode::Tape)
      {
        mLfoPhase += lfoInc;
        if (mLfoPhase > twoPi)
          mLfoPhase -= twoPi;
        delayMs += (float) (std::sin (mLfoPhase) * (double) mWowDepthMs);
      }
      const float delaySampF = juce::jlimit (1.0f, (float) maxDelay, delayMs * 0.001f * (float) mSampleRate);
      const int d0 = (int) delaySampF;
      const float frac = delaySampF - (float) d0;
      const size_t i0 = (mWrite + mBufL.size() - (size_t) d0) % mBufL.size();
      const size_t i1 = (mWrite + mBufL.size() - (size_t) (d0 + 1)) % mBufL.size();
      float dL = mBufL[i0] * (1.0f - frac) + mBufL[i1] * frac;
      float dR = mBufR[i0] * (1.0f - frac) + mBufR[i1] * frac;

      const float inL = left[i];
      const float inR = right[i];
      float fbL = dL * mFeedback;
      float fbR = dR * mFeedback;
      if (mMode == Mode::Analogue || mMode == Mode::Tape)
      {
        // Darken repeats
        mFilterL += mToneCoef * (fbL - mFilterL);
        mFilterR += mToneCoef * (fbR - mFilterR);
        fbL = mFilterL;
        fbR = mFilterR;
        const float drive = mMode == Mode::Tape ? 1.35f : 1.15f;
        fbL = std::tanh (fbL * drive);
        fbR = std::tanh (fbR * drive);
      }

      if (mPingPong)
      {
        mBufL[mWrite] = inL + fbR;
        mBufR[mWrite] = inR + fbL;
      }
      else
      {
        mBufL[mWrite] = inL + fbL;
        mBufR[mWrite] = inR + fbR;
      }
      mWrite = (mWrite + 1) % mBufL.size();
      left[i] = inL * dryG + dL * wetG;
      right[i] = inR * dryG + dR * wetG;
    }
  }

  void setParams (const NodeParams& p) override
  {
    bypass = p.bypass;
    mMix = juce::jlimit (0.0f, 1.0f, p.mix);
    mGain = juce::Decibels::decibelsToGain (p.levelDb);
    mDelayMs = juce::jlimit (1.0f, 2000.0f, p.delayMs);
    mFeedback = juce::jlimit (0.0f, 0.95f, p.feedback);
    mPingPong = p.fxId.equalsIgnoreCase ("pingpong")
                || p.displayName.containsIgnoreCase ("ping");
    const auto mode = p.fxMode.toLowerCase();
    if (mode.contains ("tape"))
      mMode = Mode::Tape;
    else if (mode.contains ("analogue") || mode.contains ("analog"))
      mMode = Mode::Analogue;
    else
      mMode = Mode::Digital;
    // Analogue ~2.8 kHz, tape a bit darker
    const double fc = mMode == Mode::Tape ? 2200.0 : 2800.0;
    mToneCoef = 1.0f - (float) std::exp (-2.0 * juce::MathConstants<double>::pi * fc / mSampleRate);
    mWowHz = 0.35f;
    mWowDepthMs = mMode == Mode::Tape ? juce::jlimit (0.2f, 4.0f, 0.8f + mFeedback * 1.5f) : 0.0f;
  }

  NodeType type() const override { return NodeType::Fx; }

private:
  enum class Mode { Digital, Analogue, Tape };

  std::vector<float> mBufL, mBufR;
  size_t mWrite = 0;
  double mSampleRate = 44100.0;
  double mLfoPhase = 0.0;
  float mMix = 0.35f;
  float mGain = 1.0f;
  float mDelayMs = 350.0f;
  float mFeedback = 0.35f;
  float mFilterL = 0.0f, mFilterR = 0.0f;
  float mToneCoef = 0.2f;
  float mWowHz = 0.35f;
  float mWowDepthMs = 0.0f;
  bool mPingPong = false;
  Mode mMode = Mode::Digital;
};

class NoiseGateRuntimeNode : public RuntimeNode
{
public:
  void prepare (double sampleRate, int maxBlockSize) override
  {
    juce::ignoreUnused (maxBlockSize);
    mSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    mDetect = 0.0f;
    mGain = 0.0f;
    mOpen = false;
    mHoldSamplesLeft = 0;
    mHpZ1 = 0.0f;
    // ~120 Hz detector HPF so rumble/hum doesn't hold the gate open
    const double fc = 120.0;
    mHpCoeff = (float) std::exp (-2.0 * juce::MathConstants<double>::pi * fc / mSampleRate);
  }

  bool isStereoProcessor() const override { return true; }

  void process (float* left, float* right, int numSamples) override
  {
    if (bypass || left == nullptr || right == nullptr)
      return;

    const float openThresh = juce::Decibels::decibelsToGain (mThresholdDb);
    const float closeThresh = juce::Decibels::decibelsToGain (mThresholdDb - mHysteresisDb);
    const float closedGain = juce::Decibels::decibelsToGain (mRangeDb);
    // Fast peak-ish detector with slightly slower release for less chatter
    const float detAtk = coeffForMs (0.8f);
    const float detRel = coeffForMs (35.0f);
    const float gainAtk = coeffForMs (mAttackMs);
    const float gainRel = coeffForMs (mReleaseMs);
    const int holdSamples = juce::jmax (0, (int) std::lround ((double) mHoldMs * 0.001 * mSampleRate));

    for (int i = 0; i < numSamples; ++i)
    {
      const float peak = juce::jmax (std::abs (left[i]), std::abs (right[i]));
      // One-pole HPF on detector input
      const float hp = peak - mHpZ1;
      mHpZ1 = peak + (mHpZ1 - peak) * mHpCoeff;
      const float sensed = std::abs (hp);

      const float detCoef = sensed > mDetect ? detAtk : detRel;
      mDetect += (sensed - mDetect) * detCoef;

      if (! mOpen && mDetect >= openThresh)
      {
        mOpen = true;
        mHoldSamplesLeft = holdSamples;
      }
      else if (mOpen)
      {
        if (mDetect >= closeThresh)
          mHoldSamplesLeft = holdSamples;
        else if (mHoldSamplesLeft > 0)
          --mHoldSamplesLeft;
        else
          mOpen = false;
      }

      const float target = mOpen ? 1.0f : closedGain;
      const float gCoef = target > mGain ? gainAtk : gainRel;
      mGain += (target - mGain) * gCoef;
      left[i] *= mGain;
      right[i] *= mGain;
    }
  }

  void setParams (const NodeParams& p) override
  {
    bypass = p.bypass;
    mThresholdDb = juce::jlimit (-80.0f, 0.0f, p.thresholdDb);
    mAttackMs = juce::jlimit (0.1f, 200.0f, p.attackMs);
    mReleaseMs = juce::jlimit (1.0f, 2000.0f, p.releaseMs);
    mRangeDb = juce::jlimit (-90.0f, 0.0f, p.gateRangeDb);
    mHysteresisDb = juce::jlimit (0.0f, 24.0f, p.hysteresisDb);
    mHoldMs = juce::jlimit (0.0f, 500.0f, p.holdMs);
  }

  NodeType type() const override { return NodeType::Fx; }

private:
  float coeffForMs (float ms) const
  {
    const float t = juce::jmax (0.0001f, ms * 0.001f);
    return 1.0f - std::exp (-1.0f / (float) (mSampleRate * (double) t));
  }

  double mSampleRate = 44100.0;
  float mDetect = 0.0f;
  float mGain = 0.0f;
  bool mOpen = false;
  int mHoldSamplesLeft = 0;
  float mHpZ1 = 0.0f;
  float mHpCoeff = 0.0f;
  float mThresholdDb = -50.0f;
  float mAttackMs = 2.0f;
  float mReleaseMs = 150.0f;
  float mRangeDb = -70.0f;
  float mHysteresisDb = 8.0f;
  float mHoldMs = 80.0f;
};

class ChorusRuntimeNode : public RuntimeNode
{
public:
  void prepare (double sampleRate, int maxBlockSize) override
  {
    juce::ignoreUnused (maxBlockSize);
    mSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    const int maxSamples = juce::jmax (64, (int) std::ceil (mSampleRate * 0.08)); // 80 ms
    mBufL.assign ((size_t) maxSamples, 0.0f);
    mBufR.assign ((size_t) maxSamples, 0.0f);
    mWrite = 0;
    mPhase = 0.0;
  }

  bool isStereoProcessor() const override { return true; }

  void process (float* left, float* right, int numSamples) override
  {
    if (bypass || mBufL.empty() || left == nullptr || right == nullptr)
      return;

    const float dryG = 1.0f - mMix;
    const float wetG = mMix * mGain;
    const double twoPi = juce::MathConstants<double>::twoPi;
    const double inc = twoPi * (double) mRateHz / mSampleRate;
    const float baseMs = 12.0f;
    const float modMs = juce::jlimit (0.5f, 8.0f, mDepth * 6.0f);
    const int maxDelay = (int) mBufL.size() - 4;

    for (int i = 0; i < numSamples; ++i)
    {
      mPhase += inc;
      if (mPhase > twoPi)
        mPhase -= twoPi;
      const float lfoL = (float) std::sin (mPhase);
      const float lfoR = (float) std::sin (mPhase + juce::MathConstants<double>::halfPi);

      auto readAt = [&] (const std::vector<float>& buf, float delayMs) -> float
      {
        const float ds = juce::jlimit (1.0f, (float) maxDelay, delayMs * 0.001f * (float) mSampleRate);
        const int d0 = (int) ds;
        const float frac = ds - (float) d0;
        const size_t i0 = (mWrite + buf.size() - (size_t) d0) % buf.size();
        const size_t i1 = (mWrite + buf.size() - (size_t) (d0 + 1)) % buf.size();
        return buf[i0] * (1.0f - frac) + buf[i1] * frac;
      };

      const float inL = left[i];
      const float inR = right[i];
      const float dL = readAt (mBufL, baseMs + lfoL * modMs);
      const float dR = readAt (mBufR, baseMs + lfoR * modMs);

      mBufL[mWrite] = inL + dL * mFeedback;
      mBufR[mWrite] = inR + dR * mFeedback;
      mWrite = (mWrite + 1) % mBufL.size();

      left[i] = inL * dryG + dL * wetG;
      right[i] = inR * dryG + dR * wetG;
    }
  }

  void setParams (const NodeParams& p) override
  {
    bypass = p.bypass;
    mMix = juce::jlimit (0.0f, 1.0f, p.mix);
    mGain = juce::Decibels::decibelsToGain (p.levelDb);
    mRateHz = juce::jlimit (0.05f, 8.0f, p.rateHz);
    mDepth = juce::jlimit (0.0f, 1.0f, p.depth);
    mFeedback = juce::jlimit (0.0f, 0.7f, p.feedback);
  }

  NodeType type() const override { return NodeType::Fx; }

private:
  std::vector<float> mBufL, mBufR;
  size_t mWrite = 0;
  double mSampleRate = 44100.0;
  double mPhase = 0.0;
  float mMix = 0.4f;
  float mGain = 1.0f;
  float mRateHz = 0.8f;
  float mDepth = 0.35f;
  float mFeedback = 0.1f;
};

class CompressorRuntimeNode : public RuntimeNode
{
public:
  void prepare (double sampleRate, int maxBlockSize) override
  {
    juce::ignoreUnused (maxBlockSize);
    mSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    mEnv = 0.0f;
    mGain = 1.0f;
  }

  bool isStereoProcessor() const override { return true; }

  void process (float* left, float* right, int numSamples) override
  {
    if (bypass || left == nullptr || right == nullptr)
      return;

    const float threshLin = juce::Decibels::decibelsToGain (mThresholdDb);
    const float makeup = juce::Decibels::decibelsToGain (mMakeupDb);
    const float atk = coeffForMs (mAttackMs);
    const float rel = coeffForMs (mReleaseMs);
    const float dryG = 1.0f - mMix;
    const float wetG = mMix;

    for (int i = 0; i < numSamples; ++i)
    {
      const float inL = left[i];
      const float inR = right[i];
      const float peak = juce::jmax (std::abs (inL), std::abs (inR));
      const float coef = peak > mEnv ? atk : rel;
      mEnv += (peak - mEnv) * coef;

      float gr = 1.0f;
      if (mEnv > threshLin && mEnv > 1.0e-8f)
      {
        const float overDb = juce::Decibels::gainToDecibels (mEnv / threshLin);
        const float reducedDb = overDb - overDb / juce::jmax (1.0f, mRatio);
        gr = juce::Decibels::decibelsToGain (-reducedDb);
      }
      mGain += (gr - mGain) * 0.35f;

      const float g = mGain * makeup;
      const float wetL = inL * g;
      const float wetR = inR * g;
      left[i] = inL * dryG + wetL * wetG;
      right[i] = inR * dryG + wetR * wetG;
    }
  }

  void setParams (const NodeParams& p) override
  {
    bypass = p.bypass;
    mMix = juce::jlimit (0.0f, 1.0f, p.mix);
    mThresholdDb = juce::jlimit (-60.0f, 0.0f, p.thresholdDb);
    mRatio = juce::jlimit (1.0f, 20.0f, p.ratio);
    mAttackMs = juce::jlimit (0.1f, 200.0f, p.attackMs);
    mReleaseMs = juce::jlimit (5.0f, 2000.0f, p.releaseMs);
    mMakeupDb = juce::jlimit (0.0f, 24.0f, p.makeupDb);
  }

  NodeType type() const override { return NodeType::Fx; }

private:
  float coeffForMs (float ms) const
  {
    const float t = juce::jmax (0.0001f, ms * 0.001f);
    return 1.0f - std::exp (-1.0f / (float) (mSampleRate * (double) t));
  }

  double mSampleRate = 44100.0;
  float mEnv = 0.0f;
  float mGain = 1.0f;
  float mMix = 1.0f;
  float mThresholdDb = -18.0f;
  float mRatio = 4.0f;
  float mAttackMs = 10.0f;
  float mReleaseMs = 100.0f;
  float mMakeupDb = 0.0f;
};

std::unique_ptr<RuntimeNode> createRuntimeNode (const GraphNode& desc);

} // namespace namplifier
