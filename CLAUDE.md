# CLAUDE.md — ESP32 Fencing Scoring Device
## Briefing for Claude Code sessions

> **Read this entire file before touching any code.**
> This is not boilerplate — it encodes hard-won architectural decisions and critical
> constraints that will cause crashes and data corruption if ignored.

## Debugging Protocol (MANDATORY)

> **If you cannot identify the root cause within ~3 minutes of reading code, STOP.**
> State 2–3 candidate causes. Ask for a serial log or clarification. Do not read more files.
> The observer chain has many paths — only runtime logs can reliably trace behavior.
> Never spend more than one exchange reading files before asking for a log.

## Protocol / Spec Decisions (FORBIDDEN without permission)

> **Never change protocol-level behavior** — what is retained, QoS levels, what gets
> published, topic structure, or any other spec-visible behavior — without first:
> 1. Explaining the problem in plain text
> 2. Proposing the change and the rationale
> 3. Waiting for explicit user permission
>
> This applies even when the fix seems obviously correct. The user owns the spec.

## Adding Fields to Established Classes (FORBIDDEN without permission)

> **Never add a member variable, method, or field to an existing handler class**
> (FPA422Handler, Opp2Handler, CyranoHandler) **or any OPP2:: struct** without first:
> 1. Explaining what the problem is in plain text
> 2. Explaining why a new field is necessary (not just convenient)
> 3. Waiting for explicit user permission
>
> This applies even when the addition seems obviously helpful. The user decides what
> gets added to established interfaces.

---

## What This Project Is

An open source ESP32-based fencing scoring device. It is part of a larger platform
(OpenPiste) that covers the full electronics stack for fencing competitions: scoring,
weapon testing, remote controls, and piste monitoring.

The device communicates with competition management software using established fencing
protocols. It runs on an **ESP32 dual-core MCU** using the **Arduino/ESP-IDF framework**.

**Author:** Piet Wauters — FIE SEMI Commission member, EFC SEMI Commission member,
electronic engineer. Volunteer project.

**Repo:** https://github.com/pietwauters/esp32scoringdeviceMqtt

---

## Protocol Stack

The device supports three communication protocols simultaneously:

| Protocol | Purpose | Transport | State authority |
|----------|---------|-----------|----------------|
| **OPP2** | New open protocol (OpenPiste) | MQTT/JSON | **Canonical state owner** |
| **Cyrano / EFP1.1** | Legacy competition management | UDP + MQTT | Reads from OPP2 state |
| **RS422-FPA** | Hardware displays / scoreboards | RS422 serial | Reads from OPP2 state |

**Why keep Cyrano:** There is a large installed base of commercial competition management
software (EnGarde, Engarde-Escrime, etc.) that speaks only Cyrano/EFP1.1. The device must
remain backward-compatible. In any real deployment, only ONE competition management
software will be active at a time — either Cyrano-based or OPP2-based, never both.

**OPP2 specification:** See `docs/level2.md` in this repo. Every message type, field
name, QoS level, retained flag, and topic structure in that document is authoritative.
Do not deviate from it.

---

## Architecture: Single Source of Truth

**The fundamental design decision:** `Opp2Handler` owns ALL piste state. It is the
single source of truth (SSOT). All other handlers read from it; none own their own copy.

```
                    ┌─────────────────────────────┐
                    │        Opp2Handler           │
                    │   OPP2::SystemState m_State  │  ← SSOT
                    │   protected by m_StateMutex  │
                    └──────────┬──────────┬────────┘
                               │          │
               push on change  │          │  push on change
                               ▼          ▼
                    ┌──────────────┐  ┌────────────────┐
                    │CyranoHandler │  │ FPA422Handler  │
                    │ (6 cached    │  │ (reads via     │
                    │  strings)    │  │  getStateCopy) │
                    └──────────────┘  └────────────────┘
```

The word "state" covers multiple distinct domains — always use the precise domain name:

**Internal state** — The machine's own operational data, used for autonomous decisions
regardless of whether any competition management software is present.
Fields: score (L/R), weapon, stopwatch, period/round number, lights, priority, cards
(Y/R/B), P-cards, timer running/stopped, match type (individual/team).
Driven by: `FencingStateMachine`. Stored in: `Opp2Handler::m_State`.

**Information state** — Everything published outward: to repeater screens, to competition
management software (via INFO/OPP2 messages), and to FPA422 displays. Contains all
internal state fields plus identifying data that only arrives from a CMS: fencer names,
nationalities, fencer IDs, competition identifier, phase, poule/tableau, match number,
referee. Identifying fields are empty/unknown until a CMS sends a DISP (Cyrano) or
equivalent OPP2 message.
Owned by: `Opp2Handler::m_State` (SSOT). Pushed to CyranoHandler cache and FPA422Handler.

