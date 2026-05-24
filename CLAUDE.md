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

DISP messages arrive via UDP and must update canonical state. Use output parameters,
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
  • async_udp task (~4KB stack) ← constrained
  • FencingStateMachine (10ms tick)
  • Opp2Handler, CyranoHandler, FPA422Handler
  • UDPIOHandler (button input)

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

| Message | QoS | Retained |
|---------|-----|----------|
| lights, score, connection, state, fencers, match, uw2f, medical, video_review, control | 1 | Yes (except control) |
| clock, blade_contact | 0 | clock: Yes, blade_contact: No |
| control | 1 | No |

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

### 🚧 Partial / not tested
- OPP2 software→apparatus control (Fencers, Match, Score, Clock from software) — receiving code exists; auto-detect now enabled; not yet tested end-to-end
- **Protocol auto-detect** — implemented: starts in NONE, first protocol to send a state-changing message wins; resets to NONE on UI_INPUT_RESET; `isProtocolAllowed()` shared by all six external update methods
- UI_SWAP_FENCERS, UI_RESERVE, UI_ABANDON buttons

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

## Source Code Locations

Two directories are available for this session. **Do not modify anything in the main
branch directory — it is read-only reference material.**

| Directory | Branch | Purpose |
|-----------|--------|---------|
| `/home/piet/esp-idfProjects/esp32scoring-main/` | `main` | Clean original code, pre-refactor. Read only. |
| `/home/piet/esp-idfProjects/esp32scoringdeviceMqtt/` | refactor (current) | Partially refactored working copy. This is the working directory. |

Start your analysis by reading the `main` branch source — it is the ground truth for
what the device actually does today. Then compare to the current branch to understand
what has changed during the refactor attempt.

---

## Key Files to Read First

Read in this order:

**From the main branch (original working code):**
1. `/home/piet/esp-idfProjects/esp32scoring-main/src/` — all `.h` and `.cpp` files
2. `/home/piet/esp-idfProjects/esp32scoring-main/main.cpp` (or equivalent entry point)

**From this repo (reference documents + refactor in progress):**
3. `docs/architecture.md` — proposed architecture and rationale
4. `docs/level2.md` — OPP2 protocol specification (authoritative)
5. `src/Opp2Handler.h` and `src/Opp2Handler.cpp` — canonical state owner (refactored)
6. `src/CyranoHandler.h` and `src/CyranoHandler.cpp` — Cyrano protocol + cache (refactored)
7. `src/FencingStateMachine.h` — FSM event definitions
8. `main.cpp` or `src/main.cpp` — wiring of observers and singletons (refactored)

---

## Your Task for This Session

> **Analyse the existing code in the `main` branch and evaluate whether the proposed
> architecture in `docs/architecture.md` is correct, implementable, and the best
> approach — or whether a better architecture should be proposed.**

Specifically:

1. Read all source files in the main branch to understand the original working architecture.
2. Read all source files in the current branch to understand what the refactor has changed.
3. Read `docs/architecture.md` to understand the intended target architecture.
4. Identify what is actually implemented versus what the architecture document describes.
5. Identify any architectural gaps, inconsistencies, or risks not covered by the document.
6. Produce a written assessment with one of these conclusions:
   - **Proceed with proposed architecture** — it is sound, gaps are minor, implementation
     path is clear.
   - **Proposed architecture needs amendments** — describe the specific changes and why.
   - **Alternative architecture recommended** — describe the alternative with rationale,
     and what would need to change.

7. Do NOT make any code changes unless explicitly asked after the assessment is complete.
   Read and analyse only in this first session.

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
