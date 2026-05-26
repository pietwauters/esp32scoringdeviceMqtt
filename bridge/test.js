'use strict';

// Standalone test for the bridge — no IP aliases needed.
// Temporarily overrides config to bind to 127.0.0.1 so it runs on any machine.
//
// Usage (three separate terminals):
//
//   Terminal 1:  node test.js bridge    — starts bridge on localhost
//   Terminal 2:  node test.js cms       — simulates CMS sending a UDP HELLO
//   Terminal 3:  node test.js apparatus — simulates apparatus publishing INFO to MQTT
//
// What to expect:
//   After 'cms':       bridge logs "UDP→MQTT", mosquitto_sub sees software/efp1
//   After 'apparatus': bridge logs "MQTT→UDP", cms terminal receives the UDP packet

const dgram = require('dgram');
const mqtt  = require('mqtt');

const LOCALHOST   = '127.0.0.1';
const UDP_PORT    = 50101;
const CMS_PORT    = 50100;
const MQTT_BROKER = 'mqtt://localhost';
const PISTE_ID    = '1';

const FAKE_HELLO = Buffer.from('|EFP1.1|HELLO|1|TestComp|||||||||||||||||');
const FAKE_INFO  = Buffer.from('|EFP1.1|INFO|1|TestComp|1|1|1|1|3:00|000|I|E|N|H|REF|||%||||||||||||||%||||||||||||||%|');

const mode = process.argv[2];

if (mode === 'bridge') {
  // ── Mini bridge bound to localhost ─────────────────────────────────────────
  const mqttClient = mqtt.connect(MQTT_BROKER);
  let cmsIp = null;

  mqttClient.on('connect', () => {
    console.log('[bridge] MQTT connected');
    mqttClient.subscribe(`openpiste/${PISTE_ID}/apparatus/efp1`, { qos: 0 });

    mqttClient.on('message', (topic, payload) => {
      console.log(`[bridge] MQTT→UDP topic=${topic} len=${payload.length}`);
      if (!cmsIp) { console.log('[bridge] MQTT→UDP skipped — CMS not known yet'); return; }
      sock.send(payload, CMS_PORT, cmsIp, () =>
        console.log(`[bridge] UDP sent to ${cmsIp}:${CMS_PORT}`));
    });
  });

  const sock = dgram.createSocket('udp4');
  sock.on('message', (msg, rinfo) => {
    cmsIp = rinfo.address;
    console.log(`[bridge] UDP→MQTT from ${rinfo.address} len=${msg.length}`);
    mqttClient.publish(`openpiste/${PISTE_ID}/software/efp1`, msg, { qos: 0, retain: false });
  });
  sock.bind(UDP_PORT, LOCALHOST, () =>
    console.log(`[bridge] Listening on UDP ${LOCALHOST}:${UDP_PORT}`));

} else if (mode === 'cms') {
  // ── Simulate CMS: send a UDP HELLO, then listen for INFO back ─────────────
  const sock = dgram.createSocket('udp4');
  sock.bind(CMS_PORT, LOCALHOST, () => {
    console.log(`[cms] Listening for response on ${LOCALHOST}:${CMS_PORT}`);
    console.log(`[cms] Sending HELLO to ${LOCALHOST}:${UDP_PORT}`);
    sock.send(FAKE_HELLO, UDP_PORT, LOCALHOST);
  });
  sock.on('message', (msg, rinfo) => {
    console.log(`[cms] Received UDP from ${rinfo.address}:${rinfo.port} — ${msg.toString().substring(0, 40)}...`);
  });

} else if (mode === 'apparatus') {
  // ── Simulate apparatus: publish INFO to MQTT ───────────────────────────────
  const client = mqtt.connect(MQTT_BROKER);
  client.on('connect', () => {
    console.log('[apparatus] Publishing INFO to MQTT');
    client.publish(`openpiste/${PISTE_ID}/apparatus/efp1`, FAKE_INFO,
      { qos: 0, retain: false }, () => {
        console.log('[apparatus] Published — check bridge and cms terminals');
        client.end();
      });
  });

} else {
  console.log('Usage: node test.js [bridge|cms|apparatus]');
  process.exit(1);
}
