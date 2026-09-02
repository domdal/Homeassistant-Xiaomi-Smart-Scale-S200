#include <Arduino.h>
#include <NimBLEDevice.h>
#include "mbedtls/ccm.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

// ---------------------------------------------------------------------
// Configuration - match these to your own scale (see the Python script
// this was ported from for how to obtain the bindkey). SCALE_MAC and
// SCALE_BINDKEY_HEX live in secrets.h; decodeSecrets() (called from
// setup()) turns them into the byte forms used below.
// ---------------------------------------------------------------------
static const char *TARGET_ADDRESS = SCALE_MAC; // scale's BLE MAC, "d0:7b:6f:5c:06:a6"-style

static uint8_t BINDKEY[16];

// Fallback MAC bytes as they appear *inside* the MiBeacon payload (reversed
// byte order vs. the human-readable SCALE_MAC address above). Only used if
// a given frame omits mac_include.
static uint8_t EXPECTED_MAC_BYTES[6];

static const uint16_t FE95_UUID16 = 0xFE95;

// Parses SCALE_BINDKEY_HEX into BINDKEY and SCALE_MAC into
// EXPECTED_MAC_BYTES (reversed - see comment above).
static void decodeSecrets() {
  for (size_t i = 0; i < sizeof(BINDKEY); i++) {
    unsigned int byte;
    sscanf(SCALE_BINDKEY_HEX + i * 2, "%2x", &byte);
    BINDKEY[i] = (uint8_t)byte;
  }
  for (size_t i = 0; i < sizeof(EXPECTED_MAC_BYTES); i++) {
    unsigned int byte;
    sscanf(SCALE_MAC + i * 3, "%2x", &byte); // "d0:7b:6f:..." - 3 chars per octet
    EXPECTED_MAC_BYTES[sizeof(EXPECTED_MAC_BYTES) - 1 - i] = (uint8_t)byte;
  }
}
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// MQTT / Home Assistant discovery. WIFI_* and MQTT_* come from secrets.h.
// ---------------------------------------------------------------------
static const char *HA_DISCOVERY_PREFIX = "homeassistant";
static const char *MQTT_TOPIC_AVAILABILITY = MQTT_CLIENT_ID "/status";
static const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

static void publishWeightMqtt(float weightKg);
// ---------------------------------------------------------------------

static String lastHex = "";

struct FrCtrl {
  uint8_t object_include;
  uint8_t capability_include;
  uint8_t mac_include;
  uint8_t is_encrypted;
};

static FrCtrl parseFrCtrl(uint16_t frctrl) {
  FrCtrl f;
  f.object_include = (frctrl >> 6) & 0x01;
  f.capability_include = (frctrl >> 5) & 0x01;
  f.mac_include = (frctrl >> 4) & 0x01;
  f.is_encrypted = (frctrl >> 3) & 0x01;
  return f;
}

static String toHex(const uint8_t *buf, size_t len) {
  static const char *hexd = "0123456789abcdef";
  String s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    s += hexd[(buf[i] >> 4) & 0xF];
    s += hexd[buf[i] & 0xF];
  }
  return s;
}

// Decrypts a MiBeacon encrypted object. blob layout: [ciphertext][ext_cnt(3)][mic(4)]
static bool decryptObject(const uint8_t *blob, size_t blobLen,
                           const uint8_t *macBytes, const uint8_t *productId,
                           uint8_t frameCntByte, uint8_t *out, size_t &outLen) {
  if (blobLen < 7) return false;

  size_t cipherLen = blobLen - 7;
  const uint8_t *extCnt = blob + cipherLen;   // 3 bytes
  const uint8_t *mic = blob + cipherLen + 3;  // 4 bytes
  const uint8_t *ciphertext = blob;

  uint8_t nonce[12];
  memcpy(nonce, macBytes, 6);
  memcpy(nonce + 6, productId, 2);
  nonce[8] = frameCntByte;
  memcpy(nonce + 9, extCnt, 3);

  static const uint8_t aad[1] = {0x11};

  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, BINDKEY, 128);
  if (rc != 0) {
    mbedtls_ccm_free(&ctx);
    return false;
  }

  rc = mbedtls_ccm_auth_decrypt(&ctx, cipherLen, nonce, sizeof(nonce), aad,
                                 sizeof(aad), ciphertext, out, mic, 4);
  mbedtls_ccm_free(&ctx);
  if (rc != 0) return false;

  outLen = cipherLen;
  return true;
}

