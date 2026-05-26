'use strict';

// OpenPiste Level 1 bridge — EFP1.1 UDP ↔ MQTT
//
// Runs on the same machine as the Mosquitto broker (Raspberry Pi).
// For each configured piste, binds a UDP socket to the piste's static IP
// so the CMS can connect without reconfiguration. Relays in both directions:
//
//   CMS → UDP → bridge → MQTT  (openpiste/{id}/software/efp1)
//   apparatus → MQTT → bridge → UDP → CMS  (openpiste/{id}/apparatus/efp1)
//
// Prerequisites:
//   IP aliases must be added before starting (see config.subnet below):
//     sudo ip addr add 192.168.0.101/24 dev eth0
//     sudo ip addr add 192.168.0.102/24 dev eth0
//
// Usage:
//   npm install
//   node bridge.js

const dgram = require('dgram');
const mqtt  = require('mqtt');

// ── Configuration ────────────────────────────────────────────────────────────

const config = {
  subnet:     '192.168.0',      // first three octets of your LAN subnet
  ipBase:     100,              // piste N gets IP subnet.(ipBase + N)
  udpPort:    50100,            // port used for all Cyrano UDP traffic
  cmsPort:    50100,
  mqttBroker: 'mqtt://localhost',
  pistes: [
    { number: 1, id: '1' },
    { number: 2, id: '2' },
  ],
};

// ── Helpers ──────────────────────────────────────────────────────────────────

function pisteIp(number) {
  return `${config.subnet}.${config.ipBase + number}`;
}

// Extract piste ID from raw EFP1.1 payload for logging.
// Format: |Protocol|Command|PisteId|...  → split('|')[3]
function parsePisteId(buf) {
  const parts = buf.toString('ascii').split('|');
  return parts.length > 3 ? parts[3] : '?';
}

// ── MQTT client ──────────────────────────────────────────────────────────────

const mqttClient = mqtt.connect(config.mqttBroker);

mqttClient.on('connect', () => {
  console.log(`[MQTT] Connected to ${config.mqttBroker}`);
  config.pistes.forEach(p => {
    const topic = `openpiste/${p.id}/apparatus/efp1`;
    mqttClient.subscribe(topic, { qos: 0 }, () => {
      console.log(`[MQTT] Subscribed to ${topic}`);
    });
  });
});

mqttClient.on('error', err => console.error('[MQTT] Error:', err.message));

// ── Per-piste UDP sockets ─────────────────────────────────────────────────────

config.pistes.forEach(piste => {
  const ip    = pisteIp(piste.number);
  const sock  = dgram.createSocket('udp4');
  let   cmsIp = null;   // learned from first incoming UDP packet

  // UDP → MQTT: CMS sends HELLO/DISP/ACK/NAK to this IP:udpPort
  sock.on('message', (msg, rinfo) => {
    if (!cmsIp) {
      cmsIp = rinfo.address;
      console.log(`[${piste.id}] CMS address learned: ${cmsIp}`);
    }
    const topic = `openpiste/${piste.id}/software/efp1`;
    console.log(`[${piste.id}] UDP→MQTT  cmd=${parsePisteId(msg)}  topic=${topic}`);
    mqttClient.publish(topic, msg, { qos: 0, retain: false });
  });

  sock.on('error', err => console.error(`[${piste.id}] UDP error:`, err.message));

  sock.bind(config.udpPort, ip, () => {
    console.log(`[${piste.id}] Listening on UDP ${ip}:${config.udpPort}`);
  });

  // MQTT → UDP: apparatus publishes INFO/NEXT/PREV; forward to CMS
  mqttClient.on('message', (topic, payload) => {
    if (topic !== `openpiste/${piste.id}/apparatus/efp1`) return;
    if (!cmsIp) {
      console.log(`[${piste.id}] MQTT→UDP skipped — CMS address not yet known`);
      return;
    }
    console.log(`[${piste.id}] MQTT→UDP  → ${cmsIp}:${config.cmsPort}`);
    sock.send(payload, config.cmsPort, cmsIp);
  });
});

console.log('OpenPiste Level 1 bridge starting...');
