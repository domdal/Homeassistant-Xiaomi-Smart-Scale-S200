// Native Zigbee backend: joins your existing Zigbee mesh directly over the
// ESP32-C6's built-in 802.15.4 radio and reports weight as a standard ZCL
// Analog Input, picked up by zigbee2mqtt. No WiFi, no MQTT broker, no
// secrets.h WIFI_*/MQTT_* fields needed for this backend.
//
// Why this exists alongside main_wifi.cpp: the WiFi/MQTT backend's
// reconnect churn (see that file's ensureWifiConnected()/
// ensureMqttConnected()/checkHealth() comments) traces back to BLE+WiFi
// radio coexistence on a weak WiFi link. The ESP32-C6 explicitly supports
// BLE + 802.15.4 (Zigbee) coexistence as one of its designed use cases, so
// this backend sidesteps that whole class of problem - at the cost of
// needing a Zigbee coordinator (zigbee2mqtt) and a one-time pairing step.
//
// Select this backend with the "esp32-c6-devkitc-1-zigbee" PlatformIO
// environment (see platformio.ini) - it sets the required
// board_build.partitions=zigbee_zczr.csv and -DZIGBEE_MODE_ZCZR build flag.
//
// First boot: zigbee2mqtt won't recognize this as a known device model, so
// it'll show up under "unsupported devices" exposing the raw genAnalogInput
// cluster. Add a small zigbee2mqtt external converter to map that to a
// friendly "weight" (kg) property - see the project README.
#include <Arduino.h>
#ifndef ZIGBEE_MODE_ZCZR
#error "Zigbee coordinator/router mode not selected - build with the esp32-c6-devkitc-1-zigbee PlatformIO environment"
#endif
#include "Zigbee.h"
#include "esp_task_wdt.h"
#include "scale_common.h"

// One endpoint, one Analog Input cluster carrying the weight in kg. There's
// no ZCL "mass" application type, so ESP_ZB_ZCL_AI_APP_TYPE_OTHER plus a
// human-readable description is the closest fit - z2m/HA just read
// presentValue and the description/unit either way.
static const uint8_t SCALE_ENDPOINT_NUMBER = 1;
static ZigbeeAnalog zbScale(SCALE_ENDPOINT_NUMBER);

// ---------------------------------------------------------------------
// Status LED + factory-reset button - both specific to the
// ESP32-C6-DevKitC-1: GPIO9 is its BOOT button (already has a board-level
// pull-up, since it doubles as a strapping pin), and RGB_BUILTIN/GPIO8 is
// its onboard addressable status LED. Adjust both if you're on different
// hardware.
// ---------------------------------------------------------------------
static const uint8_t FACTORY_RESET_BUTTON_PIN = 9;
static const unsigned long FACTORY_RESET_HOLD_MS = 3000;

// Non-blocking "flash for N ms then off" - loop() calls serviceLed() every
// iteration to turn it off once the duration elapses, so triggering a
// flash never blocks BLE/Zigbee servicing.
static unsigned long g_ledOffAtMs = 0;
static bool g_ledFlashing = false;

static void flashLed(uint8_t r, uint8_t g, uint8_t b, unsigned long durationMs) {
  rgbLedWrite(RGB_BUILTIN, r, g, b);
  g_ledOffAtMs = millis() + durationMs;
  g_ledFlashing = true;
}

static void serviceLed() {
  if (g_ledFlashing && millis() >= g_ledOffAtMs) {
    rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
    g_ledFlashing = false;
  }
}

