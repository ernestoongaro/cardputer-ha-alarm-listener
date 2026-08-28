#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

struct AlarmDetectorReading {
  bool alarm = false;  // Installed sustained smoke-alarm signature.
  bool carbonMonoxideAlarm = false;  // Installed three-burst CO signature.
  bool tone = false;
  float rms = 0.0f;
  float noiseFloor = 0.0f;
  float peakFrequency = 0.0f;
  float tonalRatio = 0.0f;
  uint8_t acceptedBursts = 0;
};

// Detects the locally measured smoke and CO signatures. Smoke is a sustained
// ~3 kHz tone; CO is three short ~3.15 kHz bursts with valid gaps and a pause.
// The mapping is intentionally specific to this fixed installation.
class AlarmDetector {
 public:
  static constexpr uint32_t kSampleRate = 16000;
  static constexpr size_t kBlockSamples = 320;  // 20 ms

  AlarmDetector();
  AlarmDetectorReading process(const int16_t* samples, size_t count,
                               uint32_t nowMs);
  void reset();

 private:
  static constexpr std::array<float, 17> kScanFrequencies = {
      1800, 1950, 2100, 2250, 2400, 2550, 2700, 2850, 3000,
      3150, 3300, 3450, 3600, 3750, 3900, 4050, 4200};

  void onToneStart(uint32_t nowMs, float frequency);
  void onToneEnd(uint32_t nowMs);
  void resetPattern();

  float noiseFloor_ = 30.0f;
  uint8_t toneIntegrator_ = 0;
  bool toneLatched_ = false;
  uint32_t toneStartMs_ = 0;
  uint32_t toneEndMs_ = 0;
  uint32_t cooldownUntilMs_ = 0;
  uint8_t acceptedBursts_ = 0;
  float patternFrequency_ = 0.0f;
  float activeFrequency_ = 0.0f;
  uint32_t activeFrequencySamples_ = 0;
  bool eventPending_ = false;
  uint16_t sustainedSignatureFrames_ = 0;
  uint8_t sustainedSignatureDropoutFrames_ = 0;
};