**Bout state** — The single variable (F/H/P/W/E) that coordinates the three-way
interaction between the referee (buttons), the apparatus (logic), and the competition
management software (protocol messages). Controls which buttons are accepted, whether
DISP is installed or ignored, and what state field INFO messages report.
  - **W** (Waiting): no active match; NEXT/PREV/BEGIN active; DISP installs match data
  - **H** (Halt): match started, timer stopped; referee deciding; all changes send INFO
  - **F** (Fencing): timer running; all changes send INFO
  - **P** (Pause): between periods or medical break; timer typically running
  - **E** (Ending): match finished; waiting for ACK; NAK → display "Incorrect END"
Bout state transitions are caused by exactly two things:
- Physical button presses: BEGIN (W→H), END (Active→E)
- ACK reception from CMS: E→W (the only protocol-driven transition; NAK keeps state at E)
DISP never changes bout state — see Invariant #6.
Stored in: `m_State.apparatus_state` inside `Opp2Handler`.

**System state** — the complete `OPP2::SystemState` struct; the full contents of `m_State`.

---

## CRITICAL: Stack Safety Constraints

**This is the most important section. Violations cause silent crashes and device reboots.**

The `async_udp` task (ESP-IDF's AsyncUDP library) has approximately **4KB of stack**.
UDP packet callbacks execute in this context. This is not enough for:

- `getStateCopy()` — allocates ~400–600 bytes on the stack
- `std::string` building, concatenation, or `sprintf` with large buffers
- JSON serialisation/deserialisation
- Large struct copies (anything over ~100 bytes)

### The push-based cache pattern (mandatory for UDP callbacks)

`Opp2Handler` **pushes** pre-built strings to `CyranoHandler` whenever state changes.
`CyranoHandler` caches 6 final strings (3 message types × 2 formats):
- `m_CachedCyranoString` — INFO in Cyrano wire format
- `m_CachedJsonString` — INFO in JSON (MQTT)
- `m_CachedNextCyrano` / `m_CachedNextJson`
- `m_CachedPrevCyrano` / `m_CachedPrevJson`

UDP callbacks use these cached strings via pointer only — zero stack allocations.

### Safe vs unsafe in UDP callbacks

```cpp
// ✓ SAFE
const char* p = m_CachedCyranoString.c_str(); // pointer only
udp.writeTo((uint8_t*)p, m_CachedCyranoString.length(), ...);

// ❌ CRASH — stack overflow
OPP2::SystemState state = Opp2Handler::getInstance().getStateCopy();
std::string msg = BuildMessage(state);
```

### Zero-copy pattern for DISP messages

> **Update 2026-09-19:** DISP is no longer processed in the UDP callback at all. The
> callback only copies the raw packet into a slot and queues it; the `cyrano_rx` task
> parses and processes it (see the Known Issues entry below). The pattern here still
> applies inside `updateFromCyranoMessage()` (keep its stack footprint small), but the
> "runs in async_udp" warnings no longer describe the DISP path.

DISP messages must update canonical state. Use output parameters,
never `getStateCopy()` in the UDP callback:

```cpp
// ✓ CORRECT
OPP2::ApparatusState apparatusState; // ~4 bytes
Opp2Handler::getInstance().updateFromCyranoMessage(input, apparatusState);
switch (apparatusState) { ... }

// ❌ CRASH
OPP2::SystemState state = Opp2Handler::getInstance().getStateCopy(); // ~600 bytes!
```

**Stack safety checklist** — verify before any code that runs in a UDP callback:
- [ ] No `getStateCopy()` calls
- [ ] No string building or concatenation
- [ ] No JSON serialisation
- [ ] No large struct copies (>100 bytes)
- [ ] Uses cached strings or output parameters only

---

## JSON / Wire-Format Buffer Capacity — Verify, Never Estimate

> **Never size a fixed-capacity serialization buffer (ArduinoJson `StaticJsonDocument`,
> `sprintf`/`snprintf` targets for EFP1/Cyrano messages, etc.) by estimating expected
> output text length.** Build a worst-case-populated message, serialize it for real
> with the actual library call, and confirm the output is complete — not just that the
> call returned success. ArduinoJson v6 pool exhaustion is **silent**: it drops fields
> with no error, no exception, no return-code signal.
>
> If the allocator's per-unit cost is platform-dependent — confirmed true for
> ArduinoJson v6, whose `VariantSlot` is 16 bytes on the real ESP32 (Xtensa, 32-bit)
> target but 32 bytes on 64-bit desktop/native — a native/desktop test is **not**
> sufficient evidence by itself. It can both under-report real capacity (looks broken
> on desktop, is actually fine on-device) and over-report it (looks fine on desktop,
> silently truncates on-device). Get real numbers from the actual target hardware
> before trusting a capacity constant, the same way `docs/level2.md` and this file
> insist on runtime logs over speculation.
>
> This mistake was made twice on this codebase, both times by a Claude Code session —
> the `OPP2::Serializer`'s `JSON_SIZE_*` constants (`opp2-library`,
> `src/opp2_serialize.h`) were originally sized by estimating JSON output text length
> and padding it, which doesn't match ArduinoJson v6's actual per-slot allocation model.
> `JSON_SIZE_LIGHTS=128` silently dropped the entire `"left"` object from every lights
> MQTT message for an unknown period before being caught (2026-08-26); six more message
> types had the same class of bug or near-zero safety margin, found only once every
> constant was re-verified against real hardware. See
> `/home/piet/.claude/projects/-home-piet-esp-idfProjects-esp32scoringdeviceMqtt/memory/project_opp2_json_size_bugs.md`
> for the full incident.

