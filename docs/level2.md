# OpenPiste Protocol
## A proposal for modern open communication in fencing electronics

**Status:** Draft — working towards v1.0
**Author:** Piet Wauters
**Repository:** https://github.com/OpenPiste
**Website:** https://openpiste.org

---

Fencing electronics have long relied on two communication protocols: EFP1.1 (known as Cyrano), the dominant standard for communication between scoring apparatus and competition management software, and RS422-FPA, a serial protocol for driving external displays and scoreboards. Both were designed for their era and both served the community well. EFP1.1 has been in use since 2008. RS422-FPA traces its roots to 1995.

The world around them has changed. MQTT and JSON are now the lingua franca of connected devices. Libraries exist for every platform from microcontrollers to cloud services. The IoT ecosystem has solved, at scale, the same problems fencing electronics face: reliable message delivery, multiple subscribers, late-joining clients, structured extensible data. There is no longer a compelling reason to maintain a bespoke binary or CSV protocol when open, well-supported alternatives exist.

At the same time, there is a substantial installed base of EFP1.1-compatible apparatus and software. A new protocol that ignores this reality will not be adopted. Migration must be possible without requiring clubs and federations to replace working equipment overnight.

This document introduces the **OpenPiste Protocol**, a proposal for a modern, open communication standard for fencing electronics. It is structured in two levels:

**Level 1** addresses the transition. It defines how existing EFP1.1 payloads can be transported over MQTT without any change to the payload itself. A bridge — a simple piece of software — relays messages between the existing UDP network and an MQTT broker. Existing apparatus and software require no modification. New MQTT-native subscribers (displays, piste monitors, video tools) can immediately consume live scoring data from existing infrastructure. Level 1 is not a long-term target. It is a practical bridge that allows the ecosystem to move at its own pace.

**Level 2** is the destination. It defines a native JSON protocol designed from the ground up for MQTT, drawing on the field semantics of EFP1.1 and the message architecture of RS422-FPA while leaving behind the encoding constraints of both. It uses typed values, purpose-specific messages, retained state, and millisecond-precision timestamps. It is implementable on an ESP32 with standard open source libraries. It is designed to be genuinely open — any manufacturer, developer, club, or federation can implement it without restriction.

This is a working proposal, not a ratified standard. It is published in the hope that it will be useful, reviewed, and improved by the fencing electronics community. Comments, corrections, and contributions are welcome at https://github.com/OpenPiste.

---

# OpenPiste Protocol — Level 2
## Native JSON over MQTT

**Status:** Draft — working towards v1.0
**Date:** May 2026
**Author:** Piet Wauters
**Protocol identifier:** `OPP2`
**Repository:** https://github.com/OpenPiste
**Website:** https://openpiste.org

## Table of Contents

