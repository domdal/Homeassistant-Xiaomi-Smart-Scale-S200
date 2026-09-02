#pragma once
// Copy this file to include/secrets.h and fill in your credentials.

// ---------------- WiFi ----------------
#define WIFI_SSID     "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

// ---------------- MQTT ----------------
#define MQTT_HOST         "192.168.1.10"
#define MQTT_PORT         1883
#define MQTT_USER         ""
#define MQTT_PASSWORD     ""
#define MQTT_CLIENT_ID    "esp32-s200-scale-bridge"
#define MQTT_TOPIC_WEIGHT "home/scale/weight"

// ---------------- Xiaomi Smart Scale S200 ----------------
#define SCALE_MAC         "aa:bb:cc:dd:ee:ff"
#define SCALE_BINDKEY_HEX "your-32-character-bindkey"