---

## State Update Patterns

### Internal updates (from FencingStateMachine or local logic)
Always accepted. Caller is the state owner.
```cpp
void Opp2Handler::updateScoreInternal(const OPP2::Score& score);
void Opp2Handler::updateLightsInternal(const OPP2::Lights& lights);
void Opp2Handler::updateClockInternal(const OPP2::Clock& clock);
void Opp2Handler::updateApparatusStateInternal(const OPP2::ApparatusStateMsg& msg);
```

### External updates (from protocols)
Subject to protocol priority checking. May be rejected.
```cpp
void Opp2Handler::updateApparatusStateExternal(const OPP2::ApparatusStateMsg& msg, InputProtocol source);
void Opp2Handler::updateFencersExternal(const OPP2::Fencers& fencers, InputProtocol source);
void Opp2Handler::updateMatchExternal(const OPP2::Match& match, InputProtocol source);
```

### After every state update (mandatory sequence):
1. Take `m_StateMutex` → write → release mutex
2. Publish relevant OPP2 MQTT message
3. Push cache to CyranoHandler (`PushCachedStatusToCyrano()`)
4. Notify observers (`notify(EVENT_*)`)

---

## Observer Pattern

```cpp
// Wiring in main.cpp
MyOpp2Handler->attach(*MyCyranoHandler);   // CyranoHandler observes Opp2Handler
MyOpp2Handler->attach(*MyFPA422Handler);   // FPA422Handler observes Opp2Handler
MyFSM->attach(*MyOpp2Handler);             // Opp2Handler observes FSM

// Event constants (defined in Opp2Handler.h or similar)
EVENT_LIGHTS, EVENT_SCORE_LEFT, EVENT_SCORE_RIGHT,
EVENT_STATE_CHANGED, EVENT_CYRANO_SEND_INFO,
EVENT_CYRANO_SEND_NEXT, EVENT_CYRANO_SEND_PREV
```

**CyranoHandler** reacts to send events from Opp2Handler — it never initiates sends itself.
**FPA422Handler** reacts to state change events — it calls `getStateCopy()` (safe, it runs in Core 0).

---

## Threading Model

```
Core 0 (PRO_CPU) — Protocol & Network
  • WiFi/lwIP
  • MQTT client (AtlasAsyncMqttClient)
  • async_udp task (~4KB stack) ← constrained; UDP callbacks must only filter/copy/queue
  • cyrano_rx task (8KB) — CMS packet processing (EFP1 parse, DISP→update→INFO)
  • opp2_ui_evt task (8KB) — button/UI events
  • FencingStateMachine (10ms tick)
  • Opp2Handler, CyranoHandler, FPA422Handler
  • UDPIOHandler (button input)
  • BrokerDiscovery::searchTask — background mDNS race while MQTT is
    disconnected (see RTOSSettings.h for the full current task list;
    this diagram is not exhaustively kept in sync with it)

Core 1 (APP_CPU) — Real-Time Weapon Sensing
  • 3WeaponSensor (150µs scan, ~6.6kHz ADC)
```

FreeRTOS mutex (`m_StateMutex`) protects `OPP2::SystemState` for dual-core access.
`getStateCopy()` is safe on Core 0 (ample stack). Never call it from Core 1 or async_udp.

---

## OPP2 Protocol Conventions (from docs/level2.md)