static void handlePacket(const uint8_t *data, size_t len) {
  if (len < 5) return;

  uint16_t frctrl = data[0] | ((uint16_t)data[1] << 8);
  const uint8_t *productId = data + 2;
  uint8_t frameCnt = data[4];
  FrCtrl flags = parseFrCtrl(frctrl);

  size_t idx = 5;
  const uint8_t *macBytes = nullptr;
  if (flags.mac_include) {
    if (len < idx + 6) return;
    macBytes = data + idx;
    idx += 6;
  }
  if (flags.capability_include) {
    idx += 1;
  }

  if (!flags.object_include) {
    Serial.printf("[%10lu] idle beacon (no measurement) - %s\n", millis(), toHex(data, len).c_str());
    return;
  }

  if (macBytes == nullptr) {
    macBytes = EXPECTED_MAC_BYTES;
  }

  if (idx > len) return;
  const uint8_t *remainder = data + idx;
  size_t remainderLen = len - idx;

  if (!flags.is_encrypted) {
    Serial.printf("[%10lu] unencrypted object - %s\n", millis(), toHex(remainder, remainderLen).c_str());
    return;
  }

  uint8_t plaintext[64];
  size_t plainLen = 0;
  if (remainderLen < 7 || remainderLen - 7 > sizeof(plaintext)) {
    Serial.printf("[%10lu] object size out of range (%u bytes)\n", millis(), (unsigned)remainderLen);
    return;
  }
  if (!decryptObject(remainder, remainderLen, macBytes, productId, frameCnt, plaintext, plainLen)) {
    Serial.printf("[%10lu] decryption FAILED for %s\n", millis(), toHex(data, len).c_str());
    return;
  }

  size_t i = 0;
  while (i + 3 <= plainLen) {
    uint16_t objId = plaintext[i] | ((uint16_t)plaintext[i + 1] << 8);
    uint8_t objLen = plaintext[i + 2];
    i += 3;
    if (i + objLen > plainLen) break;
    const uint8_t *objVal = plaintext + i;
    i += objLen;

    if (objId == 0x4e16 && objLen >= 3) {
      float weightKg = ((uint16_t)objVal[1] | ((uint16_t)objVal[2] << 8)) / 100.0f;
      // Machine-friendly line first, human-friendly detail second.
      Serial.printf("WEIGHT_KG=%.2f\n", weightKg);
      Serial.printf("[%10lu] WEIGHT = %.2f kg   (raw obj=%s)\n", millis(), weightKg, toHex(objVal, objLen).c_str());
      publishWeightMqtt(weightKg);
    } else {
      Serial.printf("[%10lu] obj_id=0x%04x value=%s\n", millis(), objId, toHex(objVal, objLen).c_str());
    }
  }
}

// Scans a raw BLE advertisement payload (sequence of [len][type][data...] AD
// structures) for a "Service Data - 16-bit UUID" (type 0x16) entry matching
// wantUuid. Returns a pointer/length into the buffer passed in, so the
// caller must keep that buffer alive for as long as the pointer is used.
static bool findServiceData16(const uint8_t *payload, size_t payloadLen, uint16_t wantUuid,
                               const uint8_t **outData, size_t *outLen) {
  size_t i = 0;
  while (i < payloadLen) {
    uint8_t adLen = payload[i];
    if (adLen == 0) break;
    if (i + 1 + adLen > payloadLen) break; // malformed / truncated AD structure

    uint8_t adType = payload[i + 1];
    const uint8_t *adData = payload + i + 2;
    size_t adDataLen = adLen - 1; // adLen counts the type byte too

    if (adType == 0x16 && adDataLen >= 2) { // Service Data - 16-bit UUID
      uint16_t uuid = adData[0] | ((uint16_t)adData[1] << 8);
      if (uuid == wantUuid) {
        *outData = adData + 2;
        *outLen = adDataLen - 2;
        return true;
      }
    }
    i += 1 + adLen;
  }
  return false;
}

#define DEBUG_LOG_ALL_ADVERTISEMENTS 0

class ScaleCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override {
    String addr = advertisedDevice->getAddress().toString().c_str();
    addr.toLowerCase();

#if DEBUG_LOG_ALL_ADVERTISEMENTS
    Serial.printf("[%10lu] seen %s (type=%d) rssi=%d\n", millis(), addr.c_str(),
                  advertisedDevice->getAddress().getType(), advertisedDevice->getRSSI());
#endif

    // Compare the address as a string instead of NimBLEAddress::equals(),
    // which also compares the address TYPE (public vs random). Guessing the
    // wrong type here means every packet gets silently dropped with zero
    // output - which is exactly the "nothing prints" symptom. Comparing the
    // plain string sidesteps that entirely.
    if (addr != TARGET_ADDRESS) {
      return;
    }

    const std::vector<uint8_t> &payload = advertisedDevice->getPayload();
    if (payload.empty()) {
      return;
    }

    const uint8_t *data = nullptr;
    size_t len = 0;
    if (!findServiceData16(payload.data(), payload.size(), FE95_UUID16, &data, &len)) {
      return;
    }

