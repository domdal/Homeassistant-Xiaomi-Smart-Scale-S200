// WiFi + MQTT backend: bridges the scale straight into Home Assistant via
// MQTT Discovery. See main_zigbee.cpp for the alternative backend that
// joins zigbee2mqtt directly over the ESP32-C6's built-in 802.15.4 radio
// instead of WiFi - useful if you're far enough from the WiFi AP that this
// backend's WiFi reconnect churn becomes a problem (see the comments on
// ensureWifiConnected()/ensureMqttConnected()/checkHealth() below).
//
// Select this backend with the "esp32-c6-devkitc-1-wifi" PlatformIO
// environment (the default - see platformio.ini).
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "esp_task_wdt.h"
#include "secrets.h"
#include "scale_common.h"

// ---------------------------------------------------------------------
// MQTT / Home Assistant discovery. WIFI_* and MQTT_* come from secrets.h.
// ---------------------------------------------------------------------
static const char *HA_DISCOVERY_PREFIX = "homeassistant";
static const char *MQTT_TOPIC_AVAILABILITY = MQTT_CLIENT_ID "/status";
static const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);
// ---------------------------------------------------------------------

// Publishes the Home Assistant MQTT Discovery configs (retained, so HA
// picks them up - and re-picks them up after an HA restart - without any
// manual YAML). See https://www.home-assistant.io/integrations/mqtt/#discovery-messages
//
// Two separate entities, deliberately not linked by availability:
//  - "Weight" has no availability_topic, so it always reads as available and
//    just keeps showing its last retained value even while the ESP32 is
//    offline, instead of flipping to "unavailable" the moment it drops off
//    WiFi/MQTT.
//  - "Bridge Status" is a dedicated connectivity sensor driven by the LWT
//    topic, for anyone who wants to know/alert on the bridge itself being
//    offline without that affecting the weight reading's displayed state.
static void mqttPublishDiscovery() {
  char topic[96];
  char payload[512];

  {
    JsonDocument doc;
    doc["name"] = "Weight";
    doc["unique_id"] = MQTT_CLIENT_ID "_weight";
    doc["state_topic"] = MQTT_TOPIC_WEIGHT;
    doc["unit_of_measurement"] = "kg";
    doc["device_class"] = "weight";
    doc["state_class"] = "measurement";

    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"].to<JsonArray>().add(MQTT_CLIENT_ID);
    device["name"] = "Xiaomi Scale S200 Bridge";
    device["manufacturer"] = "Xiaomi (bridged via ESP32)";
    device["model"] = "Scale S200";

    snprintf(topic, sizeof(topic), "%s/sensor/%s/weight/config", HA_DISCOVERY_PREFIX, MQTT_CLIENT_ID);
    size_t n = serializeJson(doc, payload, sizeof(payload));
    mqttClient.publish(topic, (const uint8_t *)payload, n, /*retained=*/true);
  }

  {
    JsonDocument doc;
    doc["name"] = "Bridge Status";
    doc["unique_id"] = MQTT_CLIENT_ID "_status";
    doc["state_topic"] = MQTT_TOPIC_AVAILABILITY;
    doc["payload_on"] = "online";
    doc["payload_off"] = "offline";
    doc["device_class"] = "connectivity";
    doc["entity_category"] = "diagnostic";

    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"].to<JsonArray>().add(MQTT_CLIENT_ID);
    device["name"] = "Xiaomi Scale S200 Bridge";
    device["manufacturer"] = "Xiaomi (bridged via ESP32)";
    device["model"] = "Scale S200";

    snprintf(topic, sizeof(topic), "%s/binary_sensor/%s/status/config", HA_DISCOVERY_PREFIX, MQTT_CLIENT_ID);
    size_t n = serializeJson(doc, payload, sizeof(payload));
    mqttClient.publish(topic, (const uint8_t *)payload, n, /*retained=*/true);
  }
}

// Registered via setWeightCallback() - runs on the *BLE host task*
// (scale_common.cpp's onResult -> handlePacket), not loop(). PubSubClient
// is not thread-safe - it has a single shared packet buffer and one socket
// - so publishing directly from here could interleave with the
// connect()/loop()/ping traffic the loop task is doing and corrupt both.
// Instead the reading is parked here and flushed by the loop task in
// flushPendingWeight(). That also means a weigh-in that happens while MQTT
// is down is no longer silently thrown away: it goes out as soon as the
// broker is reachable again.
static volatile float g_pendingWeightKg = 0.0f;
static volatile bool g_hasPendingWeight = false;

