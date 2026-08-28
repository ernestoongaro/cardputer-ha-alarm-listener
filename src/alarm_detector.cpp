#include "alarm_detector.h"

#include <algorithm>
#include <cmath>

constexpr std::array<float, 17> AlarmDetector::kScanFrequencies;

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinimumRms = 30.0f;
constexpr float kNoiseMultiplier = 2.5f;
// Real piezo sounders are harmonically rich and may sweep slightly. The
// installed alarm measured 0.05-0.07; cadence and frequency stability provide
// the stronger false-positive rejection after this spectral gate.
constexpr float kMinimumTonalRatio = 0.020f;
// The sustained smoke sounder drives the microphone near clipping, which
// spreads energy into harmonics; measured 0.013-0.10 in the installed room.
constexpr float kInstalledSmokeMinimumTonalRatio = 0.010f;
constexpr float kMinimumNoiseFloor = 12.0f;
constexpr uint32_t kMinimumBurstMs = 280;
constexpr uint32_t kMaximumBurstMs = 850;
constexpr uint32_t kMinimumGapMs = 220;
constexpr uint32_t kMaximumGapMs = 850;
constexpr uint32_t kConfirmationPauseMs = 900;
constexpr uint32_t kEventCooldownMs = 3000;
constexpr float kMaximumFrequencyDriftHz = 600.0f;
constexpr float kInstalledSmokeMinimumHz = 2800.0f;
constexpr float kInstalledSmokeMaximumHz = 3300.0f;
constexpr float kInstalledSmokeMinimumRms = 200.0f;
constexpr uint16_t kInstalledSmokeFrames = 150;  // Three seconds.
constexpr uint8_t kInstalledSmokeDropoutToleranceFrames = 5;  // 100 ms.
// The installed CO alarm was measured as three short 3.15 kHz bursts. This
// overlaps the common temporal-three evacuation cadence, so cadence alone is
// deliberately not used to label a fire alarm on this fixed installation.
constexpr float kInstalledCoMinimumHz = 2850.0f;
constexpr float kInstalledCoMaximumHz = 3450.0f;
constexpr uint8_t kInstalledCoBursts = 3;
}  // namespace

AlarmDetector::AlarmDetector() { reset(); }

void AlarmDetector::reset() {
  noiseFloor_ = 30.0f;
  toneIntegrator_ = 0;
  toneLatched_ = false;
  cooldownUntilMs_ = 0;
  sustainedSignatureFrames_ = 0;
  sustainedSignatureDropoutFrames_ = 0;
  resetPattern();
}

void AlarmDetector::resetPattern() {
  toneStartMs_ = 0;
  toneEndMs_ = 0;
  acceptedBursts_ = 0;
  patternFrequency_ = 0.0f;
  activeFrequency_ = 0.0f;
  activeFrequencySamples_ = 0;
  eventPending_ = false;
}