1. [Introduction](#1-introduction)
2. [Design principles](#2-design-principles)
3. [Relationship to prior protocols](#3-relationship-to-prior-protocols)
4. [Network and protocol stack](#4-network-and-protocol-stack)
5. [Topic structure](#5-topic-structure)
6. [Message overview](#6-message-overview)
7. [Common fields](#7-common-fields)
8. [Message: lights](#8-message-lights)
9. [Message: clock](#9-message-clock)
10. [Message: blade\_contact](#10-message-blade_contact)
11. [Message: score](#11-message-score)
12. [Message: connection](#12-message-connection)
13. [Message: state](#13-message-state)
14. [Message: fencers](#14-message-fencers)
15. [Message: match](#15-message-match)
16. [Message: uw2f](#16-message-uw2f)
17. [Message: medical](#17-message-medical)
18. [Message: video\_review](#18-message-video_review)
19. [Message: control](#19-message-control)
20. [Field types and conventions](#20-field-types-and-conventions)
21. [Sequence counter and idempotency](#21-sequence-counter-and-idempotency)
22. [Timestamp conventions](#22-timestamp-conventions)
23. [Versioning and compatibility](#23-versioning-and-compatibility)
24. [Security](#24-security)
25. [Open items](#25-open-items)

---

## 1. Introduction

Level 2 is the native JSON protocol of the OpenPiste platform. It is designed from the ground up for MQTT, taking full advantage of the broker's publish/subscribe model, topic hierarchy, and retained message capability. It does not carry forward the encoding constraints of EFP1.1.

Level 2 is intended to be a genuinely open standard — any apparatus manufacturer, software developer, club, or federation can implement it without restriction. The protocol identifier `OPP2` and a separate `version` field appear in every message, allowing receivers to identify the protocol family and enforce compliance rules appropriate for the declared version.

A JSON Schema for machine validation of all message types is maintained as a separate document in the OpenPiste repository. See `schemas/opp2/` at https://github.com/OpenPiste/protocol. *(Schema publication is a pending task — see Section 25.)*

---

## 2. Design principles

**Typed values.** Integers are integers. Booleans are booleans. No string-encoding of numeric or boolean fields.

**Purpose-specific messages.** Each message type carries only the data relevant to its purpose. A scoreboard that only needs lights and scores does not need to parse fencer names or competition metadata. A video tool that only needs blade contact timestamps does not need to process clock ticks.

**The broker is the single source of truth.** All state-bearing topics use retained messages. Any subscriber connecting at any point during a bout immediately receives the current state of every topic without waiting for the next publish cycle. No periodic heartbeat resends are needed.

**Timestamps on time-critical events.** The lights, clock, and blade contact messages carry a millisecond timestamp. This enables accurate synchronisation with video replay systems — a capability absent from both EFP1.1 and RS422-FPA. All timestamps are UTC. No local time, no timezone offsets, no daylight saving adjustments. See Section 22 for the encoding convention.

**Idempotent event processing.** Every QoS 1 message carries a mandatory sequence counter (`seq`) allowing consumers to detect and discard duplicate deliveries. See Section 21.

**Publisher identity belongs in the topic, not the payload.** The publisher role — apparatus, software, or remote — is encoded in the MQTT topic, not in the message payload. This allows subscribers to filter by publisher at the broker level, without parsing any payload. It also enables clean broker-side access control: each publisher role can be restricted to its own topic namespace. See Section 5 for the topic structure and Section 24 for the security model this enables.

**Topic is authoritative for piste identity and publisher role.** The piste identifier and publisher role are carried in the MQTT topic and are not duplicated in the payload. The topic is the single authoritative source of both.

**Extensible control.** The control topic carries named command events. New commands can be added without changing the protocol version or breaking existing receivers.

**Implementable on constrained hardware.** The reference implementation runs on an ESP32 using the Arduino MQTT and ArduinoJson libraries, both freely available.

---

## 3. Relationship to prior protocols

Level 2 draws on two existing protocols for its design:

**EFP1.1 (Cyrano)** provides the field semantics: state values, weapon codes, priority values, card counts, fencer status codes. These are preserved in Level 2 where they make sense, so developers familiar with EFP1.1 will recognise the values.

**RS422-FPA** (version 3.04a, 2019) provides the architectural inspiration for typed messages. RS422-FPA demonstrated that splitting scoring data into purpose-specific messages with different transmission priorities is practical and well-understood in the fencing community. In Level 2, MQTT topics replace the RS422 serial bus, the broker's retained message mechanism replaces RS422-FPA's periodic resend strategy, and QoS levels replace RS422-FPA's explicit message priority ordering.

| RS422-FPA message | Level 2 topic | Notes |
|-------------------|--------------|-------|
| Msg 1 — lights | `lights` | Boolean fields; timestamp added |
| Msg 2 — clock | `clock` | Typed fields; timestamp added |
| Msg 3 — scores/cards | `score` | Integer fields; black card added |
| Msg 4 — status | `state` + `connection` | Split into apparatus state and connection status |
| Msg 5+6 — competitor names | `fencers` | Restructured with left/right/common sections |
| Msg 7 — competition info | `match` | Match and competition metadata; round added |
| Msg 8 — UW2F | `uw2f` | Timer and P-cards |
| Msg 9 — bout control | `control` | Extensible command set |
| — | `blade_contact` | No RS422-FPA equivalent; blade contact with timestamp |
| — | `medical` | No RS422-FPA equivalent; medical timeout with countdown timer |
| — | `video_review` | No RS422-FPA equivalent; full call history and remaining counts |

---

## 4. Network and Protocol Stack

### 4.1 Broker

Any MQTT 3.1.1 compliant broker. Mosquitto is recommended for club and competition use — it is open source, lightweight, and runs on a laptop or Raspberry Pi.

### 4.2 Broker discovery

For club and small competition use, the broker host SHOULD be made discoverable via mDNS under the hostname:

```
openpiste.local
```

Any device on the local network can then reach the broker at `openpiste.local:1883` without IP address configuration. All OpenPiste-compatible devices SHOULD use this hostname as their default broker address, with fallback to a configurable IP address or hostname.

For larger competition setups with managed switches or multiple VLANs, mDNS may not propagate reliably across network boundaries. In these cases a static IP address or DHCP reservation for the broker is recommended, and the `openpiste.local` hostname may be configured in local DNS.

### 4.3 NTP

The broker host SHOULD also run a local NTP server. This allows all devices on the network to synchronise their clocks to UTC without requiring internet access. On Linux, `chrony` is recommended — it is lightweight and can serve NTP to local clients while itself operating without an upstream internet time source.

When all devices synchronise to the same local NTP server, timestamps in Level 2 messages are comparable across apparatus, displays, and video tools — enabling accurate video synchronisation on a fully self-contained competition network.

See Section 22 for the timestamp encoding convention, including the fallback behaviour when NTP is unavailable.

### 4.4 QoS

| QoS | Applied to | Rationale |
|-----|-----------|-----------|
| 0 (at most once) | clock, blade_contact | High frequency or latency-critical. A missed clock tick self-corrects within one second. Blade contact retransmission latency would degrade timestamp precision for video sync. |
| 1 (at least once) | lights, score, connection, state, fencers, match, uw2f, medical, video_review, control | State changes and commands that must not be silently lost. QoS 1 may deliver duplicates — use the `seq` field to detect them (Section 21). |

### 4.5 Retained messages

Apparatus-published topics use retained messages. All software-published topics (`fencers`, `match`, `score`, `clock`, `uw2f`) do **not** use retained messages. `blade_contact` and `control` are also not retained.

Retained apparatus messages mean the broker holds the last published value for every apparatus topic. A subscriber connecting after the apparatus is online — a display app, a recorder, a second CMS — immediately receives the current state without waiting for the next publish cycle. Combined with QoS 1 on all state-bearing topics, this eliminates the need for periodic heartbeat resends.

**All software-published topics are not retained.** The rationale is:

The apparatus is the authoritative source of truth for what is happening on the piste. The CMS is a manager, not a state owner. If software-published messages were retained, a stale value from a previous session would be replayed to a reconnecting apparatus even when no live CMS is present. The apparatus cannot distinguish a retained message from a live one.

- `software/fencers` and `software/match` are initialisation messages: replaying them would assign stale fencer or match data to a fresh apparatus session.
- `software/score`, `software/clock`, and `software/uw2f` are also initialisation messages sent once at match start. If retained, they would be replayed on any MQTT reconnect (including a brief network glitch) and overwrite the live score, clock, and passivity state that the apparatus correctly holds in RAM. The apparatus is the sole authority for these values once a bout is in progress.

A CMS MUST clear any previously retained software messages for `score`, `clock`, and `uw2f` on a given piste whenever it detects the apparatus coming online (`apparatus/connection` with `online: true`). This is done by publishing an empty payload with `retain: true` on each topic, which instructs the broker to delete the retained message.

Connection recovery follows this hierarchy (see also Section 4.7):
1. If the apparatus retains its RAM state (network glitch, no power loss), it republishes its own apparatus topics on reconnect. No CMS action is needed.
2. If the apparatus reboots, it first reloads state from its own non-volatile memory (NVS). Failing that, it reads its own last-known state from the retained apparatus topics on the broker — specifically `lights`, `score`, `state`, `clock`, `uw2f`. The apparatus then publishes its restored state before accepting any input.
3. Only if neither local nor broker apparatus state is recoverable does the user press NEXT, prompting the CMS to republish `fencers` and `match`.

A CMS SHOULD republish `fencers` and `match` whenever it detects an apparatus coming online (`apparatus/connection` with `online: true`), provided the CMS has an active bout assigned to that piste. This restores fencer names after a reboot (case 2) at the cost of a harmless redundant push after a glitch (case 1).

**Recovering from a reboot mid-bout:** After a reboot, the apparatus ALWAYS returns to W, regardless of what `apparatus/state` was retained on the broker. Score, lights, fencers, and match are restored from the broker's retained `apparatus/*` topics and shown on the display as a reference. The FSM resets on every power cycle, so no active bout can be considered live — external updates (NEXT/PREV, DISP) are only accepted in W, making W the only safe recovery state. The referee presses BEGIN (W→H) to restart or re-confirm, and END (H→E) to send the result, or presses NEXT/PREV to move to a different match. The CMS SHOULD re-push `fencers` and `match` on reconnect so the apparatus display is complete from the first moment.

**blade_contact** is not retained because it is a point-in-time event. A retained blade contact message would cause a late subscriber to receive a contact notification with no way to know it was already resolved.

**control** is not retained because commands are one-shot. A late subscriber must not act on a BEGIN or NEXT command that was issued before it connected.

### 4.6 Last Will and Testament

Every apparatus MUST configure a Last Will and Testament (LWT) message when connecting to the broker. The LWT is set in the MQTT CONNECT packet — it is not published by the apparatus directly, but by the broker automatically if the apparatus disconnects unexpectedly.

The LWT MUST be configured as follows:
- **Topic:** `openpiste/{piste_id}/apparatus/connection`
- **Payload:** `{"online": false}`
- **QoS:** 1
- **Retain:** true

### 4.7 Port

Standard MQTT port 1883 (unencrypted) or 8883 (TLS).

---

## 5. Topic structure

All Level 2 topics follow this pattern:

```
openpiste/{piste_id}/{publisher}/{message_type}
```

| Segment | Description | Examples |
|---------|-------------|---------|
| `openpiste` | Fixed platform prefix | — |
| `{piste_id}` | Piste identifier — number, name, or colour | `17`, `podium`, `rouge`, `vert` |
| `{publisher}` | Role of the publishing device — see values below | `apparatus`, `software`, `remote` |
| `{message_type}` | Message type as defined in Section 6 | `lights`, `clock`, `score` |

**Publisher values:**

| Value | Meaning |
|-------|---------|
| `apparatus` | Message published by the scoring apparatus |
| `software` | Message published by competition management software |
| `remote` | Message published by a remote control device |

The `{piste_id}` and `{publisher}` segments in the topic are the **authoritative** sources of piste identity and publisher role respectively. Neither is duplicated in the payload. Consumers that need to store these values alongside the payload (for logging, analytics, or display purposes) MUST extract them from the topic at the time of receipt.

Encoding the publisher role in the topic — rather than in a payload field — allows subscribers to filter by publisher at the broker level, with no payload parsing required. It also maps directly onto broker access control: each publisher role can be restricted to writing only within its own topic namespace (see Section 24). An early draft of this protocol carried a `source` field in every payload for this purpose; this was replaced by the topic segment after considering that routing information belongs in the topic, not in application data.

Useful subscription patterns:

```
openpiste/#                           # all messages from all pistes
openpiste/17/#                        # all messages from piste 17
openpiste/+/apparatus/#               # all apparatus messages from all pistes
openpiste/+/apparatus/lights          # lights from all pistes
openpiste/+/apparatus/score           # scores from all pistes
openpiste/+/+/control                 # control events from all publishers, all pistes
openpiste/+/apparatus/connection      # connection status from all pistes
```

---

## 6. Message overview

| Topic suffix | Publisher | QoS | Retained | Published when |
|-------------|-----------|-----|----------|---------------|
| `lights` | apparatus | 1 | Yes | On any light change |
| `clock` | apparatus | 0 | Yes | Every second while running; on any clock state change |
| `clock` | software | 0 | **No** | At match initialisation (sets starting time) |
| `blade_contact` | apparatus | 0 | No | On blade contact event |
| `score` | apparatus | 1 | Yes | On score, card, or priority change |
| `score` | software | 1 | **No** | At match initialisation or manual correction |
| `connection` | apparatus | 1 | Yes | On connection or disconnection (including LWT) |
| `state` | apparatus | 1 | Yes | On apparatus state change |
| `fencers` | software | 1 | No | On fencer, coach, or referee identity change |
| `match` | software | 1 | No | On match or competition metadata change |
| `uw2f` | apparatus | 1 | Yes | On UW2F timer or P-card change |
| `uw2f` | software | 1 | **No** | At match initialisation (resets passivity timer) |
| `medical` | apparatus | 1 | Yes | On medical timeout event or timer update |
| `video_review` | apparatus or software | 1 | Yes | On video review request or resolution |
| `control` | apparatus, software, or remote | 1 | No | On remote control event |

---

## 7. Common fields

Every Level 2 message contains the following common fields. They appear first in every payload.

| Field | Type | QoS 0 | QoS 1 | Description |
|-------|------|-------|-------|-------------|
| `protocol` | string | Mandatory | Mandatory | Always `"OPP2"` |
| `version` | string | Mandatory | Mandatory | Protocol version — e.g. `"1.0"`. See Section 23. |
| `seq` | integer | Absent | Mandatory | Global sequence counter — see Section 21 |
| `ts` | integer | Mandatory | Recommended | Timestamp — see Section 22 |

`ts` is mandatory on QoS 0 messages (clock, blade_contact) and on control messages. It is recommended on all other QoS 1 messages.

Note that publisher identity — previously carried as a `source` field in the payload — is encoded in the topic's `{publisher}` segment instead. See Section 5 for the rationale.

---

## 8. Message: lights

**Topic:** `openpiste/{piste_id}/apparatus/lights`
**QoS:** 1
**Retained:** Yes

Published immediately on any change to the light state. This is the highest-priority message — published before any other pending message when a light state changes. QoS 1 ensures a missed lights message is retransmitted, preventing subscribers from holding a permanently incorrect light state.

Light colour conventions apply across all weapons:
- **Red** light: left fencer scored (on target)
- **Green** light: right fencer scored (on target)
- **White** light: off-target hit (foil) or broken circuit (sabre)

### Payload

```json
{
  "protocol": "OPP2",
  "version":  "1.0",
  "seq":      42,
  "ts":       1715539200123,
  "right": {
    "green": false,
    "white": true
  },
  "left": {
    "red":   true,
    "white": false
  }
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` |
| `seq` | integer | M | — | Global sequence counter |
| `ts` | integer | M | — | Timestamp of light change, see Section 22 |
| `right.green` | boolean | M | `false` | Right fencer on-target light |
| `right.white` | boolean | M | `false` | Right fencer white (off-target / broken circuit) light |
| `left.red` | boolean | M | `false` | Left fencer on-target light |
| `left.white` | boolean | M | `false` | Left fencer white (off-target / broken circuit) light |

---

## 9. Message: clock

**Topic:** `openpiste/{piste_id}/apparatus/clock`
**QoS:** 0
**Retained:** Yes

Published once per second while the stopwatch is running. Also published immediately on any clock state change (start, stop, reset). QoS 0 is appropriate — a missed clock tick self-corrects within one second.

### Payload

```json
{
  "protocol": "OPP2",
  "version":  "1.0",
  "ts":       1715539200123,
  "running":  true,
  "time_ms":  89250,
  "time":     "1:29.25"
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` |
| `ts` | integer | M | — | Timestamp of this clock publication, see Section 22 |
| `running` | boolean | M | `false` | `true` if the stopwatch is currently running |
| `time_ms` | integer | M | `0` | Current stopwatch value in milliseconds |
| `time` | string | M | `"0:00"` | Formatted as `"M:SS"` or `"M:SS.cc"`. Hundredths mandatory below 10 seconds. |

Note: `seq` is absent on QoS 0 messages.

---

## 10. Message: blade\_contact

**Topic:** `openpiste/{piste_id}/apparatus/blade_contact`
**QoS:** 0
**Retained:** No

Published on blade contact events. The primary purpose of this message is to provide a precise timestamp for synchronisation with video replay systems. Not every blade contact is a scoring touch or a parry — this message records the raw electrical event. It enables referees and AI tools to determine whether a scoring action involved genuine blade contact.

QoS 0 is used because retransmission latency would degrade timestamp precision, which is the primary value of this message.

> **Note:** The full semantics of this message are not yet finalised — see Section 25.

### Payload

```json
{
  "protocol": "OPP2",
  "version":  "1.0",
  "ts":       1715539200089,
  "active":   true
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` |
| `ts` | integer | M | — | Timestamp of contact event, see Section 22 |
| `active` | boolean | M | — | `true` — blade contact detected; `false` — contact cleared |

Note: `seq` is absent on QoS 0 messages.

---

## 11. Message: score

**Topic:** `openpiste/{piste_id}/{publisher}/score`
**QoS:** 1
**Retained:** Yes

Published on any change to scores, cards, or priority. The apparatus publishes under `apparatus/score`; competition management software correcting a score publishes under `software/score`. All subscribers see both; the publisher segment in the topic identifies the origin.

### Payload

```json
{
  "protocol": "OPP2",
  "version":  "1.0",
  "seq":      43,
  "right": {
    "score":       8,
    "status":      "V",
    "yellow_card": false,
    "red_cards":   1,
    "black_card":  false
  },
  "left": {
    "score":       6,
    "status":      "D",
    "yellow_card": false,
    "red_cards":   0,
    "black_card":  false
  },
  "priority": "N"
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` |
| `seq` | integer | M | — | Global sequence counter |
| `right.score` | integer | M | `0` | Right fencer score |
| `right.status` | string | M | `"U"` | Right fencer match status — see values below |
| `right.yellow_card` | boolean | M | `false` | Right fencer yellow card |
| `right.red_cards` | integer | M | `0` | Right fencer red card count (0–9) |
| `right.black_card` | boolean | M | `false` | Right fencer black card |
| `left.score` | integer | M | `0` | Left fencer score |
| `left.status` | string | M | `"U"` | Left fencer match status |
| `left.yellow_card` | boolean | M | `false` | Left fencer yellow card |
| `left.red_cards` | integer | M | `0` | Left fencer red card count (0–9) |
| `left.black_card` | boolean | M | `false` | Left fencer black card |
| `priority` | string | M | `"N"` | `"N"` none, `"R"` right, `"L"` left |

**Status values:**

| Value | Meaning |
|-------|---------|
| `"U"` | Undefined |
| `"V"` | Victory |
| `"D"` | Defeat |
| `"A"` | Abandonment |
| `"E"` | Exclusion |
| `"DNS"` | Did not show |

---

## 12. Message: connection

**Topic:** `openpiste/{piste_id}/apparatus/connection`
**QoS:** 1
**Retained:** Yes

Indicates whether the apparatus is currently connected to the broker. This topic is the target of the LWT configured by the apparatus at connect time (see Section 4.6). The broker publishes `"online": false` automatically if the apparatus disconnects unexpectedly.

### Payload — apparatus online

```json
{
  "protocol":   "OPP2",
  "version":    "1.0",
  "seq":        1,
  "online":     true,
  "device":     "OpenPiste-ESP32",
  "fw_version": "1.0.0"
}
```

### Payload — offline (LWT, published by broker)

```json
{
  "online": false
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` (omitted in LWT payload) |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` (omitted in LWT payload) |
| `seq` | integer | M | — | Global sequence counter (omitted in LWT payload) |
| `online` | boolean | M | — | `true` — apparatus connected; `false` — offline |
| `device` | string | O | — | Device model or identifier |
| `fw_version` | string | O | — | Firmware version of the apparatus |

---

## 13. Message: state

**Topic:** `openpiste/{piste_id}/apparatus/state`
**QoS:** 1
**Retained:** Yes

Indicates the current operational state of the scoring apparatus. Published on every state transition.

### Payload

```json
{
  "protocol": "OPP2",
  "version":  "1.0",
  "seq":      44,
  "state":    "F"
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` |
| `seq` | integer | M | — | Global sequence counter |
| `state` | string | M | `"W"` | Apparatus state — see values below |

**State values** (inherited from EFP1.1):

| Value | Meaning |
|-------|---------|
| `"F"` | Fencing — stopwatch running |
| `"H"` | Halt — stopwatch stopped, bout in progress |
| `"P"` | Pause — between periods |
| `"W"` | Waiting — no active bout |
| `"E"` | Ending — awaiting ACK from software |

### State machine

The following transitions are the only valid ones. An apparatus MUST NOT publish a state that does not result from one of these transitions.

| From | Event | To |
|------|-------|----|
| `W` | BEGIN button | `H` |
| `H` | Timer started | `F` |
| `F` | Timer stopped | `H` |
| `H` / `F` / `P` | END button | `E` |
| `E` | ACK received from software | `W` |
| `E` | NAK received from software | `H` |

**END from W is not a valid transition.** The END button MUST be ignored if the apparatus is in W state. A bout that was interrupted by a reboot is recovered by the referee pressing BEGIN (W→H) followed immediately by END (H→E), which is the normal H→E path requiring no special handling by the CMS.

NAK returns the apparatus to H (not the state it was in before E), because the timer is stopped and the referee must resolve the disputed result before the match can continue.

---

## 14. Message: fencers

**Topic:** `openpiste/{piste_id}/software/fencers`
**QoS:** 1
**Retained:** No — see Section 4.5 for rationale.

Published when any participant identity information changes. In team competitions, republished at the end of each round when fencer assignments change. The message is structured in three sections: `left`, `right`, and `common`.

### Payload

```json
{
  "protocol": "OPP2",
  "version":  "1.0",
  "seq":      45,
  "left": {
    "fencer": {
      "id":     "32",
      "name":   "B. Panini",
      "nation": "ITA"
    },
    "coach": {
      "id":     "c1",
      "name":   "M. Rossi",
      "nation": "ITA"
    }
  },
  "right": {
    "fencer": {
      "id":     "28",
      "name":   "P. Martin",
      "nation": "FRA"
    },
    "coach": {
      "id":     "c2",
      "name":   "J. Dupont",
      "nation": "FRA"
    }
  },
  "common": {
    "referee": {
      "id":     "132",
      "name":   "J. Smith",
      "nation": "GBR"
    },
    "video_official": {
      "id":     "ref002",
      "name":   "L. Dubois",
      "nation": "FRA"
    }
  }
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` |
| `seq` | integer | M | — | Global sequence counter |
| `left.fencer.id` | string | M | — | Left fencer identifier |
| `left.fencer.name` | string | M | — | Left fencer name |
| `left.fencer.nation` | string | M | — | IOC 3-letter nation code |
| `left.coach.id` | string | O | — | Left fencer coach identifier |
| `left.coach.name` | string | O | — | Left fencer coach name |
| `left.coach.nation` | string | O | — | Left fencer coach nation |
| `right.fencer.id` | string | M | — | Right fencer identifier |
| `right.fencer.name` | string | M | — | Right fencer name |
| `right.fencer.nation` | string | M | — | IOC 3-letter nation code |
| `right.coach.id` | string | O | — | Right fencer coach identifier |
| `right.coach.name` | string | O | — | Right fencer coach name |
| `right.coach.nation` | string | O | — | Right fencer coach nation |
| `common.referee.id` | string | O | — | Referee identifier |
| `common.referee.name` | string | O | — | Referee name |
| `common.referee.nation` | string | O | — | Referee nation |
| `common.video_official.id` | string | O | — | Video review official identifier |
| `common.video_official.name` | string | O | — | Video review official name |
| `common.video_official.nation` | string | O | — | Video review official nation |

Optional fields SHOULD be omitted when not available. Receivers MUST handle their absence gracefully. See Section 20 for the mandatory/optional field model.

---

## 15. Message: match

**Topic:** `openpiste/{piste_id}/software/match`
**QoS:** 1
**Retained:** No — see Section 4.5 for rationale.

Published when match or competition metadata changes, including round changes during team competitions.

### Payload

```json
{
  "protocol":    "OPP2",
  "version":     "1.0",
  "seq":         46,
  "weapon":      "E",
  "type":        "I",
  "competition": "efj-eq",
  "phase_type":  "DE",
  "phase":       "3",
  "poule":       "A32",
  "match":       12,
  "round":       1,
  "scheduled":   "13:15"
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` |
| `seq` | integer | M | — | Global sequence counter |
| `weapon` | string | M | — | `"F"` foil, `"E"` épée, `"S"` sabre |
| `type` | string | M | — | `"I"` individual, `"T"` team |
| `competition` | string | M | — | Competition identifier |
| `phase_type` | string | M | — | Phase type — see values below |
| `phase` | string | M | — | Phase identifier |
| `poule` | string | M | — | Poule or tableau identifier |
| `match` | integer | M | — | Match number |
| `round` | integer | M | `1` | Current round or period (team: 1–9; individual: 1–3) |
| `scheduled` | string | O | — | Scheduled start time as `"HH:MM"` |

**Phase type values:**

| Value | Meaning |
|-------|---------|
| `"pool"` | Pool / poule round |
| `"DE"` | Direct elimination |
| `"repechage"` | Repechage |
| `"classification"` | Classification round |

Additional phase type values may be defined in future revisions without a protocol version change.

---

## 16. Message: uw2f

**Topic:** `openpiste/{piste_id}/apparatus/uw2f`
**QoS:** 1
**Retained:** Yes

Published on any change to the unwillingness-to-fight (passivity) timer or P-card state. The UW2F timer counts upward from zero.

### Payload

```json
{
  "protocol": "OPP2",
  "version":  "1.0",
  "seq":      47,
  "time_ms":  60000,
  "time":     "1:00",
  "right": {
    "p_card": 1
  },
  "left": {
    "p_card": 0
  }
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` |
| `seq` | integer | M | — | Global sequence counter |
| `time_ms` | integer | M* | `0` | UW2F timer value in milliseconds, counting up from zero |
| `time` | string | M* | `"0:00"` | UW2F timer formatted as `"M:SS"` |
| `right.p_card` | integer | M | `0` | Right fencer P-card status — see values below |
| `left.p_card` | integer | M | `0` | Left fencer P-card status |

\* At least one of `time_ms` or `time` MUST be present. Implementations MAY include both.

**P-card values:**

| Value | Meaning |
|-------|---------|
| `0` | No P-card |
| `1` | First P-card |
| `2` | Second P-card |
| `3` | Third P-card |
| `4` | Fourth P-card |
| `5` | Fifth P-card |

P-card semantics (which card type corresponds to which ordinal) are defined by the applicable rulebook and may change between rule editions. The protocol records only the ordinal position.

---

## 17. Message: medical

**Topic:** `openpiste/{piste_id}/apparatus/medical`
**QoS:** 1
**Retained:** Yes

Published when a medical timeout is granted and on every subsequent timer update. The medical timeout is initiated via a `MEDICAL` control command (see Section 19) issued by the apparatus when the referee grants the timeout. The countdown timer runs from the duration specified in the initiating control command.

### Payload — timeout active

```json
{
  "protocol":     "OPP2",
  "version":      "1.0",
  "seq":          48,
  "active":       true,
  "side":         "left",
  "duration_ms":  300000,
  "remaining_ms": 247000,
  "remaining":    "4:07"
}
```

### Payload — timeout ended or cleared

```json
{
  "protocol": "OPP2",
  "version":  "1.0",
  "seq":      49,
  "active":   false,
  "side":     "left"
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` |
| `seq` | integer | M | — | Global sequence counter |
| `active` | boolean | M | `false` | `true` — timeout in progress; `false` — timeout ended or cleared |
| `side` | string | M | — | `"left"` or `"right"` — the fencer granted the timeout |
| `duration_ms` | integer | M when active | — | Total timeout duration in milliseconds as specified at initiation |
| `remaining_ms` | integer | M* when active | — | Remaining time in milliseconds, counting down |
| `remaining` | string | M* when active | — | Remaining time formatted as `"M:SS"` |

\* At least one of `remaining_ms` or `remaining` MUST be present when active. Timer resolution is 1 second.

---

## 18. Message: video\_review

**Topic:** `openpiste/{piste_id}/{publisher}/video_review`
**QoS:** 1
**Retained:** Yes

Published when a video review is requested or resolved. Carries both the current remaining call counts and the full call history for the bout. The apparatus publishes under `apparatus/video_review` when a fencer requests a review; competition management software publishes under `software/video_review` when resolving a call.

### Payload

```json
{
  "protocol": "OPP2",
  "version":  "1.0",
  "seq":      50,
  "left": {
    "remaining": 1,
    "calls": [
      {
        "id":      1,
        "round":   1,
        "time_ms": 89250,
        "granted": false
      }
    ]
  },
  "right": {
    "remaining": 2,
    "calls": []
  }
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` |
| `seq` | integer | M | — | Global sequence counter |
| `left.remaining` | integer | M | — | Video review calls remaining for left fencer |
| `left.calls` | array | M | `[]` | History of all video review calls made by left fencer this bout |
| `right.remaining` | integer | M | — | Video review calls remaining for right fencer |
| `right.calls` | array | M | `[]` | History of all video review calls made by right fencer this bout |

**Call history object fields:**

| Field | Type | Description |
|-------|------|-------------|
| `id` | integer | Sequential call identifier, starting at 1 |
| `round` | integer | Round or period in which the call was made |
| `time_ms` | integer | Stopwatch value in milliseconds at the moment of the call |
| `granted` | boolean | `true` — granted; `false` — denied. Absent if not yet resolved. |

**Initial call counts by phase:**
- Pool matches and team matches: 1 call per fencer
- Direct elimination: 2 calls per fencer

These counts reflect current FIE rules and are subject to change. The apparatus or competition management software is responsible for initialising the correct count.

---

## 19. Message: control

**Topic:** `openpiste/{piste_id}/{publisher}/control`
**QoS:** 1
**Retained:** No

Published when a control event occurs. This topic is bidirectional — it carries commands from apparatus to software, from software to apparatus, and from remote controls to apparatus. The publisher segment in the topic identifies the source: `apparatus/control`, `software/control`, or `remote/control`. A receiver that encounters an unknown command value SHOULD ignore it.

### Payload

```json
{
  "protocol": "OPP2",
  "version":  "1.0",
  "seq":      51,
  "ts":       1715539200456,
  "command":  "MEDICAL",
  "side":     "left",
  "duration": 300
}
```

### Fields

| Field | Type | M/O | Default | Description |
|-------|------|-----|---------|-------------|
| `protocol` | string | M | — | Always `"OPP2"` |
| `version` | string | M | — | Protocol version, e.g. `"1.0"` |
| `seq` | integer | M | — | Global sequence counter |
| `ts` | integer | M | — | Timestamp when command was issued, see Section 22 |
| `command` | string | M | — | Command name — see defined values below |
| `side` | string | O | — | `"left"` or `"right"` — for side-specific commands |
| `duration` | integer | O | — | Duration in seconds — for MEDICAL command only |

### Defined command values

| Command | Publisher | Description |
|---------|-----------|-------------|
| `"NEXT"` | apparatus | Request next match or round |
| `"PREV"` | apparatus | Request previous match or round |
| `"END"` | apparatus | Signal end of match or round, awaiting ACK |
| `"MEDICAL"` | apparatus | Medical timeout granted; `side` and `duration` required |
| `"RESERVE"` | apparatus | Reserve fencer introduction; `side` required |
| `"VIDEO_REVIEW_REQUEST"` | apparatus | Fencer requests video review; `side` required |
| `"ACK"` | software | Approve end of match or round |
| `"NAK"` | software | Reject end of match or round |
| `"VIDEO_REVIEW_GRANTED"` | software | Video review call granted; `side` required |
| `"VIDEO_REVIEW_DENIED"` | software | Video review call denied; `side` required |
| `"BEGIN"` | remote | Start the bout |
| `"HALT"` | remote | Call halt |
| `"RESET"` | remote | Reset the apparatus |
| `"VALIDATE"` | remote | Confirm end of match |

Additional command values may be defined in future revisions without a protocol version change.

---

## 20. Field types and conventions

### 20.1 JSON types

| Type | JSON representation | Notes |
|------|--------------------|----|
| Boolean | `true` / `false` | Never `"0"` / `"1"` or string-encoded |
| Integer | JSON number, no quotes | Scores, card counts, millisecond times, sequence counter |
| String | JSON string | Identifiers, names, nation codes, formatted times |
| Timestamp | JSON integer (64-bit) | See Section 22 for encoding convention |

**Formatted time strings** use `"M:SS"` or `"M:SS.cc"` format. Hundredths are mandatory when time is below 10 seconds, consistent with EFP1.1 convention.

**Nation codes** use IOC 3-letter codes (e.g. `"FRA"`, `"GBR"`, `"ITA"`).

### 20.2 Mandatory and optional fields

Each field in the per-message tables is marked **M** (mandatory) or **O** (optional).

**Mandatory fields** MUST be present in every message published by a sender claiming compliance with the declared `version`. A receiver that receives a message with a missing mandatory field SHOULD treat it as a protocol error and MAY discard the message. Mandatory fields that carry a default value in the table have that default defined for receiver use only — a compliant sender must still include the field.

**Optional fields** MAY be absent. When absent, the receiver MUST apply the default value shown in the table. Optional fields with no default (shown as —) have no meaningful default and their absence simply means the information is unavailable; receivers MUST handle this gracefully.

### 20.3 Versioning and field obligations

Which fields are mandatory depends on the protocol version the sender declares in the `version` field. A sender MUST include all mandatory fields defined for the version it declares. A receiver encountering a sender running an older version MUST apply defaults for any mandatory fields that are absent — those fields may not yet have existed when that sender's firmware was written.

Concretely:
- **Receiver knows v1.0, sees v1.0** — enforce mandatory fields strictly; missing mandatory fields are a sender error.
- **Receiver knows v1.1, sees v1.0** — apply defaults for fields added in v1.1; the sender legitimately does not send them.
- **Receiver knows v1.0, sees v1.1** — accept the message; ignore unknown fields per Section 23.2.
- **Receiver encounters an unknown protocol version** — accept permissively; apply defaults for any absent fields.

This model ensures that an older sender remains interoperable in an upgraded system without requiring a firmware update, as long as the message remains structurally valid JSON.

---

## 21. Sequence counter and idempotency

### 21.1 Purpose

MQTT QoS 1 guarantees at-least-once delivery, which means a message may be delivered more than once under certain network conditions. Consumers that perform irreversible actions on receipt — updating a score, issuing a command, recording a video review — must be able to detect and discard duplicate deliveries without processing them twice.

### 21.2 The seq field

Every QoS 1 message carries a mandatory `seq` field: an unsigned 32-bit integer that is incremented by the producer before every publish, regardless of topic. The counter is global — shared across all topics published by one device. It is not reset between topics, only on device reboot.

Using a single global counter means that no two messages from the same device will share the same `seq` value within a session, satisfying per-topic uniqueness as a stronger property. It also allows consumers to reconstruct the cross-topic publish order if needed.

### 21.3 Detecting duplicates

A consumer tracks the last seen `seq` value per producer (identified by piste ID and publisher segment). If a received message carries a `seq` value already seen from that producer, the message is a duplicate and SHOULD be discarded.

### 21.4 Detecting a new session after reboot

On device reboot the counter resets to a low value (typically 1). A consumer distinguishes a reboot from a wraparound by checking the timestamp:

- If `seq` resets to a low value AND the `ts` has advanced significantly → new session, reset the tracked counter
- If `seq` wraps from near `0xFFFFFFFF` to near `0` AND `ts` is continuous → wraparound, not a reboot

### 21.5 Counter wraparound

The 32-bit unsigned counter wraps around after approximately 4.3 billion publishes. At one publish per second this takes over 136 years. Wraparound is not a practical concern but consumers SHOULD handle it gracefully as described above.

### 21.6 QoS 0 messages

`seq` is absent on QoS 0 messages (clock, blade_contact). These messages are inherently lossy by design — the timestamp serves as the primary identity reference for the rare cases where ordering or deduplication matters.

---

## 22. Timestamp conventions

### 22.1 UTC only

All timestamps in Level 2 are UTC. No local time, no timezone offsets, no daylight saving adjustments. Unix epoch milliseconds are by definition UTC — this is not a configuration choice, it is inherent to the format. Implementations MUST use UTC time sources and MUST NOT apply local timezone conversions.

### 22.2 Format

All timestamps are 64-bit unsigned integers. The upper byte (bits 63–56) carries a clock source flag. The lower 56 bits carry the time value in milliseconds.

| Bits | Field |
|------|-------|
| 63–56 | Clock source flag (upper byte) |
| 55–0 | Time value in milliseconds (lower 56 bits) |

### 22.3 Flag values

| Upper byte | Meaning | Lower 56 bits |
|------------|---------|---------------|
| `0x00` | NTP — UTC wall clock | Unix epoch milliseconds, NTP synchronised |
| `0x01` | Session — boot relative | Milliseconds since device boot (`millis()`) |
| `0x02`–`0xFF` | Reserved | — |

### 22.4 NTP timestamps

Current Unix epoch milliseconds are approximately `1.7 × 10¹²` (`0x0000018E...` in hex). The upper byte is naturally `0x00` for the foreseeable future. NTP timestamps therefore require no manipulation at the apparatus — the raw epoch millisecond value is correct.

### 22.5 Session timestamps

When NTP is unavailable, the apparatus SHOULD use milliseconds since device boot with the upper byte set to `0x01`:

```cpp
// NTP available — upper byte is naturally 0x00
uint64_t ts = (uint64_t)epochMillis;

// NTP not available
uint64_t ts = ((uint64_t)0x01 << 56) | (uint64_t)millis();
```

Session timestamps are useful for relative timing within a session but cannot be compared across devices or to wall-clock time.

### 22.6 Reading timestamps

```cpp
uint8_t  flag = (ts >> 56) & 0xFF;
uint64_t time = ts & 0x00FFFFFFFFFFFFFF;
// flag == 0x00: time is UTC Unix epoch milliseconds
// flag == 0x01: time is milliseconds since device boot
```

### 22.7 Video synchronisation

When using blade contact or lights timestamps to synchronise video overlays, both the apparatus and the video system SHOULD be synchronised to the same NTP server. Residual clock drift between devices is typically under 10ms on a well-managed local network.

---

## 23. Versioning and compatibility

### 23.1 Protocol identifier and version

Every message carries two mandatory fields that together identify the protocol:

- `"protocol": "OPP2"` — the protocol family identifier. Fixed for all Level 2 messages.
- `"version": "1.0"` — the protocol version as a `"major.minor"` string.

A receiver SHOULD check the `protocol` field and MAY ignore messages with an unrecognised identifier. The `version` field governs which fields are mandatory — see Section 20.2 and 20.3.

### 23.2 Minor revisions — adding fields

New fields may be added to any message in a minor revision (e.g. `"1.0"` → `"1.1"`). The new fields are mandatory for senders declaring the new version. Receivers that know only the older version will encounter unknown fields, which JSON parsers silently ignore — existing receivers continue to operate correctly. Receivers that know the newer version but encounter a sender declaring an older version MUST apply the defined defaults for the new fields.

### 23.3 Breaking changes

Removing or renaming existing mandatory fields, or changing field types, constitutes a breaking change and requires a new protocol identifier (e.g. `"OPP3"`). The `version` field resets to `"1.0"` with each new protocol identifier.

### 23.4 Adding enumerated values

New values for `command`, `phase_type`, and the `{publisher}` topic segment are not breaking changes and do not require a version increment. Receivers that encounter unknown values SHOULD ignore them.

---

## 24. Security

> **Open item — decision required before production deployment.**

Security for Level 2 has not yet been formally specified. The following considerations apply and will be resolved in a future revision:

**Asymmetric access model.** The appropriate model for most deployments is likely: subscribers (displays, monitors, video tools) may connect and subscribe without authentication on port 1883; publishers (apparatus, remote controls, competition software) SHOULD authenticate using MQTT username/password credentials over TLS on port 8883. This allows open read access while protecting the integrity of scoring data.

**Publisher-scoped access control.** The topic structure maps directly onto a clean broker ACL model. Each publisher role is restricted to writing only within its own namespace: apparatus credentials permit publishing only to `openpiste/+/apparatus/#`; software credentials only to `openpiste/+/software/#`; remote credentials only to `openpiste/+/remote/#`. All authenticated clients may subscribe to `openpiste/#`. This prevents a misconfigured or compromised remote control from publishing score corrections, and prevents software from spoofing apparatus connection state.

**Credential deployment.** For a club setup with a handful of devices, static credentials configured per device are acceptable. For a competition with many pistes and devices, a more automated credential management approach is needed. The operational burden of certificate and credential deployment at scale is a significant consideration and will influence the final recommendation.

**Local network isolation.** For deployments where authentication is not yet implemented, network isolation — restricting broker access to the local competition network — is the minimum acceptable control.

A formal security specification will be added in a future revision.

---

## 25. Open items

**Blade contact semantics.** The blade_contact message currently treats contact as a stateful on/off event. An alternative treats it as a momentary event — a single publish with no corresponding off message. The choice affects whether blade_contact should eventually become a retained message. This will be resolved based on feedback from video referee application developers.

**ACK/NAK state machine.** ~~Not yet specified.~~ Resolved: the full state machine is defined in Section 13. ACK transitions E→W; NAK transitions E→H. END is only valid from H, F, or P — W→E is explicitly forbidden.

**JSON Schema.** A machine-readable JSON Schema for all message types is planned as a separate document at `schemas/opp2/` in the OpenPiste repository. Not yet published.

---

*OpenPiste Protocol Level 2 is released under the MIT licence.*
*Reference implementation and further documentation: https://openpiste.org*
