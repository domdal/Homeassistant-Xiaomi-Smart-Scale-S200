// zigbee2mqtt external converter for the ESP32 Xiaomi Scale S200 Zigbee
// bridge (see ../../src/main_zigbee.cpp). That firmware reports weight as
// a standard ZCL Analog Input (genAnalogInput.presentValue) on endpoint 1,
// with no vendor-specific cluster - without this file z2m falls back to
// its generic auto-generated expose for the cluster, which shows the raw
// unrounded float with a guessed (wrong) unit instead of a clean "weight"
// (kg) property.
//
// Install: see ../README.md in this repo's zigbee2mqtt/ directory - in
// short, copy this whole external_converters/ folder into zigbee2mqtt's
// data directory (next to configuration.yaml) and set
// advanced.enable_external_js: true there.

import * as exposesLib from 'zigbee-herdsman-converters/lib/exposes';
import * as reporting from 'zigbee-herdsman-converters/lib/reporting';

const e = exposesLib.presets;
const ea = exposesLib.access;

const fzLocal = {
  scale_weight: {
    cluster: 'genAnalogInput',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
      if (msg.data.hasOwnProperty('presentValue')) {
        // Round away float noise from the ZCL float encoding (e.g.
        // 95.69999694824219) - the firmware itself only ever sends 2
        // decimal places.
        return {weight: Math.round(msg.data.presentValue * 100) / 100};
      }
    },
  },
};

export default {
  // Matches the Basic cluster ModelIdentifier the firmware sets via
  // zbScale.setManufacturerAndModel(..., "Scale S200") in main_zigbee.cpp.
  // If that ever collides with a real commercial device's modelID, narrow
  // this to a `fingerprint: [{modelID: 'Scale S200', manufacturerName:
  // 'Xiaomi (bridged via ESP32)'}]` entry instead.
  zigbeeModel: ['Scale S200'],
  model: 'Scale S200',
  vendor: 'Xiaomi (bridged via ESP32)',
  description: 'Xiaomi Smart Scale S200, bridged from BLE via ESP32 (esp32-scale-bridge project)',
  fromZigbee: [fzLocal.scale_weight],
  toZigbee: [],
  exposes: [e.numeric('weight', ea.STATE).withUnit('kg').withDescription('Measured weight')],
  configure: async (device, coordinatorEndpoint, logger) => {
    const endpoint = device.getEndpoint(1); // SCALE_ENDPOINT_NUMBER in main_zigbee.cpp
    await reporting.bind(endpoint, coordinatorEndpoint, ['genAnalogInput']);
    // Mirrors main_zigbee.cpp's zbScale.setAnalogInputReporting(0, 600,
    // 0.1): report on a >=0.1kg change, at least every 10 minutes
    // regardless. Re-asserting it here is harmless if the device already
    // has it configured - configureReporting is idempotent.
    await endpoint.configureReporting('genAnalogInput', [{
      attribute: 'presentValue',
      minimumReportInterval: 0,
      maximumReportInterval: 600,
      reportableChange: 0.1,
    }]);
  },
};
