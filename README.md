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
- `src/main.cpp` - the ESP32 firmware (NimBLE-Arduino) that does the same decode on-device and publishes the result to MQTT with Home Assistant auto-discovery.

## How it works

1. The scale broadcasts a BLE advertisement containing a MiBeacon object
   (service data UUID `0xFE95`) whenever it takes a measurement.
2. The payload is AES-CCM encrypted with a per-device **bindkey**.
3. Both the Python script and the firmware passively scan for that specific
   advertisement, decrypt it, and parse out the weight (TLV object
   `0x4e16`).
4. The firmware additionally publishes the weight to MQTT (retained, so
   Home Assistant shows the last known weight immediately after a restart
   instead of "unknown") and announces itself to Home Assistant via
   [MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#discovery-messages) - no manual `configuration.yaml` entity needed.

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
src/main.cpp                              ESP32 firmware: BLE scan, decrypt, MQTT publish + HA discovery
include/secrets.h                          WiFi/MQTT/scale credentials for the firmware (gitignored)
include/secrets.example.h                  Firmware credential template
scale_live_weight.py                       Standalone Python decoder/listener for testing on a PC/Mac
secrets.py                                  Scale credentials for the Python script (gitignored)
secrets.example.py                          Python credential template
platformio.ini                              Board/platform/library config
homeassistant/template_sensors.yaml        Per-person weight split (paste into configuration.yaml)
homeassistant/lovelace_card.yaml            Dashboard card for the scale entities
```

## Firmware setup (ESP32-C6)

1. Copy `include/secrets.example.h` to `include/secrets.h`, then fill in
   your WiFi/MQTT/scale values (see the comments in that file).

2. Build and flash with [PlatformIO](https://platformio.org/):

   ```
   pio run -t upload
   pio device monitor
   ```

3. On boot the firmware connects to WiFi and your MQTT broker, publishes a
   retained Home Assistant discovery config for a `Weight` sensor
   (`homeassistant/sensor/<MQTT_CLIENT_ID>/weight/config`), and from then
   on publishes each decoded weight to `MQTT_TOPIC_WEIGHT`. It also sets a
   Last Will/Testament so Home Assistant shows the entity as unavailable if
   the ESP32 drops offline.

   Step on the scale - the entity should appear in Home Assistant under
   **Settings → Devices & Services → MQTT** automatically, no YAML needed.

### Board/platform notes

- Board: ESP32-C6-DevKitC-1 (see `platformio.ini`). Any ESP32 board with
  enough flash for `huge_app.csv` (~3MB app partition) should work with
  minor tweaks.
- Uses NimBLE-Arduino instead of the Arduino core's bundled `BLEDevice.h`
  (Bluedroid) - the Bluedroid stack ran out of task stack ("overflow")
  scanning on this hardware; NimBLE is far lighter and doesn't have that
  problem.
- Uses the `pioarduino` community fork of the `espressif32` platform for
  up-to-date ESP32-C6 support - see the comment at the top of
  `platformio.ini`.

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
no way to know *who* stepped on it from the packet itself.
`sensor.xiaomi_scale_s200_bridge_weight` (published by the firmware,
retained - entity ID depends on your `MQTT_CLIENT_ID`/device name in
`secrets.h`, check yours under **Developer Tools → States** if you changed
either) always reflects the single most recent reading regardless of who
it was.

[`homeassistant/template_sensors.yaml`](homeassistant/template_sensors.yaml)
splits that into `sensor.weight_user_1` / `sensor.weight_user_2` using a
[trigger-based template sensor](https://www.home-assistant.io/integrations/template/#trigger-based-template-sensors):
each new reading is assigned to whichever of two expected weight ranges
it's closer to - the same heuristic Xiaomi's own Mi Fit app uses. Overlap
between the two ranges is fine; ties go to whichever range's midpoint is
closer, so a reading is never assigned to both, and each sensor keeps its
previous value otherwise (no flicker to the other person's weight).

To install: paste its `template:` block into `configuration.yaml` (tuning
the four range numbers to your own weights first), then reload via
**Developer Tools → YAML → Template Entities** (or restart HA). See the
comments in that file for making the ranges editable from the UI via
`input_number` helpers instead of hardcoding them.

### Dashboard card

[`homeassistant/lovelace_card.yaml`](homeassistant/lovelace_card.yaml) is a
ready-made card for the scale (current readings + a 7-day history graph per
person), built from stock Lovelace card types - no HACS custom cards
needed. Requires the template sensors above to exist first. To install:
**Edit Dashboard → Add Card → Manual**, paste it in, **Save**.

## Secrets

`include/secrets.h` and `secrets.py` hold WiFi/MQTT/scale credentials and
are both gitignored - never commit real values.

## License

[MIT](LICENSE)
