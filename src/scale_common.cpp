#include "scale_common.h"

#include <NimBLEDevice.h>
#include "mbedtls/ccm.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include <cstring>
#include "secrets.h"

#if ENABLE_CRASH_LOG
#include <Preferences.h>
#endif

// ---------------------------------------------------------------------
// Configuration - match these to your own scale (see the Python script
// this was ported from for how to obtain the bindkey). SCALE_MAC and
// SCALE_BINDKEY_HEX live in secrets.h; decodeSecrets() turns them into the
// byte forms used below.
// ---------------------------------------------------------------------
const char *TARGET_ADDRESS = SCALE_MAC; // scale's BLE MAC, "d0:7b:6f:5c:06:a6"-style

static uint8_t BINDKEY[16];

// Fallback MAC bytes as they appear *inside* the MiBeacon payload (reversed
// byte order vs. the human-readable SCALE_MAC address above). Only used if
// a given frame omits mac_include.
static uint8_t EXPECTED_MAC_BYTES[6];

static const uint16_t FE95_UUID16 = 0xFE95;

void decodeSecrets() {
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
// Watchdog. The IDF task watchdog is already enabled by default (see
// sdkconfig.esp32-c6-devkitc-1, CONFIG_ESP_TASK_WDT_EN) but with
// CONFIG_ESP_TASK_WDT_PANIC unset - meaning it only logs a warning on
// timeout instead of rebooting, so a wedged task (e.g. the WiFi/BLE stack
// stalling) is never recovered from and the board just sits hung. This
// reconfigures it at runtime to actually reboot, and hooks the loop task
// into it so a hang anywhere in loop() is caught.
// ---------------------------------------------------------------------
static const uint32_t WDT_TIMEOUT_MS = 30000; // loop() iterates every ~10ms, so this is generous

void setupWatchdog() {
  esp_task_wdt_config_t twdtConfig = {
      .timeout_ms = WDT_TIMEOUT_MS,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  if (esp_task_wdt_reconfigure(&twdtConfig) != ESP_OK) {
    esp_task_wdt_init(&twdtConfig);
  }
  esp_task_wdt_add(NULL); // watch the current (loop) task
}

#if ENABLE_CRASH_LOG
// Why a boot happened - brownout vs. our task watchdog vs. a crash/panic vs.
// a normal power-on. Used by the crash-history log below.
static const char *resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin (reset button)";
    case ESP_RST_SW:        return "esp_restart() (software)";
    case ESP_RST_PANIC:     return "PANIC - crash/exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog (scheduler/ISR lockup)";
    case ESP_RST_TASK_WDT:  return "TASK WATCHDOG (setupWatchdog() caught a hang)";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "BROWNOUT - supply voltage sagged";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
  }
}
static const char *resetReasonString() {
  return resetReasonToString(esp_reset_reason());
}
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Persistent crash history, stored in NVS flash (survives a crash, a
// watchdog reboot, and a power cycle - only an actual reflash/erase clears
// it). The point: this board runs on USB-power-only in the field with no
// data cable attached, so there's no way to catch a live serial log across
// a multi-hour crash cycle. Instead, every boot appends one record
// describing how the *previous* run ended (how long it lasted, the lowest
// free heap it saw, and the reset reason) to a small ring buffer, then
// dumps the whole buffer to serial immediately - so plugging into a PC
// after the fact and opening a serial monitor shows the full history
// up front, no timing or live capture required.
// ---------------------------------------------------------------------
static Preferences crashLogPrefs;
static const size_t MAX_BOOT_RECORDS = 20;
static uint32_t g_minFreeHeap = UINT32_MAX; // tracked in RAM, flushed to NVS periodically

struct BootRecord {
  uint32_t seq;
  uint32_t runDurationMs;    // how long the previous run lasted before it ended
  uint32_t minFreeHeap;      // lowest free heap seen during that run
  int8_t resetReasonCode;    // esp_reset_reason_t of the boot that ended that run
};

void recordAndDumpBootHistory() {
  crashLogPrefs.begin("crashlog", false);

  bool hasPrevRun = crashLogPrefs.getBool("hbValid", false);
  uint32_t prevUptime = crashLogPrefs.getUInt("hbUptime", 0);
  uint32_t prevMinHeap = crashLogPrefs.getUInt("hbMinHeap", UINT32_MAX);
  uint32_t seq = crashLogPrefs.getUInt("seq", 0);

  BootRecord records[MAX_BOOT_RECORDS];
  size_t bytesRead = crashLogPrefs.getBytes("records", records, sizeof(records));
  size_t count = bytesRead / sizeof(BootRecord);
  if (count > MAX_BOOT_RECORDS) count = 0; // corrupt/never-written - start fresh

  if (hasPrevRun) {
    if (count == MAX_BOOT_RECORDS) {
      memmove(records, records + 1, sizeof(BootRecord) * (MAX_BOOT_RECORDS - 1));
      count = MAX_BOOT_RECORDS - 1;
    }
    records[count].seq = seq;
    records[count].runDurationMs = prevUptime;
    records[count].minFreeHeap = prevMinHeap;
    records[count].resetReasonCode = (int8_t)esp_reset_reason();
    count++;
    seq++;
    crashLogPrefs.putBytes("records", records, sizeof(BootRecord) * count);
    crashLogPrefs.putUInt("seq", seq);
  }

  // Reset the heartbeat for this run - updateBootHeartbeat() (called from
  // loop()) will keep it current from here on.
  crashLogPrefs.putUInt("hbUptime", 0);
  crashLogPrefs.putUInt("hbMinHeap", UINT32_MAX);
  crashLogPrefs.putBool("hbValid", true);
  crashLogPrefs.end();

  Serial.printf("===== Boot/crash history (oldest first, this boot caused by \"%s\") =====\n",
                resetReasonString());
  if (count == 0) {
    Serial.println("(no prior runs recorded yet)");
  }
  for (size_t i = 0; i < count; i++) {
    Serial.printf("  #%lu: ran %lu.%01lus, min free heap %lu bytes, ended by: %s\n",
                  (unsigned long)records[i].seq, (unsigned long)(records[i].runDurationMs / 1000),
                  (unsigned long)((records[i].runDurationMs / 100) % 10),
                  (unsigned long)records[i].minFreeHeap,
                  resetReasonToString((esp_reset_reason_t)records[i].resetReasonCode));
  }
  Serial.println("=================================================================");
}

// Note the 30s flush interval is also the resolution of the recorded run
// duration: any run shorter than that is reported as "ran 0.0s" with a
// min-heap of 4294967295 (the never-updated sentinel), so such an entry
// means "died within 30s of boot", not necessarily "died instantly".
void updateBootHeartbeat(bool force) {
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < g_minFreeHeap) g_minFreeHeap = freeHeap;

  static unsigned long lastFlushMs = 0;
  unsigned long now = millis();
  if (!force && now - lastFlushMs < 30000) return;
  lastFlushMs = now;

  crashLogPrefs.begin("crashlog", false);
  crashLogPrefs.putUInt("hbUptime", now);
  crashLogPrefs.putUInt("hbMinHeap", g_minFreeHeap);
  crashLogPrefs.end();
}
#endif // ENABLE_CRASH_LOG
// ---------------------------------------------------------------------

