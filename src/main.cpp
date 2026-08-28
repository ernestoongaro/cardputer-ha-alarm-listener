#include <Arduino.h>
#include <M5Cardputer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstring>

#include "alarm_detector.h"
#if __has_include("device_config.h")
#include "device_config.h"
#else
#include "device_config.example.h"
#endif

namespace {
constexpr char kFirmwareVersion[] = "0.2.0";
constexpr uint32_t kAlarmQuietBeforeClearMs = 90000;
constexpr uint32_t kMinimumVisibleAlarmMs = 60000;
constexpr uint32_t kTelemetryIntervalMs = 10000;
constexpr uint32_t kUiIntervalMs = 250;
constexpr uint32_t kDisplaySleepMs = 120000;
constexpr uint8_t kNormalBrightness = 72;

AlarmDetector detector;
int16_t audioBlock[AlarmDetector::kBlockSamples];
Preferences preferences;
WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

char deviceId[40] = {};
char mqttHost[65] = {};
char mqttPortText[7] = "1883";
char mqttUser[65] = {};
char mqttPassword[65] = {};
char apName[48] = {};
char apPassword[20] = {};
char stateTopic[96] = {};
char coStateTopic[96] = {};
char availabilityTopic[96] = {};
char telemetryTopic[96] = {};
char discoveryTopic[128] = {};
char coDiscoveryTopic[128] = {};

WiFiManagerParameter* mqttHostParameter = nullptr;
WiFiManagerParameter* mqttPortParameter = nullptr;
WiFiManagerParameter* mqttUserParameter = nullptr;
WiFiManagerParameter* mqttPasswordParameter = nullptr;

portMUX_TYPE sharedMux = portMUX_INITIALIZER_UNLOCKED;
AlarmDetectorReading latestReading;
bool micHealthy = false;
bool mqttConnected = false;
bool desiredAlarmState = false;
bool desiredCoAlarmState = false;
uint32_t alarmGeneration = 0;
uint32_t coAlarmGeneration = 0;
uint32_t alarmOnPublishedMs = 0;
uint32_t coAlarmOnPublishedMs = 0;
uint32_t configRevision = 1;

bool portalActive = false;
volatile bool portalCloseRequested = false;
uint32_t portalSavedMs = 0;
uint32_t lastDetectionMs = 0;
uint32_t lastCoDetectionMs = 0;
uint32_t lastUiMs = 0;
uint32_t lastSerialMs = 0;
uint32_t lastInteractionMs = 0;
volatile uint8_t lastWiFiDisconnectReason = 0;
bool screenAwake = true;
bool alarmScreenActive = false;
bool mdnsStarted = false;
uint32_t failedAudioReads = 0;
uint32_t silentAudioBlocks = 0;
bool previousToneState = false;
uint8_t previousAcceptedBursts = 0;

void copyString(char* destination, size_t size, const char* source) {
  if (size == 0) return;
  std::strncpy(destination, source == nullptr ? "" : source, size - 1);
  destination[size - 1] = '\0';
}

void loadSettings() {
  preferences.begin("alarm-listener", false);
  preferences.getString("mqtt_host", "").toCharArray(mqttHost,
                                                       sizeof(mqttHost));
  preferences.getString("mqtt_port", "1883")
      .toCharArray(mqttPortText, sizeof(mqttPortText));
  preferences.getString("mqtt_user", "").toCharArray(mqttUser,
                                                       sizeof(mqttUser));
  preferences.getString("mqtt_pass", "").toCharArray(mqttPassword,
                                                       sizeof(mqttPassword));
  desiredAlarmState = preferences.getBool("pending_alarm", false);
  desiredCoAlarmState = preferences.getBool("pending_co", false);
  alarmGeneration = desiredAlarmState ? 1 : 0;
  coAlarmGeneration = desiredCoAlarmState ? 1 : 0;

  // This device is a fixed-purpose appliance. The local, git-ignored build
  // configuration is authoritative on every boot; NVS is still used for the
  // pending alarm latch and for portal fallback settings.
  copyString(mqttHost, sizeof(mqttHost), DeviceConfig::kMqttHost);
  std::snprintf(mqttPortText, sizeof(mqttPortText), "%u",
                DeviceConfig::kMqttPort);
  copyString(mqttUser, sizeof(mqttUser), DeviceConfig::kMqttUser);
  copyString(mqttPassword, sizeof(mqttPassword), DeviceConfig::kMqttPassword);
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWiFiDisconnectReason = info.wifi_sta_disconnected.reason;
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    lastWiFiDisconnectReason = 0;
  }
}

