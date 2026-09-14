#pragma once

#include <memory>
#include <functional>
#include <cmath>
#include <stdexcept>

// ResamplingContainer / Lanczos were written for iPlug2 — provide the few
// constants they expect without pulling in the whole framework.
#ifndef DEFAULT_BLOCK_SIZE
#define DEFAULT_BLOCK_SIZE 1024
#endif

namespace iplug
{
inline constexpr double PI = 3.1415926535897932384626433832795;
}

#include "ResamplingContainer.h"
#include "get_dsp.h"
#include "slimmable.h"

namespace namplifier
{

/** Guess 48 kHz when a model predates sample_rate metadata (official NAM behavior). */
inline double getNamSampleRate (const std::unique_ptr<nam::DSP>& model)
{
  const double assumed = 48000.0;
  const double reported = model->GetExpectedSampleRate();
  return reported <= 0.0 ? assumed : reported;
}

/**
 * Host-rate wrapper around NAM using the official AudioDSPTools Lanczos
 * ResamplingContainer (A=12). Per-block Hermite without phase state made
 * models sound like different amps at every host sample rate and added HF
 * aliasing fizz on high-gain profiles.
 */
class ResamplingNAM : public nam::DSP
{
public:
  ResamplingNAM (std::unique_ptr<nam::DSP> encapsulated, const double hostSampleRate)
    : nam::DSP (encapsulated->NumInputChannels(), encapsulated->NumOutputChannels(), hostSampleRate)
    , mEncapsulated (std::move (encapsulated))
    , mResampler (getNamSampleRate (mEncapsulated))
  {
    mBlockProcessFunc = [this] (NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames)
    {
      mEncapsulated->process (input, output, numFrames);
    };

    if (mEncapsulated->HasLoudness())
      SetLoudness (mEncapsulated->GetLoudness());
    if (mEncapsulated->HasInputLevel())
      SetInputLevel (mEncapsulated->GetInputLevel());
    if (mEncapsulated->HasOutputLevel())
      SetOutputLevel (mEncapsulated->GetOutputLevel());

    Reset (hostSampleRate, 2048);
  }

  void prewarm() override { mEncapsulated->prewarm(); }

  void process (NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames) override
  {
    if (num_frames > mMaxExternalBlockSize)
      Reset (mExpectedSampleRate > 0.0 ? mExpectedSampleRate : 48000.0, num_frames);

    if (! needToResample())
    {
      mEncapsulated->process (input, output, num_frames);
      return;
    }

    mResampler.ProcessBlock (input, output, num_frames, mBlockProcessFunc);
  }

  int getLatency() const { return needToResample() ? mResampler.GetLatency() : 0; }

  void Reset (const double sampleRate, const int maxBlockSize) override
  {
    mExpectedSampleRate = sampleRate;
    mMaxExternalBlockSize = maxBlockSize;
    mResampler.Reset (sampleRate, maxBlockSize);

    // Encapsulated model always runs at its native rate; size buffers for the
    // largest corresponding block the resampler may feed it.
    const double modelRate = getEncapsulatedSampleRate();
    const double upRatio = sampleRate / modelRate;
    const auto maxEnc = (std::max) (1, (int) std::ceil ((double) maxBlockSize / upRatio));
    mEncapsulated->Reset (modelRate, maxEnc);
  }

  double getEncapsulatedSampleRate() const { return getNamSampleRate (mEncapsulated); }

  nam::SlimmableModel* getSlimmableModel()
  {
    return dynamic_cast<nam::SlimmableModel*> (mEncapsulated.get());
  }

private:
  bool needToResample() const
  {
    return GetExpectedSampleRate() != getEncapsulatedSampleRate();
  }

  std::unique_ptr<nam::DSP> mEncapsulated;
  dsp::ResamplingContainer<NAM_SAMPLE, 1, 12> mResampler;
  int mMaxExternalBlockSize = 0;
  std::function<void (NAM_SAMPLE**, NAM_SAMPLE**, int)> mBlockProcessFunc;
};

} // namespace namplifier