AlarmDetectorReading AlarmDetector::process(const int16_t* samples,
                                            size_t count, uint32_t nowMs) {
  AlarmDetectorReading reading;
  if (samples == nullptr || count < 32) return reading;

  double mean = 0.0;
  for (size_t i = 0; i < count; ++i) mean += samples[i];
  mean /= static_cast<double>(count);

  double energy = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double centered = static_cast<double>(samples[i]) - mean;
    energy += centered * centered;
  }
  const float rms = static_cast<float>(std::sqrt(energy / count));

  float peakPower = 0.0f;
  float peakFrequency = 0.0f;
  for (const float frequency : kScanFrequencies) {
    const float coefficient =
        2.0f * std::cos(2.0f * kPi * frequency / kSampleRate);
    float q0 = 0.0f;
    float q1 = 0.0f;
    float q2 = 0.0f;
    for (size_t i = 0; i < count; ++i) {
      q0 = static_cast<float>(samples[i] - mean) + coefficient * q1 - q2;
      q2 = q1;
      q1 = q0;
    }
    const float power = q1 * q1 + q2 * q2 - coefficient * q1 * q2;
    if (power > peakPower) {
      peakPower = power;
      peakFrequency = frequency;
    }
  }

  const float tonalRatio = energy > 1.0
                               ? peakPower /
                                     static_cast<float>(energy * count)
                               : 0.0f;
  const float threshold = std::max(kMinimumRms, noiseFloor_ * kNoiseMultiplier);
  const bool instantaneousTone =
      rms >= threshold && tonalRatio >= kMinimumTonalRatio;

  // The installed smoke alarm's TEST output is a sustained, harmonically rich
  // 3 kHz sound rather than temporal-three. Require its measured frequency,
  // high acoustic level and three-second duration. Brief dropouts are allowed
  // because the sounder sweeps between harmonically rich and nearly pure tone.
  bool installedSmokeAlarm = false;
  const bool installedSignature =
      rms >= kInstalledSmokeMinimumRms &&
      tonalRatio >= kInstalledSmokeMinimumTonalRatio &&
      peakFrequency >= kInstalledSmokeMinimumHz &&
      peakFrequency <= kInstalledSmokeMaximumHz;
  if (installedSignature) {
    sustainedSignatureDropoutFrames_ = 0;
    if (sustainedSignatureFrames_ < UINT16_MAX) ++sustainedSignatureFrames_;
    installedSmokeAlarm =
        sustainedSignatureFrames_ == kInstalledSmokeFrames;
  } else {
    if (++sustainedSignatureDropoutFrames_ >=
        kInstalledSmokeDropoutToleranceFrames) {
      sustainedSignatureFrames_ = 0;
      sustainedSignatureDropoutFrames_ = 0;
    }
  }

  // Learn only from frames that do not look like an alarm tone. Upward changes
  // are deliberately slow so an alarm cannot immediately become the baseline.
  // Never learn from frames loud enough to pass the level gate, even when
  // they are not tonal: a clipping sounder otherwise drags the floor up
  // within seconds and locks the detector out.
  if (!instantaneousTone && rms < threshold) {
    const float alpha = rms < noiseFloor_ ? 0.02f : 0.003f;
    noiseFloor_ += alpha * (rms - noiseFloor_);
    noiseFloor_ = std::max(kMinimumNoiseFloor, noiseFloor_);
  }

  if (instantaneousTone) {
    toneIntegrator_ = std::min<uint8_t>(5, toneIntegrator_ + 1);
  } else if (toneIntegrator_ > 0) {
    --toneIntegrator_;
  }

  if (instantaneousTone && toneIntegrator_ > 0) {
    activeFrequency_ += peakFrequency;
    ++activeFrequencySamples_;
  }

  if (!toneLatched_ && toneIntegrator_ >= 3) {
    toneLatched_ = true;
    onToneStart(nowMs, peakFrequency);
  } else if (toneLatched_ && toneIntegrator_ == 0) {
    toneLatched_ = false;
    onToneEnd(nowMs);
  }

  bool smokeAlarm = false;
  bool carbonMonoxideAlarm = false;
  const bool installedCoCadence =
      acceptedBursts_ == kInstalledCoBursts &&
      patternFrequency_ >= kInstalledCoMinimumHz &&
      patternFrequency_ <= kInstalledCoMaximumHz;
  if (!toneLatched_ && installedCoCadence &&
      nowMs - toneEndMs_ >= kConfirmationPauseMs) {
    carbonMonoxideAlarm = true;
    cooldownUntilMs_ = nowMs + kEventCooldownMs;
    resetPattern();
  } else if (!toneLatched_ && acceptedBursts_ > 0 &&
             acceptedBursts_ < 3 && nowMs - toneEndMs_ > kMaximumGapMs) {
    resetPattern();
  }

  reading.alarm = smokeAlarm || installedSmokeAlarm;
  reading.carbonMonoxideAlarm = carbonMonoxideAlarm;
  reading.tone = toneLatched_;
  reading.rms = rms;
  reading.noiseFloor = noiseFloor_;
  reading.peakFrequency = peakFrequency;
  reading.tonalRatio = tonalRatio;
  reading.acceptedBursts = acceptedBursts_;
  return reading;
}

void AlarmDetector::onToneStart(uint32_t nowMs, float frequency) {
  activeFrequency_ = frequency;
  activeFrequencySamples_ = 1;

  if (static_cast<int32_t>(nowMs - cooldownUntilMs_) < 0) return;

  if (acceptedBursts_ > 0) {
    const uint32_t gap = nowMs - toneEndMs_;
    const bool validGap = gap >= kMinimumGapMs && gap <= kMaximumGapMs;
    if (!validGap || acceptedBursts_ >= 4) resetPattern();
  }
  toneStartMs_ = nowMs;
}

void AlarmDetector::onToneEnd(uint32_t nowMs) {
  if (static_cast<int32_t>(nowMs - cooldownUntilMs_) < 0 || toneStartMs_ == 0) {
    return;
  }

  const uint32_t duration = nowMs - toneStartMs_;
  const float burstFrequency = activeFrequencySamples_ > 0
                                   ? activeFrequency_ / activeFrequencySamples_
                                   : 0.0f;
  if (duration < kMinimumBurstMs || duration > kMaximumBurstMs) {
    resetPattern();
    return;
  }
  if (acceptedBursts_ > 0 &&
      std::fabs(burstFrequency - patternFrequency_) >
          kMaximumFrequencyDriftHz) {
    resetPattern();
    return;
  }

  patternFrequency_ = acceptedBursts_ == 0
                          ? burstFrequency
                          : 0.65f * patternFrequency_ + 0.35f * burstFrequency;
  ++acceptedBursts_;
  toneEndMs_ = nowMs;
  eventPending_ = acceptedBursts_ >= 3;
}