### Topic structure
```
openpiste/{piste_id}/{publisher}/{message_type}
```
Publisher values: `apparatus`, `software`, `remote`.
Piste ID and publisher are in the topic — **never duplicated in the payload**.

### QoS and retained rules

| Message | Publisher | QoS | Retained |
|---------|-----------|-----|----------|
| lights, score, connection, state, uw2f, medical, video_review | apparatus | 1 | Yes |
| fencers, match | apparatus | 1 | Yes |
| fencers, match | **software** | 1 | **No** — stale retained CMS data would replay on apparatus reconnect |
| clock | apparatus | 0 | Yes |
| blade_contact | apparatus | 0 | No |
| control | software/remote | 1 | No |

### Mandatory common fields
Every QoS 1 message: `protocol` ("OPP2"), `version` ("1.0"), `seq` (global counter).
Every message: `ts` (mandatory on QoS 0; recommended on QoS 1).
`seq` is absent on QoS 0 messages.

### Cyrano protocol identifier
When building EFP1.1 messages, the Protocol field MUST be `"EFP1.1"`, never `"OPP2"`.
This caused a real bug — commercial software silently ignores messages with wrong identifier.

---

## Protocol Priority

In real deployments, only one competition management software is active at a time
(either Cyrano-based or OPP2-based). The design goal:

- **Auto-detect:** first protocol to send a state-changing message (DISP or Match)
  becomes the active input protocol.
- **Manual override:** user can force a specific protocol.
- `isProtocolAllowed(source)` guard enforces this in external update methods.

Currently, auto-detect logic is not yet implemented — both protocols are accepted.
Do not implement auto-detect unless explicitly asked to. Note the gap; do not fill it.

---

## What Is Complete vs In Progress

### ✅ Working
- Canonical state in Opp2Handler with mutex
- Push-based cache for Cyrano (6 strings)
- Zero-copy DISP handling
- Button routing through Opp2Handler
- OPP2 MQTT publishing: Connection, ApparatusState, Lights, Clock, Score, Fencers, Match, UW2F
- MQTT message routing (OnMqttMessageStatic dispatcher)
- FPA422Handler observes Opp2Handler
- NEXT/PREV/BEGIN/END buttons working
- **Cyrano CMS end-to-end** — DISP→INFO roundtrip complete; CMS accepts INFO and sends ACK (fixed 2026-05-23)
- **FPA422 score update on change** — score messages sent immediately on state change events, not only periodically (fixed 2026-05-23)
- **OPP2 retained MQTT guard** — input protocol defaults to CYRANO; retained OPP2 messages at boot cannot overwrite state (fixed 2026-05-23)
- **DISP→FSM sync** — DISP updates canonical state AND syncs FSM internal state (score, cards, clock, weapon) via m_pFSM pointer; weapon sync guarded — only applied when DISP contains a weapon field (fixed 2026-05-24)
- **FPA422 full refresh from canonical state** — update(Opp2Handler*) now pushes all message types: score + cards (Y/R/B) + priority + round (Msg3), clock (Msg2), weapon (Msg4), fencers (Msg5/6), P-cards (Msg8) (fixed 2026-05-24)
- **BladeContact publishing** — MASK_PARRY transitions in ProcessLightsChange publish blade_contact (QoS 0, not retained) on contact/release (2026-05-24)
- **Clock 03:01 anomaly fixed** — FencingTimer's m_Hundredths=100 "top of second" sentinel was being treated as 1000 ms extra; clamped to 0 in Opp2Handler EVENT_TIMER handler (fixed 2026-05-24)
- **Protocol auto-detect** — starts in NONE, first protocol to send a state-changing message wins; resets to NONE on UI_INPUT_RESET; `isProtocolAllowed()` shared by all external update methods (2026-05-24)
- **OPP2 CMS end-to-end** — inbound software/fencers+match+score+clock+uw2f accepted with guards; all external update methods now follow full mandatory sequence (publish → push cache → notify); FSM synced for weapon, score, clock corrections; tested (2026-05-24)
- **UI_SWAP_FENCERS** — swaps fencers, score, lights, uw2f under mutex; flips priority; syncs FSM (2026-05-24)
- **software/fencers and software/match retained=No** — spec and rationale documented in docs/level2.md §4.5; apparatus/fencers+match remain retained (recovery state) (2026-05-24)
- **Broker connection: static IP first, mDNS as background recovery** — `CyranoHandler::Begin()` points the MQTT client at the static/configured `MqttBroker` IP immediately (no blocking mDNS wait); a new `BrokerDiscovery` singleton (`src/BrokerDiscovery.h/.cpp`) races a background `mdns_query_a("openpiste", ...)` lookup against the client's own built-in reconnect for as long as nothing is connected, switching over via `AtlasAsyncMqttClient::reconnectWithNewSettings()` if mDNS resolves to a different address. Stops the race entirely once connected, resumes on the next disconnect — no periodic re-check of a working connection. Design rationale (never depend on mDNS being available at all, never let a single early miss be a permanent decision) discussed and agreed with the user 2026-09-18 (fixed 2026-09-18)
- **NTP target derived from the broker's resolved address, not its own hostname resolution** — `AbsoluteTime` no longer does its own DNS/mDNS lookup for the NTP server. `Opp2Handler::OnMqttConnectStatic()` re-points it at `mqttClient.getHost()` — whichever address actually won the broker connection race — every time a connection is established, so NTP always targets a host that's already known reachable. The NTP client itself is a standard ESP-IDF `esp_sntp` client (plain SNTP over UDP/123); it has no chrony-specific (or any other server-specific) dependency — any standards-compliant NTP server reachable at that address works (fixed 2026-09-18)