void savePortalSettings() {
  portENTER_CRITICAL(&sharedMux);
  copyString(mqttHost, sizeof(mqttHost), mqttHostParameter->getValue());
  copyString(mqttPortText, sizeof(mqttPortText),
             mqttPortParameter->getValue());
  copyString(mqttUser, sizeof(mqttUser), mqttUserParameter->getValue());
  copyString(mqttPassword, sizeof(mqttPassword),
             mqttPasswordParameter->getValue());
  portEXIT_CRITICAL(&sharedMux);

  const long port = std::strtol(mqttPortText, nullptr, 10);
  if (port < 1 || port > 65535) copyString(mqttPortText, sizeof(mqttPortText), "1883");
  preferences.putString("mqtt_host", mqttHost);
  preferences.putString("mqtt_port", mqttPortText);
  preferences.putString("mqtt_user", mqttUser);
  preferences.putString("mqtt_pass", mqttPassword);
  portENTER_CRITICAL(&sharedMux);
  ++configRevision;
  portEXIT_CRITICAL(&sharedMux);
  portalCloseRequested = true;
}

void startConfigPortal() {
  if (portalActive) return;
  portalCloseRequested = false;
  wifiManager.startConfigPortal(apName, apPassword);
  portalActive = true;
}

void setDesiredAlarm(bool enabled) {
  bool changed = false;
  portENTER_CRITICAL(&sharedMux);
  if (desiredAlarmState != enabled) {
    desiredAlarmState = enabled;
    ++alarmGeneration;
    if (enabled) alarmOnPublishedMs = 0;
    changed = true;
  }
  portEXIT_CRITICAL(&sharedMux);
  if (changed) preferences.putBool("pending_alarm", enabled);
}

void setDesiredCoAlarm(bool enabled) {
  bool changed = false;
  portENTER_CRITICAL(&sharedMux);
  if (desiredCoAlarmState != enabled) {
    desiredCoAlarmState = enabled;
    ++coAlarmGeneration;
    if (enabled) coAlarmOnPublishedMs = 0;
    changed = true;
  }
  portEXIT_CRITICAL(&sharedMux);
  if (changed) preferences.putBool("pending_co", enabled);
}

bool publishDiscovery() {
  char payload[1024];
  const int written = std::snprintf(
      payload, sizeof(payload),
      "{\"name\":\"Smoke alarm listener\",\"unique_id\":\"%s_smoke\","
      "\"state_topic\":\"%s\",\"payload_on\":\"ALARM\",\"payload_off\":\"OK\","
      "\"availability_topic\":\"%s\",\"device_class\":\"smoke\","
      "\"json_attributes_topic\":\"%s\",\"device\":{\"identifiers\":[\"%s\"],"
      "\"name\":\"Cardputer Alarm Listener\",\"manufacturer\":\"M5Stack\","
      "\"model\":\"Cardputer ADV\",\"sw_version\":\"%s\"}}",
      deviceId, stateTopic, availabilityTopic, telemetryTopic, deviceId,
      kFirmwareVersion);
  const bool smokePublished =
      written > 0 && static_cast<size_t>(written) < sizeof(payload) &&
      mqtt.publish(discoveryTopic, payload, true);

  const int coWritten = std::snprintf(
      payload, sizeof(payload),
      "{\"name\":\"Carbon monoxide alarm listener\","
      "\"unique_id\":\"%s_co\",\"state_topic\":\"%s\","
      "\"payload_on\":\"ALARM\",\"payload_off\":\"OK\","
      "\"availability_topic\":\"%s\",\"device_class\":\"carbon_monoxide\","
      "\"json_attributes_topic\":\"%s\",\"device\":{\"identifiers\":[\"%s\"],"
      "\"name\":\"Cardputer Alarm Listener\",\"manufacturer\":\"M5Stack\","
      "\"model\":\"Cardputer ADV\",\"sw_version\":\"%s\"}}",
      deviceId, coStateTopic, availabilityTopic, telemetryTopic, deviceId,
      kFirmwareVersion);
  const bool coPublished =
      coWritten > 0 && static_cast<size_t>(coWritten) < sizeof(payload) &&
      mqtt.publish(coDiscoveryTopic, payload, true);
  return smokePublished && coPublished;
}

