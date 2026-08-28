#include "alarm_detector.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {
constexpr float kPi = 3.14159265358979323846f;

struct Harness {
  AlarmDetector detector;
  uint32_t nowMs = 0;
  uint32_t rng = 0x12345678;
  float phase = 0.0f;
  int alarms = 0;
  int carbonMonoxideAlarms = 0;

  void feed(uint32_t durationMs, float frequency = 0.0f,
            float amplitude = 0.0f, float noiseAmplitude = 80.0f,
            float secondHarmonicAmplitude = 0.0f) {
    const uint32_t blocks = durationMs / 20;
    int16_t samples[AlarmDetector::kBlockSamples];
    for (uint32_t block = 0; block < blocks; ++block) {
      for (size_t i = 0; i < AlarmDetector::kBlockSamples; ++i) {
        rng = 1664525u * rng + 1013904223u;
        const float noise =
            (static_cast<int32_t>(rng >> 16) - 32768) / 32768.0f *
            noiseAmplitude;
        const float tone =
            frequency > 0.0f
                ? std::sin(phase) * amplitude +
                      std::sin(2.0f * phase) * secondHarmonicAmplitude
                : 0;
        phase += 2.0f * kPi * frequency / AlarmDetector::kSampleRate;
        if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
        samples[i] = static_cast<int16_t>(tone + noise);
      }
      nowMs += 20;
      const auto reading =
          detector.process(samples, AlarmDetector::kBlockSamples, nowMs);
      if (reading.alarm) ++alarms;
      if (reading.carbonMonoxideAlarm) ++carbonMonoxideAlarms;
    }
  }

  void coCadence(float frequency = 3150.0f) {
    for (int i = 0; i < 3; ++i) {
      feed(500, frequency, 6000);
      feed(i == 2 ? 1200 : 500);
    }
  }
};

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
}  // namespace

int main() {
  {
    Harness h;
    h.feed(1500);
    h.coCadence();
    require(h.alarms == 0,
            "the installed CO cadence must not trigger smoke");
    require(h.carbonMonoxideAlarms == 1,
            "a clean three-burst installed CO cadence must trigger once");
  }
  {
    Harness h;
    h.feed(1000);
    h.feed(6000, 2400, 6000);
    h.feed(1500);
    require(h.alarms == 0,
            "a continuous tone outside the installed signature must not trigger");
  }
  {
    Harness h;
    h.feed(1000);
    h.feed(4000, 3000, 5000, 80, 5000);
    h.feed(1000);
    require(h.alarms == 1,
            "the installed harmonically rich sustained signature must trigger");
    require(h.carbonMonoxideAlarms == 0,
            "the installed smoke signature must not trigger CO");
  }
  {
    Harness h;
    h.feed(1000);
    for (int i = 0; i < 4; ++i) {
      h.feed(500, 3000, 6000);
      h.feed(i == 3 ? 1500 : 500);
    }
    require(h.alarms == 0,
            "an unprofiled temporal-four cadence must not trigger smoke");
    require(h.carbonMonoxideAlarms == 0,
            "an unprofiled temporal-four cadence must not trigger CO");
  }
  {
    Harness h;
    h.feed(1000, 0, 0, 500);
    h.feed(500, 3000, 6000);
    h.feed(1400);
    h.feed(500, 3000, 6000);
    h.feed(500);
    h.feed(500, 2200, 6000);
    h.feed(1500);
    require(h.alarms == 0 && h.carbonMonoxideAlarms == 0,
            "bad gaps and changing pitch must not trigger either alarm");
  }
  {
    Harness h;
    h.feed(2000, 0, 0, 1200);
    h.coCadence();
    require(h.carbonMonoxideAlarms == 1,
            "the adaptive floor must tolerate loud background noise");
  }
  {
    Harness h;
    h.feed(1000);
    h.coCadence(2400);
    require(h.carbonMonoxideAlarms == 0,
            "three bursts outside the installed CO frequency must not trigger");
  }
  std::cout << "All alarm detector tests passed\n";
  return 0;
}