// Hold the BOOT button for FACTORY_RESET_HOLD_MS to wipe the device's
// Zigbee network membership and force a genuine rejoin - the on-demand
// replacement for the one-off erase_nvs=true hack this used during
// development. Needed any time z2m no longer recognizes this device (e.g.
// after removing/re-adding it there), since Zigbee.begin()'s default
// (erase_nvs=false) otherwise just trusts whatever's already in flash and
// silently skips a real join.
static void checkFactoryResetButton() {
  if (digitalRead(FACTORY_RESET_BUTTON_PIN) != LOW) return; // not pressed

  Serial.println("BOOT button held - keep holding 3s to factory-reset Zigbee and rejoin...");
  unsigned long pressStartMs = millis();
  while (digitalRead(FACTORY_RESET_BUTTON_PIN) == LOW) {
    esp_task_wdt_reset(); // this loop blocks for up to FACTORY_RESET_HOLD_MS
    unsigned long heldMs = millis() - pressStartMs;
    rgbLedWrite(RGB_BUILTIN, (heldMs % 200 < 100) ? 64 : 0, 0, 0); // rapid red blink while armed
    if (heldMs > FACTORY_RESET_HOLD_MS) {
      Serial.println("Held long enough - factory-resetting Zigbee and rebooting...");
      rgbLedWrite(RGB_BUILTIN, 64, 0, 0);
      delay(300);
      Zigbee.factoryReset(); // erases stored network state and restarts (default restart=true)
    }
    delay(20);
  }
  // Released before the threshold - abort, no reset.
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
  Serial.println("BOOT button released early - factory reset cancelled.");
}

// Registered via setWeightCallback() - runs on the *BLE host task*
// (scale_common.cpp's onResult -> handlePacket), not loop(). Same rationale
// as the WiFi/MQTT backend's publishWeightMqtt(): park the value here and
// let loop() do the actual Zigbee call, rather than driving the Zigbee
// stack from two tasks at once.
static volatile float g_pendingWeightKg = 0.0f;
static volatile bool g_hasPendingWeight = false;

static void onWeightMeasured(float weightKg) {
  g_pendingWeightKg = weightKg;
  g_hasPendingWeight = true; // set last: loop() only reads the value once this is true
}

static void flushPendingWeight() {
  if (!g_hasPendingWeight) return;
  g_hasPendingWeight = false;

  float weightKg = g_pendingWeightKg;
  zbScale.setAnalogInput(weightKg);
  zbScale.reportAnalogInput(); // force an immediate report - don't wait for the periodic one below
  Serial.printf("Zigbee: reported %.2f kg\n", weightKg);
  flashLed(255, 255, 255, 200); // brief white flash: a weighing event was reported
}

// ---------------------------------------------------------------------
// Health watchdog - same rationale as main_wifi.cpp's checkHealth(): the
// task watchdog only catches a hung loop(), not a loop() that's alive but
// whose network stack underneath has quietly died. Zigbee.connected() is
// the equivalent of that backend's mqttClient.connected() check.
// ---------------------------------------------------------------------
static const uint32_t MIN_FREE_HEAP_BYTES = 25000;
static const unsigned long MAX_ZIGBEE_DOWNTIME_MS = 30UL * 60UL * 1000UL; // 30 min
static unsigned long g_lastZigbeeOkMs = 0;

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
  if (Zigbee.connected()) {
    g_lastZigbeeOkMs = now;
    return;
  }
  if (now - g_lastZigbeeOkMs > MAX_ZIGBEE_DOWNTIME_MS) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Zigbee unreachable for %lus", (unsigned long)((now - g_lastZigbeeOkMs) / 1000));
    restartWithReason(msg);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Xiaomi Scale S200 BLE listener starting (NimBLE-Arduino, Zigbee backend)...");
#if ENABLE_CRASH_LOG
  recordAndDumpBootHistory();
