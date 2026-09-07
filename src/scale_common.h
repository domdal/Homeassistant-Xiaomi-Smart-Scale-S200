#pragma once
// Shared BLE scan/decrypt logic and NVS-backed boot/crash history log, used
// by both backends (main_wifi.cpp: WiFi + MQTT, main_zigbee.cpp: native
// Zigbee via zigbee2mqtt). Nothing in here knows how a weight reading
// reaches Home Assistant - see setWeightCallback() below.

#include <Arduino.h>

// Set via platformio.ini build_flags (-D ENABLE_CRASH_LOG=1/0). Gates the
// NVS-backed boot/crash history log - on by default.
#ifndef ENABLE_CRASH_LOG
#define ENABLE_CRASH_LOG 1
#endif

// The scale's BLE MAC ("d0:7b:6f:5c:06:a6"-style), from secrets.h.
extern const char *TARGET_ADDRESS;

// Parses SCALE_BINDKEY_HEX/SCALE_MAC from secrets.h into the byte forms the
// BLE decoder needs. Call once, near the top of setup(), before startBleScan().
void decodeSecrets();

// Reconfigures the IDF task watchdog to actually reboot on timeout (see
// scale_common.cpp for why) and hooks the calling task (the loop task) into
// it. Call once from setup().
void setupWatchdog();

#if ENABLE_CRASH_LOG
// Appends a record for the previous run to the NVS ring buffer and dumps
// the whole history to serial. Call once, near the very top of setup(),
// before anything else that might crash.
void recordAndDumpBootHistory();

// Call from loop(): tracks the running minimum free heap and flushes
// {uptime, minFreeHeap} to NVS every 30s. Pass force=true to flush
// immediately, e.g. right before a deliberate restart.
void updateBootHeartbeat(bool force = false);
#endif

// Called with each decoded weight reading (kg) as soon as it's decrypted -
// from the BLE host task (NimBLE's onResult callback), NOT the loop task.
// Whatever callback is registered here must be safe to call from that
// context: park the value and do the real publish from loop(), don't block
// or touch anything that isn't safe to touch from two tasks at once.
using WeightCallback = void (*)(float weightKg);
void setWeightCallback(WeightCallback cb);

// Configures and starts continuous passive BLE scanning for the scale
// (runs forever in the background via the BLE host task). Call once from
// setup(), after setWeightCallback() so no reading is ever missed.
//
// intervalUnits/windowUnits are raw NimBLE scan interval/window values (in
// 0.625ms units, per the BLE spec) - windowUnits/intervalUnits is the scan
// duty cycle. The defaults (~99%) are fine for main_wifi.cpp: BLE and WiFi
// coexistence tolerates a near-continuous scan. main_zigbee.cpp passes a
// much lower duty cycle instead - the ESP32-C6 has a single 2.4GHz radio
// shared (time-multiplexed) between BLE and 802.15.4/Zigbee, and a
// near-100%-duty BLE scan starves Zigbee of airtime badly enough that
// zigbee2mqtt's post-join interview (which depends on several ZDO/ZCL
// request-response round trips) can fail outright - see that file's setup()
// for the details.
void startBleScan(uint16_t intervalUnits = 100, uint16_t windowUnits = 99);