void publishTelemetry() {
  AlarmDetectorReading reading;
  bool healthy;
  portENTER_CRITICAL(&sharedMux);
  reading = latestReading;
  healthy = micHealthy;
  portEXIT_CRITICAL(&sharedMux);

  char payload[384];
  std::snprintf(
      payload, sizeof(payload),
      "{\"rms\":%.1f,\"noise_floor\":%.1f,\"tone_frequency_hz\":%.0f,"
      "\"tonal_ratio\":%.3f,\"accepted_bursts\":%u,\"tone\":%s,"
      "\"microphone\":\"%s\",\"wifi_rssi_dbm\":%d,\"uptime_s\":%lu}",
      reading.rms, reading.noiseFloor, reading.peakFrequency,
      reading.tonalRatio, reading.acceptedBursts,
      reading.tone ? "true" : "false", healthy ? "ok" : "fault",
      WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127,
      static_cast<unsigned long>(millis() / 1000));
  mqtt.publish(telemetryTopic, payload, true);
}

bool connectMqtt(const char* host, uint16_t port, const char* user,
                 const char* password) {
  if (host[0] == '\0' || WiFi.status() != WL_CONNECTED) return false;
  mqtt.setServer(host, port);
  if (user[0] != '\0') {
    return mqtt.connect(deviceId, user, password, availabilityTopic, 1, true,
                        "offline");
  }
  return mqtt.connect(deviceId, availabilityTopic, 1, true, "offline");
}

bool resolveMqttHost(const char* host, char* resolved, size_t resolvedSize) {
  IPAddress address;
  if (WiFi.hostByName(host, address) == 1 && address != IPAddress()) {
    copyString(resolved, resolvedSize, address.toString().c_str());
    return true;
  }

  const String hostname(host);
  if (!hostname.endsWith(".local")) return false;
  if (!mdnsStarted) mdnsStarted = MDNS.begin(deviceId);
  if (!mdnsStarted) return false;
  const String shortName = hostname.substring(0, hostname.length() - 6);
  address = MDNS.queryHost(shortName, 2500);
  if (address == IPAddress()) return false;
  copyString(resolved, resolvedSize, address.toString().c_str());
  return true;
}