#endif

  decodeSecrets();
  setupWatchdog();

  pinMode(FACTORY_RESET_BUTTON_PIN, INPUT_PULLUP);

  zbScale.setManufacturerAndModel("Xiaomi (bridged via ESP32)", "Scale S200");
  zbScale.addAnalogInput();
  zbScale.setAnalogInputApplication(ESP_ZB_ZCL_AI_APP_TYPE_OTHER);
  zbScale.setAnalogInputDescription("Weight (kg)");
  zbScale.setAnalogInputResolution(0.01);
  zbScale.setAnalogInputMinMax(0, 200); // sanity range for z2m/HA - adjust if needed

  Zigbee.addEndpoint(&zbScale);

  Serial.println("Starting Zigbee...");
  // ZIGBEE_ROUTER, not ZIGBEE_END_DEVICE: this board is USB-powered and
  // always on (never sleeps), so it can stay awake as a mesh router,
  // extending Zigbee range for other devices too instead of just consuming
  // it as a sleepy end device.
  //
  // erase_nvs defaults to false here - it trusts whatever network state is
  // already stored in flash and reconnects to it, rather than joining
  // fresh every boot. If z2m ever stops recognizing this device (e.g.
  // after removing/re-adding it there, which leaves this board still
  // believing it's joined to a network the coordinator no longer
  // remembers), hold the BOOT button for 3s (see checkFactoryResetButton())
  // to force a clean rejoin instead of reflashing.
  if (!Zigbee.begin(ZIGBEE_ROUTER)) {
    // Only fails on a stack/config problem, not "network not found yet" -
    // that's what the connected() wait below is for - so this isn't
    // recoverable without a reboot.
    Serial.println("Zigbee failed to start - rebooting");
    ESP.restart();
  }

  Serial.print("Connecting to Zigbee network");
  while (!Zigbee.connected()) {
    // First pairing needs zigbee2mqtt's "permit join" enabled and can take
    // a while (minutes, if you have to go flip that switch) - keep feeding
    // the watchdog setupWatchdog() just armed above, or a slow join trips
    // it and reboots us mid-wait.
    esp_task_wdt_reset();
    Serial.print(".");
    // Slow blue blink while searching/joining - toggles every ~250ms.
    rgbLedWrite(RGB_BUILTIN, 0, 0, (millis() % 500 < 250) ? 64 : 0);
    delay(100);
  }
  Serial.println(" connected.");
  // Brief green flashes to confirm the join, then back to off - the LED
  // stays dark during normal idle operation and only lights up again for
  // checkFactoryResetButton() (red) or a weighing event (white flash).
  for (int i = 0; i < 3; i++) {
    rgbLedWrite(RGB_BUILTIN, 0, 64, 0);
    delay(150);
    rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
    delay(150);
  }

  // Report at least every 10 minutes even if unchanged, so z2m/HA don't
  // mark it stale between weigh-ins - flushPendingWeight() above already
  // forces an immediate report on every actual measurement regardless.
  zbScale.setAnalogInputReporting(0, 600, 0.1);

  // zigbee2mqtt's post-join "interview" (Active Endpoint / Simple
  // Descriptor / Basic cluster reads) needs several prompt ZDO/ZCL
  // request-response round trips right after Zigbee.connected() returns.
  // Give it a clear window before introducing any BLE radio activity: a
  // few seconds' grace here is cheap, whereas an interview that fails
  // (device stuck as "unsupported", model/vendor blank) is not - it
  // usually means removing the device from z2m and re-pairing.
  esp_task_wdt_reset();
  delay(5000);

  setWeightCallback(onWeightMeasured);
  // Much lower duty cycle than main_wifi.cpp's default (~99%): the
  // ESP32-C6 has one 2.4GHz radio shared (time-multiplexed) between BLE
  // and 802.15.4/Zigbee, and a near-continuous BLE scan starves Zigbee of
  // airtime badly enough to fail interviews and, later, routine z2m
  // availability pings/mesh traffic - the exact kind of connectivity
  // flakiness this backend exists to avoid. ~30% duty (scanning every
  // 100ms) still reliably catches the scale, which repeats its
  // advertisement several times per weigh-in.
  startBleScan(/*intervalUnits=*/160, /*windowUnits=*/48);

  Serial.printf("Listening for %s ... step on the scale.\n", TARGET_ADDRESS);
}

void loop() {
  esp_task_wdt_reset();
  checkFactoryResetButton(); // blocks for up to ~3s if the BOOT button is held
  flushPendingWeight();      // publish any reading the BLE task parked for us
  serviceLed();               // turns off a flashLed() flash once its duration elapses
  checkHealth();              // catches the "loop() alive but network dead" wedge the task watchdog can't see
#if ENABLE_CRASH_LOG
  updateBootHeartbeat(); // keeps the persistent crash-history log current (see recordAndDumpBootHistory())
#endif
  esp_task_wdt_reset();
  delay(10);
}
