#pragma once

// Copy to device_config.h and fill in values for a dedicated installation.
// device_config.h is git-ignored because it contains credentials.
namespace DeviceConfig {
static constexpr char kWifiSsid[] = "YOUR_WIFI_SSID";
static constexpr char kWifiPassword[] = "YOUR_WIFI_PASSWORD";
static constexpr char kMqttHost[] = "homeassistant.local";  // No http:// prefix.
static constexpr uint16_t kMqttPort = 1883;
static constexpr char kMqttUser[] = "YOUR_MQTT_USER";
static constexpr char kMqttPassword[] = "YOUR_MQTT_PASSWORD";
}  // namespace DeviceConfig
