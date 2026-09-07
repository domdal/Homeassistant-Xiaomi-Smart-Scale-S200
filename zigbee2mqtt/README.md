# zigbee2mqtt setup for the Zigbee firmware backend

Only needed if you're running `src/main_zigbee.cpp` (see that file and the
main README's "Zigbee (via zigbee2mqtt)" section). The WiFi/MQTT backend
(`src/main_wifi.cpp`) doesn't touch zigbee2mqtt at all - skip this folder.

## Why this is needed

`main_zigbee.cpp` reports weight over a standard Zigbee ZCL **Analog
Input** cluster (`genAnalogInput`, attribute `presentValue`) - there's no
Zigbee "mass/weight" cluster to use instead. Out of the box, zigbee2mqtt
has no device definition for this custom firmware, so after pairing it
falls back to a generic auto-generated expose for that cluster: a raw,
unrounded float (e.g. `95.69999694824219`) labeled "Analog Input Weight
(kg) on endpoint 1" with a guessed (wrong) unit. `external_converters/scale_s200_converter.mjs`
in this folder is a small external converter that tells z2m to expose
that cluster's value as a proper, rounded `weight` (kg) property instead.

## Install

**This is the zigbee2mqtt 2.x method** (external converters as `.mjs`
files in an `external_converters/` folder, no `configuration.yaml` list
entry). If you're on an older 1.x install, see the
[legacy docs](https://github.com/Koenkk/zigbee2mqtt.io/blob/master/docs/advanced/support-new-devices/01_support_new_devices.md)
instead - the general idea (map `genAnalogInput.presentValue` to a
`weight` property) is the same, just in CommonJS/`.js` form.

1. Copy this repo's whole `zigbee2mqtt/external_converters/` folder into
   zigbee2mqtt's **data directory** (the same folder that holds its
   `configuration.yaml` and `database.db`), so you end up with
   `<data dir>/external_converters/scale_s200_converter.mjs`. Where that
   data directory is depends on how you run z2m:
   - Docker: the host directory mounted to the container's `/app/data`.
   - Home Assistant Add-on: `/addon_configs/<slug>_zigbee2mqtt/` (visible
     via the Samba/SSH add-on, or the add-on's own file editor).
   - Manual/systemd install: wherever `ZIGBEE2MQTT_DATA` points, commonly
     `~/zigbee2mqtt/data`.

2. External converters can execute arbitrary code, so z2m 2.11+ disables
   them by default - enable it in `configuration.yaml`:

   ```yaml
   advanced:
     enable_external_js: true
   ```

3. Restart zigbee2mqtt. It re-matches already-paired devices against
   external converters on startup using data already stored in its
   database, so **you should not need to re-pair** the board - if the
   device still shows the generic fallback expose after a restart, check
   the z2m log for a converter load error (wrong path, or
   `enable_external_js` still false), then remove the device from z2m and
   re-pair (see below) as a last resort.

## Pairing the board

1. In the z2m frontend (or `configuration.yaml`), enable **Permit join**.
2. Power-cycle or reset the ESP32 running `main_zigbee.cpp`. Its onboard
   LED blinks blue while searching/joining, then flashes green a few times
   once connected; the serial log mirrors this with dots then "connected."
   - this can take a little while if you have to go flip permit-join on
   after the board's already running.
3. Once joined (and with the converter installed per above), it should
   appear in z2m's device list as model **Scale S200** / vendor **Xiaomi
   (bridged via ESP32)**, exposing a `weight` property in kg. The LED
   flashes white briefly on each weighing event thereafter.

The join is stored on the ESP32's flash (in the `zb_storage` partition)
and survives normal reboots/power cycles. If z2m ever stops recognizing
the device - most commonly after removing/re-adding it in z2m, which
leaves the board still believing it's joined to a network the coordinator
no longer remembers - **hold the BOOT button for 3 seconds** (LED blinks
red while held) to wipe that stored state and force a genuine rejoin, then
follow the pairing steps above again (with permit-join enabled). No
reflashing needed.

## Home Assistant

If z2m's Home Assistant integration is enabled (**Settings → Integrations
→ Home Assistant**, on by default in the HA Add-on), the `weight`
property is auto-discovered as an entity named after the device's
friendly_name in z2m - `sensor.xiaomi_smart_scale_s200_weight` with z2m's
default friendly_name for this device.

`../homeassistant/template_sensors_zigbee.yaml` already targets that
entity by default - double check it matches yours under **Developer Tools
→ States** (it won't if you gave the device a different friendly name in
z2m) and adjust its trigger `entity_id` if not.

## If "Scale S200" ever collides with a real device

`scale_s200_converter.mjs` matches by Zigbee model identifier
(`zigbeeModel: ['Scale S200']`, set by `main_zigbee.cpp`'s
`setManufacturerAndModel()`). That string is unlikely to collide with a
real commercial device, but if it ever does, narrow the match in the
converter to:

```js
fingerprint: [{modelID: 'Scale S200', manufacturerName: 'Xiaomi (bridged via ESP32)'}],
```

(replacing the `zigbeeModel: [...]` line) so it only matches devices
reporting both fields exactly as this firmware does.