    String h = toHex(data, len);
    if (h == lastHex) return; // skip pure repeats of the exact same advertisement
    lastHex = h;

    handlePacket(data, len);
  }
};

NimBLEScan *pBLEScan;

// Publishes the Home Assistant MQTT Discovery config for the weight sensor
// (retained, so HA picks it up - and re-picks it up after an HA restart -
// without any manual YAML). See https://www.home-assistant.io/integrations/mqtt/#discovery-messages
static void mqttPublishDiscovery() {
  JsonDocument doc;
  doc["name"] = "Weight";
  doc["unique_id"] = MQTT_CLIENT_ID "_weight";
  doc["state_topic"] = MQTT_TOPIC_WEIGHT;
  doc["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
  doc["unit_of_measurement"] = "kg";
  doc["device_class"] = "weight";
  doc["state_class"] = "measurement";

  JsonObject device = doc["device"].to<JsonObject>();
  device["identifiers"].to<JsonArray>().add(MQTT_CLIENT_ID);
  device["name"] = "Xiaomi Scale S200 Bridge";
  device["manufacturer"] = "Xiaomi (bridged via ESP32)";
  device["model"] = "Scale S200";

  char topic[96];
  snprintf(topic, sizeof(topic), "%s/sensor/%s/weight/config", HA_DISCOVERY_PREFIX, MQTT_CLIENT_ID);

  char payload[512];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  mqttClient.publish(topic, (const uint8_t *)payload, n, /*retained=*/true);
}

static void publishWeightMqtt(float weightKg) {
  if (!mqttClient.connected()) return;
  char payload[16];
  snprintf(payload, sizeof(payload), "%.2f", weightKg);
  // Retained: the broker holds this as the topic's last value, so a
  // reconnecting/restarting HA (or a fresh subscriber) gets the last known
  // weight immediately instead of showing "unknown" until the next weigh-in.
  mqttClient.publish(MQTT_TOPIC_WEIGHT, payload, /*retained=*/true);
}

static void ensureWifiConnected() {
  static unsigned long lastAttemptMs = 0;
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastAttemptMs < MQTT_RECONNECT_INTERVAL_MS) return;
  lastAttemptMs = now;

  Serial.println("WiFi: connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static void ensureMqttConnected() {
  static unsigned long lastAttemptMs = 0;
  if (mqttClient.connected()) return;

  unsigned long now = millis();
  if (now - lastAttemptMs < MQTT_RECONNECT_INTERVAL_MS) return;
  lastAttemptMs = now;

  Serial.println("MQTT: connecting...");
  // Last Will + Testament: broker marks us "offline" (retained) if we drop
  // off without a clean disconnect, so the HA entity shows unavailable.
  bool ok = strlen(MQTT_USER) > 0
                ? mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD,
                                      MQTT_TOPIC_AVAILABILITY, 0, true, "offline")
                : mqttClient.connect(MQTT_CLIENT_ID, MQTT_TOPIC_AVAILABILITY, 0, true, "offline");

  if (!ok) {
    Serial.printf("MQTT: connect failed, rc=%d\n", mqttClient.state());
    return;
  }

  Serial.println("MQTT: connected");
  mqttClient.publish(MQTT_TOPIC_AVAILABILITY, "online", /*retained=*/true);
  mqttPublishDiscovery();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Xiaomi Scale S200 BLE listener starting (NimBLE-Arduino)...");

  decodeSecrets();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(512); // discovery JSON is bigger than PubSubClient's 256-byte default

  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(new ScaleCallbacks(), /*wantDuplicates=*/true);
  pBLEScan->setActiveScan(false); // passive scan, matches the original Python script
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  // duration=0 means scan forever (BLE_HS_FOREVER). start() is
  // asynchronous - it kicks off the scan and returns immediately, while
  // onResult() keeps firing from the BLE host task as advertisements
  // arrive. Calling start() repeatedly from loop() (as a previous version
  // of this file did, to run "bounded bursts") doesn't work: since start()
  // returns instantly, loop() would call it again immediately, and each
  // call with restart=true (the default) tears down and re-issues the scan
  // - so the radio never got a real window to receive anything, and not
  // even other nearby devices' "seen ..." lines ever printed.
  if (!pBLEScan->start(0, false)) {
    Serial.println("ERROR: failed to start BLE scan");
  }

  Serial.printf("Listening for %s ... step on the scale.\n", TARGET_ADDRESS);
}

void loop() {
  // BLE scanning runs continuously in the background (BLE host task) and
  // doesn't need anything from loop(). WiFi/MQTT connection upkeep does.
  ensureWifiConnected();
  if (WiFi.status() == WL_CONNECTED) {
    ensureMqttConnected();
    mqttClient.loop();
  }
  delay(10);
}
