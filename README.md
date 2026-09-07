# esp32-scale-bridge

Bridges a Xiaomi Mi Smart Scale (BLE, MiBeacon-encrypted) to Home Assistant
over MQTT, using an ESP32-C6 as a passive BLE listener. Tested against a
**Xiaomi Mi Smart Scale S200 (model `MJTZC02YM` https://www.amazon.de/dp/B0F7SCD618)**, but should work
with any Xiaomi/Mi scale that advertises weight via an encrypted MiBeacon
(service data UUID `0xFE95`).

The scale is never paired or connected to - the ESP32 just passively
listens for its BLE advertisements and decrypts them with the scale's
bindkey.

There are two independent implementations of the same decode logic here:

- `scale_live_weight.py` - a desktop Python script (via [bleak](https://github.com/hbldh/bleak)) for prototyping/debugging the decode against your own scale before touching firmware.
- ESP32 firmware (NimBLE-Arduino) that does the same decode on-device, in two interchangeable backends sharing one BLE/crypto/crash-log core (`src/scale_common.*`):
  - `src/main_wifi.cpp` - publishes to MQTT with Home Assistant auto-discovery over WiFi (the default).
  - `src/main_zigbee.cpp` - joins your Zigbee mesh directly (via zigbee2mqtt) over the ESP32-C6's built-in 802.15.4 radio, no WiFi/MQTT involved. See that file's header comment for why you might prefer this - in short, a weak WiFi signal's reconnect churn interacting badly with BLE+WiFi radio coexistence.

## How it works

1. The scale broadcasts a BLE advertisement containing a MiBeacon object
   (service data UUID `0xFE95`) whenever it takes a measurement.
2. The payload is AES-CCM encrypted with a per-device **bindkey**.
3. Both the Python script and the firmware passively scan for that specific
   advertisement, decrypt it, and parse out the weight (TLV object
   `0x4e16`).
4. The firmware then hands each decoded weight to whichever backend is
   built:
   - **WiFi/MQTT** (`main_wifi.cpp`, default): publishes to MQTT (retained,
     so Home Assistant shows the last known weight immediately after a
     restart - and keeps showing it even while the ESP32 itself is offline,
     rather than going "unavailable") and announces itself to Home
     Assistant via [MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#discovery-messages) -
     no manual `configuration.yaml` entity needed.
   - **Zigbee** (`main_zigbee.cpp`): reports the weight as a standard ZCL
     Analog Input over your existing Zigbee mesh, picked up by
     zigbee2mqtt - see "Firmware setup" below.

## Getting your scale's MAC address and bindkey

Xiaomi scales don't hand out their bindkey over BLE - it has to be pulled
from your Xiaomi Cloud account (the same key the official Mi Fit/Mi Home
app uses to decrypt the scale's broadcasts). This project used
[piotrmachowski/xiaomi-cloud-tokens-extractor](https://github.com/piotrmachowski/xiaomi-cloud-tokens-extractor)
for that:

1. Follow that tool's instructions to log in with your Xiaomi account and
   dump your devices' tokens/beaconkeys.
2. Find your scale in the output (model `MJTZC02YM` or similar) and note:
   - its BLE **MAC address**
   - its **BLE beaconkey** - this is the bindkey used below.
3. Plug both into `secrets.h` / `secrets.py` as described below.

## Repo layout

```
src/scale_common.h/.cpp                    Shared BLE scan/decrypt logic + NVS boot/crash-history log
src/main_wifi.cpp                          WiFi/MQTT backend: publishes weight + HA discovery over MQTT
src/main_zigbee.cpp                        Zigbee backend: reports weight to zigbee2mqtt directly
include/secrets.h                          WiFi/MQTT/scale credentials for the firmware (gitignored)
include/secrets.example.h                  Firmware credential template
scale_live_weight.py                       Standalone Python decoder/listener for testing on a PC/Mac
secrets.py                                  Scale credentials for the Python script (gitignored)
secrets.example.py                          Python credential template
platformio.ini                              Board/platform/library config
homeassistant/template_sensors_wifi.yaml   Per-person weight split (WiFi/MQTT backend) - paste into configuration.yaml
homeassistant/template_sensors_zigbee.yaml Per-person weight split (Zigbee backend) - paste into configuration.yaml
homeassistant/lovelace_card.yaml            Dashboard card for the scale entities
zigbee2mqtt/                                External converter + setup for the Zigbee backend (see its README)
```

## Firmware setup (ESP32-C6)

Pick one backend - both decode the scale identically, they just differ in
how the weight reaches Home Assistant. `platformio.ini` defines one
PlatformIO environment per backend; plain `pio run`/`pio run -t upload`
build/flash the default (`esp32-c6-devkitc-1-wifi`), or pick explicitly with
`-e <env>`.

### WiFi + MQTT (default)

1. Copy `include/secrets.example.h` to `include/secrets.h`, then fill in
   your WiFi/MQTT/scale values (see the comments in that file).

2. Build and flash with [PlatformIO](https://platformio.org/):

   ```
   pio run -e esp32-c6-devkitc-1-wifi -t upload
   pio device monitor
   ```

3. On boot the firmware connects to WiFi and your MQTT broker, publishes
   retained Home Assistant discovery configs for a `Weight` sensor
   (`homeassistant/sensor/<MQTT_CLIENT_ID>/weight/config`) and a `Bridge
   Status` connectivity sensor (`homeassistant/binary_sensor/<MQTT_CLIENT_ID>/status/config`),
   and from then on publishes each decoded weight to `MQTT_TOPIC_WEIGHT`.
   `Weight` has no availability tied to it, so it keeps showing the last
   known reading even while the ESP32 is offline instead of going
   "unavailable" - `Bridge Status` is a separate diagnostic entity (driven
   by a Last Will/Testament) for anyone who wants to monitor/alert on the
   bridge's own connectivity without that affecting the weight reading.

   Step on the scale - the entity should appear in Home Assistant under
   **Settings → Devices & Services → MQTT** automatically, no YAML needed.

### Zigbee (via zigbee2mqtt)

Only the scale's MAC/bindkey in `secrets.h` are needed for this backend -
no WiFi/MQTT fields.

1. Copy `include/secrets.example.h` to `include/secrets.h` and fill in the
   scale's `SCALE_MAC`/`SCALE_BINDKEY_HEX` (the WiFi/MQTT fields are unused
   by this backend but must still be present in the file).

2. Build and flash:

   ```
   pio run -e esp32-c6-devkitc-1-zigbee -t upload
   pio device monitor
   ```

3. Enable "Permit join" in zigbee2mqtt, then power the board (or reset it) -
   the serial monitor prints dots while it joins, then "connected." First
   join can take a while if you have to go flip that switch after
   flashing. The join is stored on-device and persists across normal
   reboots/power cycles - only a full flash erase forces re-pairing.

4. zigbee2mqtt won't recognize this as a known device model on first join,
   so it'll list it under "unsupported devices" until you install the
   external converter in **[`zigbee2mqtt/`](zigbee2mqtt/)** - see that
   folder's README for the exact steps (where to put the file, how to
   enable it, and how to point `template_sensors_zigbee.yaml` at the
   resulting entity).

### Board/platform notes

- Board: ESP32-C6-DevKitC-1 (see `platformio.ini`). Any ESP32-C6/H2 board
  (needed for the Zigbee backend's 802.15.4 radio; the WiFi backend works
  on any ESP32 with enough flash) should work with minor tweaks.
- Uses NimBLE-Arduino instead of the Arduino core's bundled `BLEDevice.h`
  (Bluedroid) - the Bluedroid stack ran out of task stack ("overflow")
  scanning on this hardware; NimBLE is far lighter and doesn't have that
  problem.
- Uses the `pioarduino` community fork of the `espressif32` platform for
  up-to-date ESP32-C6 support - see the comment at the top of
  `platformio.ini`.
- The two backends use different partition tables (`huge_app.csv` for
  WiFi/MQTT's larger image vs. `zigbee_zczr.csv`, which the Zigbee stack
  needs for its own network/pairing storage) - this is why they're separate
  PlatformIO environments rather than a single build-time flag.

## Python script (for testing on a PC/Mac before flashing)

Useful for confirming your MAC/bindkey are correct, or debugging the
decode, without needing hardware.

1. Copy `secrets.example.py` to `secrets.py`, then fill in your scale's
   `ADDRESS`/`BINDKEY_HEX` (see the comments in that file).

2. Set up the virtualenv and install dependencies:

   ```
   python3 -m venv env
   ./env/bin/pip install bleak pycryptodome
   ```

3. Run it and step on the scale:

   ```
   ./env/bin/python scale_live_weight.py
   ```

   **macOS note:** `bleak`'s CoreBluetooth backend doesn't expose real BLE
   MAC addresses (it hands out an OS-generated UUID instead) - this script
   already accounts for that by matching on the MAC bytes embedded in the
   MiBeacon payload rather than the advertising device's reported address.
   You may also need to grant your terminal app Bluetooth permission under
   System Settings → Privacy & Security → Bluetooth.

## Home Assistant: telling two people apart

The scale's BLE broadcast carries no per-user ID (weight only), so there's
no way to know *who* stepped on it from the packet itself. Whichever
backend you're running publishes a single "last reading" sensor that
always reflects the most recent weight regardless of who it was:
`sensor.xiaomi_scale_s200_bridge_weight` for the WiFi/MQTT backend (entity
ID depends on your `MQTT_CLIENT_ID`/device name in `secrets.h`), or
whatever zigbee2mqtt exposes for the Zigbee backend (see
[`zigbee2mqtt/README.md`](zigbee2mqtt/README.md)) - check yours under
**Developer Tools → States** either way.

- [`homeassistant/template_sensors_wifi.yaml`](homeassistant/template_sensors_wifi.yaml) - for the WiFi/MQTT backend.
- [`homeassistant/template_sensors_zigbee.yaml`](homeassistant/template_sensors_zigbee.yaml) - for the Zigbee backend.

Either one splits that single sensor into `sensor.weight_user_1` /
`sensor.weight_user_2` using a
[trigger-based template sensor](https://www.home-assistant.io/integrations/template/#trigger-based-template-sensors):
each new reading is assigned to whichever of two expected weight ranges
it's closer to - the same heuristic Xiaomi's own Mi Fit app uses. Overlap
between the two ranges is fine; ties go to whichever range's midpoint is
closer, so a reading is never assigned to both, and each sensor keeps its
previous value otherwise (no flicker to the other person's weight).

To install: pick the file matching your backend, paste its `template:`
block into `configuration.yaml` (tuning the four range numbers to your own
weights first, and double-checking the trigger `entity_id` matches yours),
then reload via **Developer Tools → YAML → Template Entities** (or restart
HA). See the comments in that file for making the ranges editable from the
UI via `input_number` helpers instead of hardcoding them.

### Dashboard view

[`homeassistant/lovelace_card.yaml`](homeassistant/lovelace_card.yaml) is a
ready-made **Sections** dashboard view for the scale (current readings side
by side with a 7-day history graph per person, each with a smoothed curve
between readings). It needs the
[apexcharts-card](https://github.com/RomRider/apexcharts-card) custom card
(install via HACS → Frontend → search "apexcharts-card") - the stock
history-graph card can't interpolate a curve between the scale's sparse
readings and only shows a flat/rectangular hold. apexcharts-card keeps the
axis labels and gridlines of the stock chart while smoothing the curve.
Requires the template sensors above to exist first. To install: create or
open a dashboard view of type **Sections** (Settings → Dashboards → Add
Dashboard/View), then **Edit → Edit in YAML**, paste it in, **Save**.

## Secrets

`include/secrets.h` and `secrets.py` hold WiFi/MQTT/scale credentials and
are both gitignored - never commit real values.

## License

[MIT](LICENSE)