static void publishWeightMqtt(float weightKg) {
  g_pendingWeightKg = weightKg;
  g_hasPendingWeight = true; // set last: the loop task only reads the value once this is true
}

static void flushPendingWeight() {
  if (!g_hasPendingWeight) return;
  if (!mqttClient.connected()) return;

  float weightKg = g_pendingWeightKg;
  char payload[16];
  snprintf(payload, sizeof(payload), "%.2f", weightKg);
  // Retained: the broker holds this as the topic's last value, so a
  // reconnecting/restarting HA (or a fresh subscriber) gets the last known
  // weight immediately instead of showing "unknown" until the next weigh-in.
  if (mqttClient.publish(MQTT_TOPIC_WEIGHT, payload, /*retained=*/true)) {
    g_hasPendingWeight = false; // keep it buffered for another try if the publish failed
    Serial.printf("MQTT: published %.2f kg\n", weightKg);
  }
}

// Exponential backoff (capped) between WiFi.begin() retries. Without this,
// a persistently out-of-range AP made the radio retry association at max
// TX power every MQTT_RECONNECT_INTERVAL_MS forever - a sustained high
// current draw (rather than a brief spike) that was tripping power-on
// resets even on a good supply. Backing off once it's clearly not working
// keeps the radio idle most of the time while still checking back
// periodically in case the AP becomes reachable again.
static const unsigned long WIFI_BACKOFF_MAX_MS = 60000;