void mqttTask(void*) {
  uint32_t observedConfigRevision = 0;
  uint32_t publishedAlarmGeneration = UINT32_MAX;
  uint32_t publishedCoAlarmGeneration = UINT32_MAX;
  uint32_t reconnectDelayMs = 1000;
  uint32_t nextReconnectMs = 0;
  uint32_t lastTelemetryMs = 0;
  char host[sizeof(mqttHost)] = {};
  char user[sizeof(mqttUser)] = {};
  char password[sizeof(mqttPassword)] = {};
  char resolvedHost[20] = {};
  uint16_t port = 1883;

  for (;;) {
    uint32_t revision;
    portENTER_CRITICAL(&sharedMux);
    revision = configRevision;
    portEXIT_CRITICAL(&sharedMux);
    if (revision != observedConfigRevision) {
      mqtt.disconnect();
      portENTER_CRITICAL(&sharedMux);
      copyString(host, sizeof(host), mqttHost);
      copyString(user, sizeof(user), mqttUser);
      copyString(password, sizeof(password), mqttPassword);
      port = static_cast<uint16_t>(std::strtol(mqttPortText, nullptr, 10));
      observedConfigRevision = configRevision;
      portEXIT_CRITICAL(&sharedMux);
      if (port == 0) port = 1883;
      publishedAlarmGeneration = UINT32_MAX;
      publishedCoAlarmGeneration = UINT32_MAX;
      nextReconnectMs = 0;
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (mqtt.connected()) mqtt.disconnect();
      portENTER_CRITICAL(&sharedMux);
      mqttConnected = false;
      portEXIT_CRITICAL(&sharedMux);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (!mqtt.connected()) {
      const uint32_t now = millis();
      if (static_cast<int32_t>(now - nextReconnectMs) >= 0 &&
          resolveMqttHost(host, resolvedHost, sizeof(resolvedHost)) &&
          connectMqtt(resolvedHost, port, user, password)) {
        reconnectDelayMs = 1000;
        publishedAlarmGeneration = UINT32_MAX;
        publishedCoAlarmGeneration = UINT32_MAX;
        mqtt.publish(availabilityTopic, "online", true);
        publishDiscovery();
        publishTelemetry();
      } else if (!mqtt.connected() &&
                 static_cast<int32_t>(now - nextReconnectMs) >= 0) {
        nextReconnectMs = now + reconnectDelayMs;
        reconnectDelayMs = std::min<uint32_t>(60000, reconnectDelayMs * 2);
      }
    }

    const bool connected = mqtt.connected();
    portENTER_CRITICAL(&sharedMux);
    mqttConnected = connected;
    portEXIT_CRITICAL(&sharedMux);

    if (connected) {
      mqtt.loop();
      bool alarmState;
      bool coAlarmState;
      uint32_t generation;
      uint32_t coGeneration;
      portENTER_CRITICAL(&sharedMux);
      alarmState = desiredAlarmState;
      coAlarmState = desiredCoAlarmState;
      generation = alarmGeneration;
      coGeneration = coAlarmGeneration;
      portEXIT_CRITICAL(&sharedMux);
      if (generation != publishedAlarmGeneration &&
          mqtt.publish(stateTopic, alarmState ? "ALARM" : "OK", true)) {
        publishedAlarmGeneration = generation;
        if (alarmState) {
          portENTER_CRITICAL(&sharedMux);
          if (desiredAlarmState && alarmGeneration == generation &&
              alarmOnPublishedMs == 0) {
            alarmOnPublishedMs = millis();
          }
          portEXIT_CRITICAL(&sharedMux);
        }
      }
      if (coGeneration != publishedCoAlarmGeneration &&
          mqtt.publish(coStateTopic, coAlarmState ? "ALARM" : "OK", true)) {
        publishedCoAlarmGeneration = coGeneration;
        if (coAlarmState) {
          portENTER_CRITICAL(&sharedMux);
          if (desiredCoAlarmState && coAlarmGeneration == coGeneration &&
              coAlarmOnPublishedMs == 0) {
            coAlarmOnPublishedMs = millis();
          }
          portEXIT_CRITICAL(&sharedMux);
        }
      }
      if (millis() - lastTelemetryMs >= kTelemetryIntervalMs) {
        lastTelemetryMs = millis();
        publishTelemetry();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void configureWatchdog() {
#if ESP_IDF_VERSION_MAJOR >= 5
  const esp_task_wdt_config_t config = {
      .timeout_ms = 12000,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic = true,
  };
  esp_task_wdt_init(&config);
#else
  esp_task_wdt_init(12, true);
#endif
  esp_task_wdt_add(nullptr);
}

void restartMicrophone() {
  portENTER_CRITICAL(&sharedMux);
  micHealthy = false;
  portEXIT_CRITICAL(&sharedMux);
  M5Cardputer.Mic.end();
  delay(30);
  M5Cardputer.Speaker.end();
  M5Cardputer.Mic.begin();
  failedAudioReads = 0;
  silentAudioBlocks = 0;
}

void drawUi() {
  AlarmDetectorReading reading;
  bool healthy;
  bool broker;
  bool alarm;
  bool coAlarm;
  portENTER_CRITICAL(&sharedMux);
  reading = latestReading;
  healthy = micHealthy;
  broker = mqttConnected;
  alarm = desiredAlarmState;
  coAlarm = desiredCoAlarmState;
  portEXIT_CRITICAL(&sharedMux);

  auto& display = M5Cardputer.Display;
  const uint16_t background = (alarm || coAlarm) ? TFT_RED : TFT_BLACK;
  display.fillScreen(background);
  display.setTextColor(TFT_WHITE, background);
  display.setTextSize(1);
  display.setCursor(5, 5);
  const char* headline = alarm ? "FIRE ALARM" : (coAlarm ? "CO ALARM" : "LISTENING");
  display.printf("ALARM LISTENER  %s\n", headline);
  display.printf("Mic: %-5s  Tone: %s  Bursts: %u/4\n",
                 healthy ? "OK" : "FAULT", reading.tone ? "YES" : "no",
                 reading.acceptedBursts);
  display.printf("Level: %5.0f  floor: %5.0f\n", reading.rms,
                 reading.noiseFloor);
  display.printf("Peak: %4.0f Hz  quality: %2.0f%%\n",
                 reading.peakFrequency, reading.tonalRatio * 100.0f);
  if (WiFi.status() == WL_CONNECTED) {
    display.printf("WiFi: OK  IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    display.printf("WiFi: down  reason: %u\n", lastWiFiDisconnectReason);
  }
  display.printf("HA/MQTT: %s\n", broker ? "OK" : "down");
  if (portalActive) {
    display.setTextColor(TFT_YELLOW, background);
    display.printf("Setup: %s\nPass: %s\n", apName, apPassword);
  } else {
    display.printf("Hold G0 for setup\n");
  }
}

void wakeScreen(uint8_t brightness = kNormalBrightness) {
  if (!screenAwake) M5Cardputer.Display.wakeup();
  M5Cardputer.Display.setBrightness(brightness);
  screenAwake = true;
  lastInteractionMs = millis();
  lastUiMs = 0;
}

void manageScreen(bool alarm) {
  if (alarm) {
    if (!alarmScreenActive) {
      wakeScreen(255);
      alarmScreenActive = true;
    } else {
      lastInteractionMs = millis();
    }
    return;
  }
  if (alarmScreenActive) {
    M5Cardputer.Display.setBrightness(kNormalBrightness);
    alarmScreenActive = false;
    lastInteractionMs = millis();
  }
  if (portalActive) {
    if (!screenAwake) wakeScreen();
    return;
  }
  if (screenAwake && millis() - lastInteractionMs >= kDisplaySleepMs) {
    M5Cardputer.Display.sleep();
    screenAwake = false;
  }
}

void logStatus() {
  AlarmDetectorReading reading;
  bool healthy;
  bool broker;
  bool alarm;
  bool coAlarm;
  portENTER_CRITICAL(&sharedMux);
  reading = latestReading;
  healthy = micHealthy;
  broker = mqttConnected;
  alarm = desiredAlarmState;
  coAlarm = desiredCoAlarmState;
  portEXIT_CRITICAL(&sharedMux);
  Serial.printf(
      "STATUS mic=%s rms=%.1f floor=%.1f peak=%.0fHz quality=%.3f "
      "tone=%d bursts=%u smoke=%d co=%d wifi=%d wl=%d reason=%u ip=%s mqtt=%d "
      "heap=%u\n",
      healthy ? "ok" : "fault", reading.rms, reading.noiseFloor,
      reading.peakFrequency, reading.tonalRatio, reading.tone,
      reading.acceptedBursts, alarm, coAlarm, WiFi.status() == WL_CONNECTED,
      static_cast<int>(WiFi.status()), lastWiFiDisconnectReason,
      WiFi.localIP().toString().c_str(), broker,
      ESP.getFreeHeap());
}
}  // namespace

void setup() {
  Serial.begin(115200);
  auto m5Config = M5.config();
  M5Cardputer.begin(m5Config);
  M5Cardputer.Speaker.end();
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(kNormalBrightness);
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.setCursor(5, 5);
  M5Cardputer.Display.println("Starting smoke listener...");

  loadSettings();
  const uint64_t chipId = ESP.getEfuseMac();
  std::snprintf(deviceId, sizeof(deviceId), "cardputer-alarm-%06lx",
                static_cast<unsigned long>(chipId & 0xFFFFFF));
  std::snprintf(apName, sizeof(apName), "AlarmListener-%06lX",
                static_cast<unsigned long>(chipId & 0xFFFFFF));
  std::snprintf(apPassword, sizeof(apPassword), "alarm-%06lx",
                static_cast<unsigned long>(chipId & 0xFFFFFF));
  std::snprintf(stateTopic, sizeof(stateTopic), "%s/smoke", deviceId);
  std::snprintf(coStateTopic, sizeof(coStateTopic), "%s/carbon_monoxide",
                deviceId);
  std::snprintf(availabilityTopic, sizeof(availabilityTopic), "%s/status",
                deviceId);
  std::snprintf(telemetryTopic, sizeof(telemetryTopic), "%s/telemetry",
                deviceId);
  std::snprintf(discoveryTopic, sizeof(discoveryTopic),
                "homeassistant/binary_sensor/%s/smoke/config", deviceId);
  std::snprintf(coDiscoveryTopic, sizeof(coDiscoveryTopic),
                "homeassistant/binary_sensor/%s/carbon_monoxide/config",
                deviceId);
  Serial.printf("BOOT %s firmware=%s direct_boot=1\n", deviceId,
                kFirmwareVersion);

  mqtt.setBufferSize(1536);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(2);

  mqttHostParameter =
      new WiFiManagerParameter("mqtt_host", "MQTT broker IP/host", mqttHost, 64);
  mqttPortParameter =
      new WiFiManagerParameter("mqtt_port", "MQTT port", mqttPortText, 6);
  mqttUserParameter =
      new WiFiManagerParameter("mqtt_user", "MQTT username", mqttUser, 64);
  mqttPasswordParameter = new WiFiManagerParameter(
      "mqtt_pass", "MQTT password", mqttPassword, 64, "type='password'");
  wifiManager.addParameter(mqttHostParameter);
  wifiManager.addParameter(mqttPortParameter);
  wifiManager.addParameter(mqttUserParameter);
  wifiManager.addParameter(mqttPasswordParameter);
  wifiManager.setSaveParamsCallback(savePortalSettings);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConnectTimeout(10);
  wifiManager.setHostname(deviceId);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  WiFi.begin(DeviceConfig::kWifiSsid, DeviceConfig::kWifiPassword);

  M5Cardputer.Mic.begin();
  lastDetectionMs = millis();
  lastCoDetectionMs = millis();
  lastInteractionMs = millis();
  if (portalActive) {
    Serial.printf("SETUP ap=%s password=%s url=http://192.168.4.1\n", apName,
                  apPassword);
  }

  xTaskCreatePinnedToCore(mqttTask, "mqtt", 6144, nullptr, 1, nullptr, 0);
  configureWatchdog();
}

void loop() {
  esp_task_wdt_reset();
  M5Cardputer.update();
  if (M5Cardputer.BtnA.wasPressed() || M5Cardputer.Keyboard.isChange()) {
    wakeScreen();
  }
  if (M5Cardputer.BtnA.wasHold()) startConfigPortal();

  if (portalActive) {
    wifiManager.process();
    if (portalCloseRequested) {
      if (portalSavedMs == 0) portalSavedMs = millis();
      if (WiFi.status() == WL_CONNECTED && millis() - portalSavedMs > 1500) {
        wifiManager.stopConfigPortal();
        portalActive = false;
        portalCloseRequested = false;
        portalSavedMs = 0;
      }
    }
  }

  if (!M5Cardputer.Mic.record(audioBlock, AlarmDetector::kBlockSamples,
                              AlarmDetector::kSampleRate)) {
    ++failedAudioReads;
    if (failedAudioReads > 1000) restartMicrophone();
    delay(2);
  } else {
    failedAudioReads = 0;
    const AlarmDetectorReading reading =
        detector.process(audioBlock, AlarmDetector::kBlockSamples, millis());
    if (reading.rms < 1.0f) {
      ++silentAudioBlocks;
    } else {
      silentAudioBlocks = 0;
    }
    portENTER_CRITICAL(&sharedMux);
    latestReading = reading;
    micHealthy = silentAudioBlocks < 500;
    portEXIT_CRITICAL(&sharedMux);

    static uint32_t lastFrameLogMs = 0;
    if (reading.rms > 2.0f * reading.noiseFloor &&
        millis() - lastFrameLogMs >= 100) {
      lastFrameLogMs = millis();
      Serial.printf("FRAME rms=%.1f floor=%.1f peak=%.0fHz quality=%.3f tone=%d "
                    "bursts=%u\n",
                    reading.rms, reading.noiseFloor, reading.peakFrequency,
                    reading.tonalRatio, reading.tone, reading.acceptedBursts);
    }

    if (reading.tone != previousToneState ||
        reading.acceptedBursts != previousAcceptedBursts || reading.alarm ||
        reading.carbonMonoxideAlarm) {
      Serial.printf(
          "DETECT tone=%d bursts=%u rms=%.1f peak=%.0fHz quality=%.3f "
          "smoke_event=%d co_event=%d\n",
          reading.tone, reading.acceptedBursts, reading.rms,
          reading.peakFrequency, reading.tonalRatio, reading.alarm,
          reading.carbonMonoxideAlarm);
      previousToneState = reading.tone;
      previousAcceptedBursts = reading.acceptedBursts;
    }

    if (reading.alarm) {
      lastDetectionMs = millis();
      setDesiredAlarm(true);
    }
    if (reading.carbonMonoxideAlarm) {
      lastCoDetectionMs = millis();
      setDesiredCoAlarm(true);
    }
    if (silentAudioBlocks >= 500) restartMicrophone();
  }

  bool alarm;
  bool coAlarm;
  uint32_t publishedMs;
  uint32_t coPublishedMs;
  portENTER_CRITICAL(&sharedMux);
  alarm = desiredAlarmState;
  coAlarm = desiredCoAlarmState;
  publishedMs = alarmOnPublishedMs;
  coPublishedMs = coAlarmOnPublishedMs;
  portEXIT_CRITICAL(&sharedMux);
  if (alarm && publishedMs != 0 &&
      millis() - lastDetectionMs >= kAlarmQuietBeforeClearMs &&
      millis() - publishedMs >= kMinimumVisibleAlarmMs) {
    setDesiredAlarm(false);
  }
  if (coAlarm && coPublishedMs != 0 &&
      millis() - lastCoDetectionMs >= kAlarmQuietBeforeClearMs &&
      millis() - coPublishedMs >= kMinimumVisibleAlarmMs) {
    setDesiredCoAlarm(false);
  }

  if (millis() - lastUiMs >= kUiIntervalMs) {
    lastUiMs = millis();
    if (screenAwake) drawUi();
  }
  if (millis() - lastSerialMs >= 2000) {
    lastSerialMs = millis();
    logStatus();
  }
  portENTER_CRITICAL(&sharedMux);
  alarm = desiredAlarmState || desiredCoAlarmState;
  portEXIT_CRITICAL(&sharedMux);
  manageScreen(alarm);
}
