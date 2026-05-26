'use strict';

// OpenPiste Level 1 bridge — EFP1.1 UDP ↔ MQTT
//
// Reads config.json from the same directory (copy from config.example.json).
// For each configured piste, binds a UDP socket to the piste's static IP
// so the CMS can connect without reconfiguration.
//
// Before starting, add IP aliases for each piste (once per boot):
//   sudo ip addr add 192.168.0.101/24 dev eth0
// or use a startup script — see install-on-pi.sh.
//
// Usage:
//   cp config.example.json config.json   # edit to match your network
//   npm install
//   node bridge.js [--verbose]

const dgram       = require('dgram');
const mqtt        = require('mqtt');
const fs          = require('fs');
const path        = require('path');
const { execSync } = require('child_process');

// ── Logging ───────────────────────────────────────────────────────────────────

const verbose = process.argv.includes('--verbose') || process.argv.includes('-v');
const log = (...args) => { if (verbose) console.log(...args); };

// ── Config ───────────────────────────────────────────────────────────────────

const configPath = path.join(__dirname, 'config.json');
if (!fs.existsSync(configPath)) {
  console.error(`[config] ${configPath} not found.`);
  console.error('[config] Copy config.example.json to config.json and edit it.');
  process.exit(1);
}

let config;
try {
  config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
} catch (e) {
  console.error('[config] Failed to parse config.json:', e.message);
  process.exit(1);
}

const { mqttBroker, udpPort, subnet, ipBase, pistes } = config;

function pisteIp(number) {
  return `${subnet}.${ipBase + number}`;
}

// ── IP alias setup ────────────────────────────────────────────────────────────
// Requires root (run via systemd service). Silently skips if already assigned.

pistes.forEach(p => {
  const cidr = `${pisteIp(p.number)}/24`;
  try {
    execSync(`ip addr add ${cidr} dev ${config.networkInterface}`, { stdio: 'pipe' });
    log(`[network] Added IP alias ${cidr} on ${config.networkInterface}`);
  } catch (e) {
    // RTNETLINK: File exists — alias already set, ignore
  }
});

// ── MQTT client ───────────────────────────────────────────────────────────────

const mqttClient = mqtt.connect(mqttBroker);

mqttClient.on('connect', () => {
  log(`[MQTT] Connected to ${mqttBroker}`);
  pistes.forEach(p => {
    const topic = `openpiste/${p.id}/apparatus/efp1`;
    mqttClient.subscribe(topic, { qos: 0 }, () =>
      log(`[MQTT] Subscribed: ${topic}`));
  });
});

mqttClient.on('error',      err => console.error('[MQTT] Error:', err.message));
mqttClient.on('disconnect', ()  => console.warn('[MQTT] Disconnected'));
mqttClient.on('reconnect',  ()  => log('[MQTT] Reconnecting...'));

// ── Per-piste UDP sockets ─────────────────────────────────────────────────────

pistes.forEach(piste => {
  const ip   = pisteIp(piste.number);
  const sock = dgram.createSocket('udp4');
  let   cmsIp = null;

  // UDP → MQTT: CMS sends HELLO/DISP/ACK/NAK to this piste's IP
  sock.on('message', (msg, rinfo) => {
    if (!cmsIp) {
      cmsIp = rinfo.address;
      log(`[${piste.id}] CMS address: ${cmsIp}`);
    }
    const topic = `openpiste/${piste.id}/software/efp1`;
    mqttClient.publish(topic, msg, { qos: 0, retain: false });
    log(`[${piste.id}] UDP→MQTT  from=${rinfo.address}  bytes=${msg.length}`);
  });

  sock.on('error', err => {
    console.error(`[${piste.id}] UDP error: ${err.message}`);
    console.error(`[${piste.id}] Is the IP alias set? sudo ip addr add ${ip}/24 dev ${config.networkInterface}`);
  });

  sock.bind(udpPort, ip, () =>
    log(`[${piste.id}] Listening on UDP ${ip}:${udpPort}`));

  // MQTT → UDP: apparatus publishes INFO/NEXT/PREV; forward to CMS
  mqttClient.on('message', (topic, payload) => {
    if (topic !== `openpiste/${piste.id}/apparatus/efp1`) return;
    if (!cmsIp) {
      log(`[${piste.id}] MQTT→UDP skipped — CMS address not yet known`);
      return;
    }
    sock.send(payload, udpPort, cmsIp, err => {
      if (err) console.error(`[${piste.id}] UDP send error: ${err.message}`);
      else log(`[${piste.id}] MQTT→UDP  to=${cmsIp}  bytes=${payload.length}`);
    });
  });

  log(`[${piste.id}] Configured — IP ${ip}:${udpPort}`);
});

log(`[bridge] Started — ${pistes.length} piste(s)`);