static void ensureWifiConnected() {
  static unsigned long lastAttemptMs = 0;
  static unsigned long backoffMs = MQTT_RECONNECT_INTERVAL_MS;

  if (WiFi.status() == WL_CONNECTED) {
    backoffMs = MQTT_RECONNECT_INTERVAL_MS; // back near the AP again - retry promptly if it drops next time
    return;
  }

  unsigned long now = millis();
  if (now - lastAttemptMs < backoffMs) return;
  lastAttemptMs = now;
  backoffMs = min(backoffMs * 2, WIFI_BACKOFF_MAX_MS);

  Serial.println("WiFi: connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Same exponential backoff (capped) as ensureWifiConnected(), and for the
// same underlying reason: seen in the field hammering mqttClient.connect()
// at a fixed 5s interval for hours (rc=-2 = MQTT_CONNECT_FAILED, i.e. the
// underlying TCP connect to the broker itself failed - WiFi association can
// succeed on a weak link while the broker stays unreachable). Backing off
// stops that churn instead of retrying at full speed indefinitely.
static const unsigned long MQTT_BACKOFF_MAX_MS = 60000;

static void ensureMqttConnected() {
  static unsigned long lastAttemptMs = 0;
  static unsigned long backoffMs = MQTT_RECONNECT_INTERVAL_MS;

  if (mqttClient.connected()) {
    backoffMs = MQTT_RECONNECT_INTERVAL_MS;
    return;
  }

  unsigned long now = millis();
  if (now - lastAttemptMs < backoffMs) return;
  lastAttemptMs = now;
  backoffMs = min(backoffMs * 2, MQTT_BACKOFF_MAX_MS);

  Serial.println("MQTT: connecting...");
  // Last Will + Testament: broker marks us "offline" (retained) if we drop
  // off without a clean disconnect, so the HA entity shows unavailable.
  bool ok = strlen(MQTT_USER) > 0
                ? mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD,
                                      MQTT_TOPIC_AVAILABILITY, 0, true, "offline")
                : mqttClient.connect(MQTT_CLIENT_ID, MQTT_TOPIC_AVAILABILITY, 0, true, "offline");

  if (!ok) {
    // Free heap logged here on purpose: this is the exact retry loop seen
    // running unbounded for hours in the field while far from the AP, and
    // whether it correlates with the heap decline in the boot/crash history
    // is still unconfirmed - this makes that visible directly in the next
    // long-running serial log instead of guessing.
    Serial.printf("MQTT: connect failed, rc=%d, free heap=%lu\n", mqttClient.state(),
                  (unsigned long)ESP.getFreeHeap());
    return;
  }

  Serial.println("MQTT: connected");
  mqttClient.publish(MQTT_TOPIC_AVAILABILITY, "online", /*retained=*/true);
  mqttPublishDiscovery();
}

// ---------------------------------------------------------------------
// Health watchdog.
//
// setupWatchdog()'s task watchdog only catches a *hung loop()* - but the
// failure actually seen in the field is the opposite one: loop() keeps
// running and dutifully feeding the watchdog while the network stack
// underneath it is dead. The boot history showed free heap falling with
// uptime, floored at ~11KB, well past the point lwIP/WiFi can allocate
// buffers - the board then looks frozen to Home Assistant, never reboots
// itself, and the only recorded reset reason was the power cycle used to
// recover it.
//
// These two checks turn that silent wedge into a clean restart that also
// shows up in the boot history as "esp_restart() (software)", so the next
// serial dump says which condition tripped rather than just "power-on".
// ---------------------------------------------------------------------
static const uint32_t MIN_FREE_HEAP_BYTES = 25000;                        // well above the observed ~11KB floor
static const unsigned long MAX_MQTT_DOWNTIME_MS = 30UL * 60UL * 1000UL;   // 30 min
static unsigned long g_lastMqttOkMs = 0;

static void restartWithReason(const char *why) {
  Serial.printf("HEALTH: %s - restarting\n", why);
  Serial.flush();
#if ENABLE_CRASH_LOG
  updateBootHeartbeat(/*force=*/true); // record this run's duration/min-heap before we go
#endif
  ESP.restart();
}

static void checkHealth() {
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_BYTES) {
    char msg[64];
    snprintf(msg, sizeof(msg), "free heap down to %lu bytes", (unsigned long)freeHeap);
    restartWithReason(msg);
  }

  unsigned long now = millis();
  if (mqttClient.connected()) {
    g_lastMqttOkMs = now;
    return;
  }
  if (now - g_lastMqttOkMs > MAX_MQTT_DOWNTIME_MS) {
    char msg[64];
    snprintf(msg, sizeof(msg), "MQTT unreachable for %lus", (unsigned long)((now - g_lastMqttOkMs) / 1000));
    restartWithReason(msg);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Xiaomi Scale S200 BLE listener starting (NimBLE-Arduino, WiFi/MQTT backend)...");
#if ENABLE_CRASH_LOG
  recordAndDumpBootHistory();
#endif

  decodeSecrets();
  setupWatchdog();

  WiFi.mode(WIFI_STA);
  // persistent(false): stop the driver re-writing the same SSID/password to
  // NVS on every WiFi.begin() - with the reconnect loop below that's a flash
  // write per retry, forever, whenever the AP is out of range.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(512); // discovery JSON is bigger than PubSubClient's 256-byte default
  // PubSubClient's defaults are 15s for both, and they interact badly on a
  // weak link: connect() busy-waits for CONNACK for the full socket timeout
  // *without yielding*, which on a marginal connection blocks loop() (and
  // the watchdog feed) for 15s at a time, which in turn delays the keepalive
  // pings enough for the broker to drop us - producing exactly the reconnect
  // churn seen in HA. Short socket timeout, generous keepalive.
  mqttClient.setSocketTimeout(5);
  mqttClient.setKeepAlive(60);

  setWeightCallback(publishWeightMqtt);
  startBleScan();

  Serial.printf("Listening for %s ... step on the scale.\n", TARGET_ADDRESS);
}

void loop() {
  // BLE scanning runs continuously in the background (BLE host task) and
  // doesn't need anything from loop(). WiFi/MQTT connection upkeep does.
  ensureWifiConnected();
  esp_task_wdt_reset(); // feed before the MQTT work below, which can block for seconds on a weak link
  if (WiFi.status() == WL_CONNECTED) {
    ensureMqttConnected();
    mqttClient.loop();
    flushPendingWeight(); // publish any reading the BLE task parked for us
  }
  checkHealth(); // catches the "loop() alive but network dead" wedge the task watchdog can't see
#if ENABLE_CRASH_LOG
  updateBootHeartbeat(); // keeps the persistent crash-history log current (see recordAndDumpBootHistory())
#endif
  esp_task_wdt_reset(); // feed the watchdog - if this stops happening, setupWatchdog() reboots us
  delay(10);
}