### 🚧 Partial / not tested
- UI_RESERVE, UI_ABANDON buttons

### ❌ Not started
- Medical and VideoReview publishing
- Team match support
- Configuration web UI

### ⚠️ Known OPP2 spec gaps (do not fix until team competition is in scope)
- **Medical intervention count**: Cyrano R10/L10 track a cumulative per-fencer count (0–9).
  Applies to BOTH individual and team competitions — a single fencer may have multiple
  medical timeouts for different injuries. `OPP2::Medical` has only the active timer.
  Fix: add `uint8_t medical_count` per side to `OPP2::Medical` and the spec.
- **Reserve fencer flag**: Cyrano R11/L11 carry a persistent N/R flag per fencer per round.
  `OPP2::SystemState` has no equivalent field (only a one-shot Control command). Fix: add
  `bool reserve_active` to `OPP2::FencerSide` and the spec.

---

## Known Issues — Whole-Codebase Architecture Audit (2026-07-31)

A full-codebase review (beyond OPP2) found the following. Status markers are updated as
items are fixed; do not silently fix items marked 🚧 — see "How to Work in This Project."

### 🔴 Critical — Invariant #3 violated on the write/event path — ✅ Fixed (2026-07-31)
The push-cache discipline (Invariant #3) was applied to the **read/send** side
(CyranoHandler UDP sends use cached strings) but not the **write/event-fan-out** side.
Every button press and every Cyrano DISP packet ran stack-heavy code inside the
`async_udp` task (~4KB stack). Two call sites, two different fixes (they aren't
interchangeable — see rationale below):

- **`UDPIOHandler.cpp` `onPacket` → `InputChanged()` → `notify()` →
  `Opp2Handler::update(UDPIOHandler*)` → `ProcessUIEvents()`** (mutex + JSON + blocking
  `mqttClient.publish()`). Fixed by applying the existing FPA422 queue+task pattern
  verbatim (template actually lives at `FPA422Handler.cpp:663-757`, not
  `Opp2Handler.cpp` — the old note had the wrong file). `update(UDPIOHandler*)` now just
  posts to a new `m_UIEventQueue`; a dedicated `uiEventTask()` (4096 stack, core 0,
  priority 2) calls `ProcessUIEvents()`. Safe because UDPIOHandler's observers
  (NetWork/FSM/CyranoHandler/Opp2Handler) are independent — nothing downstream depends on
  Opp2Handler's reaction finishing synchronously.

- **`Opp2Handler::updateFromCyranoMessage()`** — could NOT use the same fire-and-forget
  queue pattern: `CyranoHandler::ProcessMessageFromSoftware()`'s DISP branch calls it and
  then *synchronously* calls `SendInfoMessage()`, which must reflect the state the DISP
  just set (Invariant #7 — CMS validates INFO echoes everything DISP sent; a race would
  silently break the CMS handshake, the exact bug class fixed 2026-05-23). Deferring to a
  background task would have reintroduced that race. Fixed instead by cutting the stack
  footprint in place, keeping everything synchronous:
  - `convertOpp2ToCyrano()` changed from return-by-value to an output parameter — was
    building one `EFP1Message` (~1.3KB, 41 `std::string` fields) locally and returning a
    second one into the caller, up to 2 live copies without guaranteed NRVO (pre-C++17).
    Now exactly one.
  - `PushCachedStatusToCyrano()` no longer takes a full `OPP2::SystemState` stack copy
    (~500-600 bytes) — converts directly from `m_State` while holding `m_StateMutex`
    (safe: `convertOpp2ToCyrano()` is a pure conversion, no calls back into Opp2Handler).
  - `CyranoHandler::RebuildCachedStrings()` no longer copies `m_CachedStatus` into a local
    `EFP1Message msg` (~1.3KB) just to overwrite two fields — mutates `m_CachedStatus` in
    place (Command/CompetitionId are unconditionally overwritten on every call anyway).

  Combined, worst-case stack for a DISP round-trip was *estimated* to drop from an
  unsafe ~4-4.5KB+ to ~2.6-3KB.

  **⚠️ That estimate was wrong — superseded 2026-09-19.** On real hardware every DISP
  from Engarde still overflowed `async_udp` (core dump: "stack overflow in task
  async_udp"), rebooting the device to state W with no match data — seen as "NEXT/PREV
  never shows the next bout". `ProcessCyranoPacket()` also built a full `EFP1Message`
  (~1.3KB) on the callback stack *before* any of the above. The premise that a task
  couldn't be used was too narrow: deferring only the *update* would race the INFO
  reply, but deferring the *whole* DISP→update→INFO sequence into one task keeps the
  ordering (Invariant #7) intact. Fixed (`e91dc29`): the callback now only filters by
  sender IP, copies the packet into a preallocated slot (`CyranoHandler::m_RxSlots`) and
  queues the slot index (`EnqueueSoftwarePacket()`); a dedicated `cyrano_rx` task (8192
  stack, core 0, priority 2 — `RTOSSettings.h`) builds the `EFP1Message` and runs
  `ProcessMessageFromSoftware()`. Queue depth is `kRxSlots-1` so the producer can never
  overwrite the slot being processed. **Never trust a hand-estimated stack figure for
  this path — measure (core dump, or `ENABLE_STACK_HWM_LOGGING`).** `cyrano_rx`'s real
  high-water mark has not been measured yet.
  (`Opp2Handler.cpp` `OnMqttMessage` also calls `ProcessMessageFromSoftware()` for the
  MQTT Level 1 path, from the MQTT task — unchanged.)

### 🟡 Correctness bugs found
- ✅ Fixed (2026-07-31) `adc_calibrator.cpp:19` — `r1_eff = 495, 6;` comma-operator bug;
  only `495` was assigned, `, 6` silently discarded. Now `r1_eff = 495.6f;`.
- ✅ Fixed (2026-07-31) `RS422_FPA_Type5_Message.cpp:35-40` — `operator=` was a stub that
  did nothing after the self-assign check; now copies `m_message` element-wise.
- ✅ Fixed (2026-07-31) `WS2812BLedStrip.h:146` — `m_LedStatus` had no initializer, so
  `SetLedStatus()`'s first call could compare against uninitialized memory and silently
  drop the first real status update. Now initialized to `0xFFFFFFFF` (a sentinel outside
  all real mask combinations).
- ✅ Fixed (2026-07-31) `TimeScoreDisplay.cpp:504-530` — `char text[6]; sprintf(text,
  "P-%03d", PisteId)`; `PisteId` from NVS had no range check, so a value ≥1000 overflowed
  the 6-byte buffer. `DisplayPisteId()` now clamps to [0, 999] before formatting.
- **Not a bug — intentional, confirmed 2026-07-31**: `WS2812BLedStrip.cpp:320` —
  `setParry()`'s unconditional `return;` is a deliberate temporary disable (commit
  `1e35515`, "Temporarily disable the display of parries as it interferes with the UW2F
  timer display"). Do not re-enable without addressing the UW2F display conflict.
- **Not a bug — intentional, confirmed 2026-07-31**: `WS2812BLedStrip.cpp:895-934` —
  `setRedPCardRight/Left`'s `theFillColor2` is unused by design; 2 red P-cards trigger a
  full white-panel indicator (`setWhiteRight/Left(true, true)`) instead of lighting a
  second red pixel. Works as intended per user confirmation — leave as-is.
- ✅ Fixed (2026-08-26) — 4 sites formatted `PisteNr` via unbounded
  `sprintf("%03d"/"%.3d", ...)` into undersized buffers: `CyranoHandler.cpp:85-86`
  (heap overflow — `PisteNr` is `uint32_t`; NVS "unset" sentinel `-1` wraps to a
  10-digit value on assignment, didn't fit the 11-byte alloc), `RS422_FPA_Type10_
  Message.cpp:41-47`, `network.cpp:499-500`, `WifiSetupMode.cpp:67-68` (all three
  stack overflow, `char temp[8]`). `"%03d"` zero-pads but does not cap max width, so
  any value needing more than 3 digits (or negative) overflowed. RS422's field and the
  two WiFi-SSID sites now clamp to `[0, 999]` before formatting — same precedent as
  `TimeScoreDisplay::DisplayPisteId()` below — since those destinations have a real
  fixed-width constraint (protocol field / SSID-matching convention); the MQTT client
  ID has no such constraint, so it's widened (bigger buffer + `snprintf`) instead of
  clamped, to avoid colliding two different real piste numbers onto one client ID.
- ✅ Fixed (2026-09-18) `AbsoluteTime.cpp` `getTimestamp()` — the NTP-synced branch used
  `time()`, which only has whole-second resolution, so every OPP2 `ts` value silently
  came out as a multiple of 1000ms despite the wire format (docs/level2.md §22) carrying
  real millisecond precision — the entire point of which is sub-second accuracy for video
  replay sync. The fallback (no-broker) branch was already correct
  (`esp_timer_get_time()/1000`). Switched to `gettimeofday()`, which SNTP's smooth-sync
  `adjtime()` adjusts the same way it adjusts `time()`, so the real sub-second value now
  carries through. Root cause of the original "local timestamps" / "strange large time
  differences" report this fix started from was a separate, now-also-fixed bug: `mdnsName`
  (`"openpiste"`, no `.local` suffix) was being handed directly to
  `AbsoluteTime::begin()`/SNTP, which never resolved — see the broker-discovery entry
  above for the actual fix (NTP no longer does its own hostname resolution at all).

### 🟡 Concurrency / mutex discipline
- ✅ Fixed (2026-07-31) `Opp2Handler.cpp` — did the dedicated pass across the whole file
  (206 raw `m_State.` references vs. 31 lock/unlock pairs at the time). Found and fixed
  unguarded reads in:
  - `update(FencingStateMachine*, ...)` — all 13 event cases (`EVENT_WEAPON`,
    `EVENT_SCORE_LEFT/RIGHT`, `EVENT_TIMER_STATE`, `EVENT_TIMER`, `EVENT_ROUND`,
    `EVENT_YELLOW/RED/BLACK_CARD_LEFT/RIGHT`, `EVENT_P_CARD`, `EVENT_PRIO`,
    `EVENT_UW2F_TIMER`) copied `m_State.X` into a local before mutating and writing back
    via `updateXInternal()`, unguarded — a concurrent writer (async_udp task processing a
    DISP, or the new `uiEventTask`) could be overwritten (lost update).
  - `ProcessLightsChange()` — same pattern, high-frequency (fires on every light change).
  - `ProcessUIEvents()` `UI_INPUT_CYRANO_END` — checked `m_State.apparatus_state.state`
    unguarded before deciding whether END is a valid transition.
  - `CheckConnection()`'s boot-recovery-window-close block — ~15 individual unguarded
    reads spread across FSM-sync calls; replaced with one mutex-protected snapshot at the
    top, used for the rest of the block.
  - `SetPisteID()` — wrote `m_State.piste_id` via `strncpy` with zero mutex protection
    (its sibling `getPisteId()` already correctly locks). No live caller today
    (`CyranoHandler::SetPisteID` forwards to it but is itself never called), so not an
    active race, but fixed for consistency since it's part of the public API surface.

  Everywhere else `m_State` is touched (Publish* functions, `updateXXXInternal/External`,
  `updateFromCyranoMessage()`, `UI_SWAP_FENCERS`, `ClearIdentifyingData()`,
  `ProcessBootRecovery()`) was already correctly bracketed by
  `xSemaphoreTakeRecursive`/`xSemaphoreGiveRecursive` — verified, not just assumed.
  Remaining unguarded reads are `m_State.piste_id` lookups in `CheckConnection()`,
  `Begin()`, and `OnMqttConnectStatic()` (topic-building/logging) — left as-is: piste_id
  is effectively write-once at boot, and these run at boot/connect time with low
  concurrency pressure. Flagging here rather than fixing preemptively.
- `WS2812BLedStrip.cpp` — three separate tasks (FSM-driven direct calls, `LedStripHandler`
  queue-draining task, `LedStripAnimator` task) touch `m_pixels` and status fields with no
  mutex; `m_animationRunning` is only a `volatile bool` hint, not a lock.
- `AutoRef.cpp:303,337` — `handleDoubleHit`/`handleTimerZero` call synchronous
  `vTaskDelay` up to ~7s total without re-feeding the task watchdog during the wait, and
  incoming queue events aren't processed until the delay returns.
- `foil.cpp:133` / `epee.cpp:97` — `vTaskDelay(0)` inside the Core-1 150µs ADC scan loop
  forces a scheduler yield, injecting jitter. `sabre.cpp`'s equivalent loop has no such
  call — unexplained inconsistency between the three weapon files.

### ⚪ Minor hygiene (low priority)
- `TimeScoreDisplay.cpp:33`, `WS2812BLedStrip.cpp:105` — glyph/digit lookup tables are
  mutable globals instead of `const`/`constexpr`, wasting RAM on read-only data.
- `FPA422Handler.cpp:44-55` — dead global Wi-Fi/BLE credential strings behind
  `#ifdef ALLOW_BLUETOOTH`, unused since real credentials come from `Preferences`.
- `EFP1Message.h:113` — `void const print() const;` is a meaningless const-qualified void
  return.
- `Opp2Handler.cpp` is 2734 lines — state ownership, publishing for 3 protocols, DISP
  parsing, FSM sync, and the FPA422 queue task all live in one file. Not necessarily wrong
  for "canonical state owner," but worth revisiting if it keeps growing.

---

## Invariants — Never Violate These

1. `Opp2Handler` is the ONLY owner of `OPP2::SystemState`. No other class stores a
   parallel copy of system state.

2. All state writes go through `Opp2Handler` update methods. Never write directly to
   `m_State` from outside `Opp2Handler`.

3. UDP callback code (async_udp task) never calls `getStateCopy()` or builds strings.
   It uses cached strings or output parameters only.

4. After every state change: publish MQTT → push cache → notify observers. All three.
   Never skip one.

5. EFP1.1 wire messages use Protocol field `"EFP1.1"`. OPP2 MQTT messages use
   `"protocol": "OPP2"`. These are different things; never mix them.

6. DISP messages from Cyrano software do NOT change bout state (W/H/F/P/E).
   Bout state is changed only by button presses (BEGIN, END) or ACK reception (E→W).

7. **All 41 EFP1 fields from a DISP must roundtrip to INFO.** The CMS validates that
   every field it sent in DISP appears in the INFO response. Any missing field causes
   silent rejection — the CMS stops responding without any error message.
   When touching `updateFromCyranoMessage()` or `convertOpp2ToCyrano()`, verify the
   full `EPF1SubMessage` enum (src/EFP1Message.h) against what is extracted and emitted.
   The four critical match identification fields (PhaseNumber → `match.phase`,
   Poule_Tableau_Id → `match.poule`, MatchNumber → `match.match_num`,
   CompetitionType → `match.type` with "I"↔Individual / "T"↔Team) were missing until
   the 2026-05-23 fix. Note: `CompetitionType` maps to `match.type` (Individual vs Team),
   NOT `match.phase_type` (Pool vs DE).

8. **FPA422Handler::update(Opp2Handler*, ...) must send ALL message types that depend
   on the state it reads.** When it calls `getStateCopy()`, it must update and transmit
   score messages (Message3) in addition to fencer messages (5, 6). External state
   changes (e.g. DISP resetting score to 0) only arrive via the Opp2Handler observer
   path — if score messages are not sent there, FPA displays lag until the periodic tick.

---

## Coding Conventions

- C++14 or later, Arduino/ESP-IDF framework
- No dynamic allocation in hot paths or UDP callbacks
- Singletons accessed via `getInstance()` (existing pattern — follow it)
- Prefer `const&` parameters to avoid copies
- Output parameters for lightweight return values from constrained contexts
- Log with `ESP_LOGI/W/E` macros — tag should identify the handler class

---

## Source Code Location

The working directory is `/home/piet/esp-idfProjects/esp32scoringdeviceMqtt/` on the `main` branch.
The OPP2 canonical state refactor is complete and merged. There is no separate reference directory.

---

## Key Files to Read First

Read in this order:

1. `docs/level2.md` — OPP2 protocol specification (authoritative)
2. `docs/level1.md` — EFP1.1 over MQTT (Level 1 transport spec)
3. `src/Opp2Handler.h` and `src/Opp2Handler.cpp` — canonical state owner (SSOT)
4. `src/CyranoHandler.h` and `src/CyranoHandler.cpp` — Cyrano protocol + push-based cache
5. `src/FencingStateMachine.h` — FSM event definitions
6. `src/main.cpp` — observer wiring and singleton setup

---

## How to Work in This Project

- One task at a time. Complete and verify before moving to the next.
- State what you are about to do before doing it.
- If you are uncertain about a constraint (especially stack safety or state ownership),
  ask before implementing.
- Do not invent features not described in this file or the protocol spec.
- Do not refactor opportunistically — only change what is in scope for the current task.
- Reference the OPP2 spec (docs/level2.md) for any message-level questions.
- If you find something that seems wrong in the existing code, note it — do not silently fix it.