// Fixed upper bound on how many raw bytes toHex() below will render - covers
// every call site here (max ~31-byte BLE advertisement, max 64-byte
// decrypted plaintext buffer). Kept off the heap (String heap-fragments over
// long uptimes) in favor of stack buffers sized from this constant.
static const size_t HEX_BUF_MAX_BYTES = 64;
static const size_t HEX_BUF_LEN = HEX_BUF_MAX_BYTES * 2 + 1;

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

// Writes buf as lowercase hex into out (null-terminated), truncating safely
// if it doesn't fit rather than overflowing. No heap allocation.
static void toHex(const uint8_t *buf, size_t len, char *out, size_t outSize) {
  static const char *hexd = "0123456789abcdef";
  if (outSize == 0) return;
  size_t n = (outSize - 1) / 2;
  if (n > len) n = len;
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hexd[(buf[i] >> 4) & 0xF];
    out[i * 2 + 1] = hexd[buf[i] & 0xF];
  }
  out[n * 2] = '\0';
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

static WeightCallback g_weightCallback = nullptr;

void setWeightCallback(WeightCallback cb) {
  g_weightCallback = cb;
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
    char hexbuf[HEX_BUF_LEN];
    toHex(data, len, hexbuf, sizeof(hexbuf));
    Serial.printf("[%10lu] idle beacon (no measurement) - %s\n", millis(), hexbuf);
    return;
  }

  if (macBytes == nullptr) {
    macBytes = EXPECTED_MAC_BYTES;
  }

  if (idx > len) return;
  const uint8_t *remainder = data + idx;
  size_t remainderLen = len - idx;

  if (!flags.is_encrypted) {
    char hexbuf[HEX_BUF_LEN];
    toHex(remainder, remainderLen, hexbuf, sizeof(hexbuf));
    Serial.printf("[%10lu] unencrypted object - %s\n", millis(), hexbuf);
    return;
  }

  uint8_t plaintext[64];
  size_t plainLen = 0;
  if (remainderLen < 7 || remainderLen - 7 > sizeof(plaintext)) {
    Serial.printf("[%10lu] object size out of range (%u bytes)\n", millis(), (unsigned)remainderLen);
    return;
  }
  if (!decryptObject(remainder, remainderLen, macBytes, productId, frameCnt, plaintext, plainLen)) {
    char hexbuf[HEX_BUF_LEN];
    toHex(data, len, hexbuf, sizeof(hexbuf));
    Serial.printf("[%10lu] decryption FAILED for %s\n", millis(), hexbuf);
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

    char hexbuf[HEX_BUF_LEN];
    toHex(objVal, objLen, hexbuf, sizeof(hexbuf));
    if (objId == 0x4e16 && objLen >= 3) {
      float weightKg = ((uint16_t)objVal[1] | ((uint16_t)objVal[2] << 8)) / 100.0f;
      // Machine-friendly line first, human-friendly detail second.
      Serial.printf("WEIGHT_KG=%.2f\n", weightKg);
      Serial.printf("[%10lu] WEIGHT = %.2f kg   (raw obj=%s)\n", millis(), weightKg, hexbuf);
      if (g_weightCallback) g_weightCallback(weightKg);
    } else {
      Serial.printf("[%10lu] obj_id=0x%04x value=%s\n", millis(), objId, hexbuf);
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
#if DEBUG_LOG_ALL_ADVERTISEMENTS
    std::string addr = advertisedDevice->getAddress().toString();
    Serial.printf("[%10lu] seen %s (type=%d) rssi=%d\n", millis(), addr.c_str(),
                  advertisedDevice->getAddress().getType(), advertisedDevice->getRSSI());
#endif

    // Compare raw address bytes via memcmp instead of a string compare or
    // NimBLEAddress::equals() (which also compares the address TYPE - public
    // vs random, and guessing the wrong type here means every packet gets
    // silently dropped with zero output). getVal() returns the 6 address
    // bytes in on-air order, which is exactly EXPECTED_MAC_BYTES as computed
    // from SCALE_MAC in decodeSecrets(). This runs for every nearby BLE
    // advertisement (not just the scale's own), so it's kept allocation-free.
    if (memcmp(advertisedDevice->getAddress().getVal(), EXPECTED_MAC_BYTES, sizeof(EXPECTED_MAC_BYTES)) != 0) {
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

    char hexbuf[HEX_BUF_LEN];
    toHex(data, len, hexbuf, sizeof(hexbuf));
    static char lastHexBuf[HEX_BUF_LEN] = {0};
    if (strcmp(hexbuf, lastHexBuf) == 0) return; // skip pure repeats of the exact same advertisement
    strcpy(lastHexBuf, hexbuf); // safe: both buffers are HEX_BUF_LEN and hexbuf is always null-terminated within it

    handlePacket(data, len);
  }
};

static NimBLEScan *pBLEScan;

void startBleScan(uint16_t intervalUnits, uint16_t windowUnits) {
  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(new ScaleCallbacks(), /*wantDuplicates=*/true);
  pBLEScan->setActiveScan(false); // passive scan, matches the original Python script
  pBLEScan->setInterval(intervalUnits);
  pBLEScan->setWindow(windowUnits);

  // duration=0 means scan forever (BLE_HS_FOREVER). start() is asynchronous
  // - it kicks off the scan and returns immediately, while onResult() keeps
  // firing from the BLE host task as advertisements arrive. Calling start()
  // repeatedly from loop() (as a previous version of this file did, to run
  // "bounded bursts") doesn't work: since start() returns instantly, loop()
  // would call it again immediately, and each call with restart=true (the
  // default) tears down and re-issues the scan - so the radio never got a
  // real window to receive anything, and not even other nearby devices'
  // "seen ..." lines ever printed.
  if (!pBLEScan->start(0, false)) {
    Serial.println("ERROR: failed to start BLE scan");
  }
}
