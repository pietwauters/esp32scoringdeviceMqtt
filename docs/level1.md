# OpenPiste Protocol — Level 1
## EFP1.1 transport over MQTT

**Status:** Draft
**Date:** May 2026
**Author:** Piet Wauters
**Repository:** https://github.com/OpenPiste
**Website:** https://openpiste.org

---

## Table of Contents

1. [Purpose](#1-purpose)
2. [What Level 1 is not](#2-what-level-1-is-not)
3. [How it works](#3-how-it-works)
4. [Topic structure](#4-topic-structure)
5. [Message transport](#5-message-transport)
6. [Deployment scenarios](#6-deployment-scenarios)
7. [The UDP–MQTT bridge](#7-the-udp-mqtt-bridge)
8. [Fixed IP constraint](#8-fixed-ip-constraint)
9. [Implementation in native MQTT apparatus](#9-implementation-in-native-mqtt-apparatus)
10. [Limitations](#10-limitations)
11. [Migration path to Level 2](#11-migration-path-to-level-2)

---

## 1. Purpose

Level 1 defines how existing EFP1.1 (Cyrano) payloads can be transported over MQTT without any modification to the payload itself. The raw EFP1.1 wire format is used as the MQTT message payload, unchanged.

The goal is to allow MQTT-native subscribers — displays, piste monitors, video tools, logging systems — to consume live scoring data from existing infrastructure without requiring any changes to the competition management software (CMS) or legacy apparatus. It is also the mechanism by which a native MQTT apparatus like the OpenPiste ESP32 device can expose its Cyrano traffic to the MQTT bus, so that all consumers on the broker see the same data regardless of which protocol the CMS uses.

Level 1 is a transition mechanism. It is not the end state. Its value is that it requires almost no implementation effort on either end and immediately enables the MQTT ecosystem.

---

## 2. What Level 1 is not

Level 1 is not a conversion or translation layer. The EFP1.1 payload is not parsed, re-encoded, or restructured. A Level 1 publisher takes whatever it would have sent over UDP and publishes it to an MQTT topic instead (or in addition). A Level 1 subscriber takes whatever arrives on an MQTT topic and processes it exactly as it would a UDP packet.

Level 1 does not carry timestamps, sequence numbers, or any of the structured fields defined in Level 2. Any subscriber that needs those features must wait for Level 2.

---

## 3. How it works

Under Level 1, an EFP1.1 message is published to an MQTT topic as its raw payload — the same byte sequence that would appear in a UDP packet. No framing, no wrapping, no JSON encoding.

A UDP–MQTT bridge relays traffic in both directions between the existing UDP network and the MQTT broker. The bridge is transparent to both ends: the CMS continues to send and receive UDP packets at the address it was configured with; MQTT-native subscribers receive the same data on the broker.

A native MQTT apparatus (such as the OpenPiste ESP32 device) does not need a bridge. It publishes its EFP1.1 strings directly to MQTT as a side effect of its normal UDP send path.

---

## 4. Topic structure

Level 1 uses the same topic hierarchy as Level 2:

```
openpiste/{piste_id}/{publisher}/efp1
```

| Segment | Values |
|---------|--------|
| `piste_id` | Piste identifier — number, name, or colour, e.g. `1`, `podium`, `rouge` |
| `publisher` | `apparatus` or `software` |
| `efp1` | Fixed — identifies this as a Level 1 message |

**Apparatus → broker** (INFO, NEXT, PREV messages):
```
openpiste/{piste_id}/apparatus/efp1
```

**Software → broker** (HELLO, DISP, ACK, NAK messages):
```
openpiste/{piste_id}/software/efp1
```

The piste identifier is encoded in the EFP1.1 payload itself (the `PisteId` field). The topic and the payload are consistent — the topic segment is used for routing and filtering, the payload field is what the CMS and apparatus validate.

The `apparatus`/`software` split is a routing convenience, not a strict requirement. The EFP1.1 `Command` field is self-identifying: INFO, NEXT, and PREV can only originate from apparatus; HELLO, DISP, ACK, and NAK can only originate from software. An implementation that prefers a single subscription may use `openpiste/{piste_id}/efp1` for all traffic and derive direction from the payload. The split is retained here because it enables broker-level filtering without payload parsing, supports per-role access control consistent with the Level 2 ACL model, and allows the bridge to route by UDP source IP without touching the payload.

### QoS and retained

| Direction | QoS | Retained |
|-----------|-----|----------|
| apparatus/efp1 | 0 | No |
| software/efp1 | 0 | No |

Level 1 mirrors UDP semantics: fire-and-forget, no persistence. QoS 0, not retained. A subscriber connecting mid-bout will not receive the last INFO — it must wait for the next one. This is identical behaviour to joining a UDP session already in progress.

---

## 5. Message transport

The raw EFP1.1 payload is published as a binary or UTF-8 string payload, exactly as it would appear in a UDP packet. No length prefix, no framing. The EFP1.1 format is self-delimiting — the `%|` group separator and trailing `|` allow parsers to determine message boundaries without an external length field.

---

## 6. Deployment scenarios

There are four distinct deployment scenarios, depending on what each end of the communication supports natively.

---

### Scenario A: Native MQTT apparatus, legacy UDP CMS

The most common scenario during migration. The CMS speaks only UDP. The apparatus speaks MQTT natively (and optionally also UDP for backward compatibility).

```
[Legacy CMS] ←UDP→ [Apparatus at fixed IP]
                          ↕ MQTT
                      [Broker]
                          ↕ MQTT
              [MQTT subscribers: displays, monitors, loggers]
```

The apparatus is the MQTT publisher. It publishes EFP1.1 strings to `apparatus/efp1` whenever it sends INFO, NEXT, or PREV over UDP. It subscribes to `software/efp1` to receive HELLO, DISP, ACK, and NAK from any MQTT-native CMS that might be present — but in this scenario the CMS is legacy, so inbound traffic arrives via UDP only.

**No bridge required.** The apparatus is the endpoint for both transports simultaneously.

---

### Scenario B: Native MQTT apparatus, native MQTT CMS

Both ends speak MQTT natively. No UDP involved. This is the target state for a fully migrated deployment.

```
[Native MQTT CMS] ←MQTT→ [Broker] ←MQTT→ [Native MQTT apparatus]
```

Under Level 1, both ends use the `efp1` topics. Level 2 supersedes this once both ends support it, but Level 1 works immediately with no bridge.

---

### Scenario C: Legacy UDP apparatus, legacy UDP CMS, with MQTT monitoring

Neither end speaks MQTT. A bridge is added to make the scoring data visible on the broker without modifying either end.

```
[Legacy CMS] ←UDP→ [Bridge] ←MQTT→ [Broker] ←MQTT→ [MQTT subscribers]
                      ↕ UDP
               [Legacy apparatus]
```

The bridge operates on the apparatus subnet, capturing UDP traffic from both ends and publishing it to the broker. The CMS and apparatus are unaware of the bridge. See Section 7 for bridge design.

---

### Scenario D: Legacy UDP apparatus, native MQTT CMS

The CMS has been upgraded to speak MQTT. The legacy apparatus has not. A bridge on the apparatus side translates between the apparatus's UDP and the broker.

```
[Native MQTT CMS] ←MQTT→ [Broker] ←MQTT→ [Bridge] ←UDP→ [Legacy apparatus]
```

The bridge presents itself to the apparatus as if it were the CMS (sending HELLO, DISP, ACK via UDP) and relays to/from the broker. See Section 7 for bridge design.

---

## 7. The UDP–MQTT bridge

The bridge is a small, stateless relay. It does not parse EFP1.1 payloads — it only reads the source IP and port of incoming UDP packets and the MQTT topic of incoming MQTT messages to determine routing. The payload is forwarded unchanged.

### Bridge operation

**UDP → MQTT direction:**
1. Listen on UDP port 50101 (apparatus listen port).
2. On receipt of a UDP packet from the CMS: record the source IP (CMS address); publish the payload to `openpiste/{piste_id}/software/efp1`.
3. On receipt of a UDP packet from the apparatus: publish the payload to `openpiste/{piste_id}/apparatus/efp1`.

The bridge learns peer IP addresses from the first incoming packet in each direction — the same mechanism used by the existing CyranoHandler implementation.

**MQTT → UDP direction:**
1. Subscribe to `openpiste/+/apparatus/efp1` and `openpiste/+/software/efp1`.
2. On receipt of an `apparatus/efp1` message: send the payload as a UDP packet to the CMS address on port 50100 (CMS broadcast port).
3. On receipt of a `software/efp1` message: send the payload as a UDP packet to the apparatus address on port 50101.

### Where the bridge runs

The bridge can run on any machine with network access to both the UDP subnet and the MQTT broker:

- **On the broker machine** — the simplest option. The broker machine is always running and connected to MQTT. This works when the broker, CMS, and apparatus are all on the same LAN.
- **On the CMS machine** — useful when the CMS machine is on the same subnet as the apparatus but the broker is remote.
- **On a dedicated small device** (Raspberry Pi or ESP8266/ESP32) — a standalone plug-in device assigned a static IP on the apparatus subnet.

The bridge is a lightweight process with no persistent state beyond the learned peer IP addresses. It can be implemented in approximately 50 lines of Python or Node.js.

---

## 8. Fixed IP constraint

EFP1.1 competition management software typically connects to a scoring apparatus at a fixed, configured IP address and port. The CMS does not broadcast to discover apparatus — it sends directly to the configured address. This is a unicast, point-to-point connection.

This constraint has an important consequence for bridge deployment: **the bridge must present itself at the IP address and port that the CMS is configured to use**. There can only be one listener at a given IP:port on a subnet.

This leads to different resolutions depending on which end is being bridged:

### Native MQTT apparatus (Scenario A)

No constraint at all. The apparatus is the endpoint the CMS is configured to contact. It listens on UDP 50101 at its own IP — the same IP it has always had. Adding MQTT publishing is additive; the UDP listener is unchanged. The CMS configuration does not need to change.

### Legacy apparatus being bridged (Scenarios C and D)

The bridge must occupy the apparatus's IP address. The options are:

**Option 1: Assign the legacy apparatus's IP to the bridge device.**
The legacy apparatus is given a new IP (or retired). The bridge device — a small MCU or Raspberry Pi — is assigned the original apparatus IP. The CMS configuration does not change. The bridge receives CMS traffic at the expected address and relays it to the apparatus at its new IP.

**Option 2: Run the bridge on the broker machine and update the CMS configuration.**
The broker machine is assigned the apparatus's old IP (or the CMS is reconfigured to point to the broker machine). This requires a one-time change to the CMS configuration but no change to the apparatus.

**Option 3: Use a network-level port forward.**
On a managed switch or router, forward traffic destined for the apparatus IP:50101 to the bridge IP:50101. The CMS configuration does not change; the network redirects transparently. This is the most transparent option but requires router access.

In all cases, the constraint is resolved by ensuring exactly one listener sits at the address the CMS expects. The bridge design itself is identical regardless of which option is used.

---

## 9. Implementation in native MQTT apparatus

For an apparatus that already builds EFP1.1 wire strings for UDP transmission (as the OpenPiste ESP32 device does), implementing Level 1 requires:

**Outbound (apparatus → broker):**
Publish the pre-built Cyrano wire string to `openpiste/{piste_id}/apparatus/efp1` alongside the existing UDP send. The string is already constructed — no additional work. QoS 0, not retained.

**Inbound (broker → apparatus):**
Subscribe to `openpiste/{piste_id}/software/efp1` on MQTT connect. When a message arrives, pass the raw payload directly into the existing EFP1.1 message handler, with piste ID verification bypassed (the topic already encodes the piste). This is functionally equivalent to receiving the same packet over UDP.

No new parsing, no new state, no new struct fields. The cost is approximately five additional lines in the MQTT connect handler and three additional publish calls.

---

## 10. Limitations

**No retained state.** A subscriber connecting mid-bout receives nothing until the next INFO is sent. Level 2 solves this with retained messages.

**No timestamps.** EFP1.1 does not carry millisecond timestamps. Light events, blade contacts, and clock updates carried via Level 1 cannot be synchronised with external systems at sub-second precision. Level 2 adds timestamps to all time-critical messages.

**Payload opacity.** A Level 1 subscriber must parse EFP1.1 to extract any field. There is no topic-level filtering of data types. A subscriber interested only in scores must still receive and parse the full INFO message. Level 2 separates each data type into its own topic.

**Single CMS constraint.** EFP1.1 is designed for a 1-to-1 relationship between CMS and apparatus. Level 1 does not change this — it only makes the traffic visible to additional read-only subscribers. Multiple writable clients on `software/efp1` would cause the apparatus to receive conflicting DISP messages.

---

## 11. Migration path to Level 2

Level 1 and Level 2 can coexist on the same broker simultaneously. A native MQTT apparatus can publish on both `apparatus/efp1` (Level 1) and the individual `apparatus/lights`, `apparatus/score`, etc. topics (Level 2) from the same state update. Subscribers choose which they consume based on their capabilities.

The migration path for a venue:

1. Deploy MQTT broker.
2. Flash native MQTT apparatus (or deploy bridge for legacy apparatus). Level 1 traffic appears on the broker immediately.
3. MQTT-native subscribers (new displays, monitoring tools) connect and consume Level 1 data.
4. Upgrade CMS to Level 2 when available. Level 1 publishing can be retained for backward compatibility or disabled once no Level 1 consumers remain.
5. Upgrade displays and tools to Level 2 to gain timestamps, retained state, and per-topic filtering.

At no point is a flag day required. Each component migrates independently.
