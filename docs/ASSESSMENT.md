# Architectural Assessment — ESP32 Fencing Scoring Device

**Branch under review:** `opp2-canonical-state`  
**Reference:** main branch + `docs/ARCHITECTURE.md`  
**Date:** 2026-05-23

---

## Conclusion

**Proposed architecture needs amendments before the implementation can be declared correct.**

The core architectural decisions in `ARCHITECTURE.md` are sound and the right approach for this platform. However, the current refactored code has several concrete defects — one of which prevents compilation — and the document itself contains one item that contradicts a stated invariant. None of these require rethinking the architecture; they are correctness gaps that need to be closed before the next phase of development can proceed safely.

---

## What Was Assessed

### Main Branch (original, working)
- `CyranoHandler` held its own state (`EFP1Message m_MachineStatus`) and was effectively the state owner
- `Opp2Handler` was a later addition that *observed* `CyranoHandler` via string notifications to derive its own copy of state
- `FencingStateMachine` drove both handlers via observer events
- No mutex protection on OPP2 state
- Both handlers maintained independent, parallel copies of score/weapon/cards/etc.
- `ProcessUIEvents` for NEXT/PREV/BEGIN/END lived in `CyranoHandler`

### Refactored Branch (current)
The refactor has made substantial progress. What has been correctly implemented:
- `Opp2Handler` now owns `OPP2::SystemState m_State` protected by `m_StateMutex`
- Push-based cache in `CyranoHandler` (6 strings: INFO/NEXT/PREV × Cyrano wire + JSON)
- `CyranoHandler` now observes `Opp2Handler` for `EVENT_CYRANO_SEND_*` events
- `ProcessUIEvents` for NEXT/PREV/BEGIN/END moved to `Opp2Handler`
- `updateFromCyranoMessage()` added for zero-copy DISP forwarding
- Internal vs. external update method split with protocol guards
- `EVENT_CYRANO_SEND_INFO/NEXT/PREV` event constants added

---

## Issues Found

### Issue 1 — Compilation error: `updateFromCyranoMessage` signature mismatch [BLOCKING]

`Opp2Handler.h:89-91` declares:
```cpp
bool updateFromCyranoMessage(const class EFP1Message &msg,
                             OPP2::ApparatusState &outApparatusState);
```

`Opp2Handler.cpp:1285` implements:
```cpp
bool Opp2Handler::updateFromCyranoMessage(const EFP1Message &EFP1Input) {
```

`CyranoHandler.cpp:228` calls it with two arguments:
```cpp
Opp2Handler::getInstance().updateFromCyranoMessage(input, apparatusState);
```

The header and the call site match; the implementation does not. The code will not compile. The implementation must be updated to accept the output parameter and return the apparatus state through it, consistent with the zero-copy pattern described in the architecture document.

---

### Issue 2 — Invariant #6 violated: DISP still applies apparatus State field [CORRECTNESS]

CLAUDE.md invariant #6 states: *"DISP messages from Cyrano software do NOT change apparatus state (W/H/F/P/E). Only physical buttons change apparatus state."*

However, `updateFromCyranoMessage()` (`Opp2Handler.cpp:1435-1449`) parses and unconditionally applies the State field from DISP:
```cpp
if (EFP1Input[State] != "") {
    if (EFP1Input[State] == "W") {
        m_State.apparatus_state.state = OPP2::ApparatusState::WAITING;
    } ...
    apparatusStateChanged = true;
}
```

The comment on line 1431 says *"Per architecture, DISP doesn't change protocol state"* but then does exactly that. Following this code path, `CyranoHandler::ProcessMessageFromSoftware` reads back `apparatusState` and fires `StateChanged(EVENT_CYRANO_STATE_*)` which propagates to `FencingStateMachine`.

This contradicts both the architecture document (section 6, Scenario 3 states "State Changes: NONE (stays in WAITING)") and the invariant. The State-field parsing block should be removed from `updateFromCyranoMessage`. The apparatus state change from DISP is correctly a no-op per spec; the current code makes it an active change.

---

### Issue 3 — `notify(0)` aliases EVENT_LIGHTS: misleading and dangerous [CORRECTNESS]

After DISP/INFO processing in `update(CyranoHandler*, const std::string&)` and `updateFromCyranoMessage`, state change notifications are fired with:
```cpp
StateChanged(0); // Generic state change notification
```

`StateChanged` calls `notify(eventtype)`. But `EVENT_LIGHTS` is defined as `0x00000000`. So every DISP-driven state change fires what observers interpret as a lights event with data 0 (all lights off). `FPA422Handler` observes `Opp2Handler` and will receive `update(Opp2Handler*, 0)` — whatever it does with that determines whether this produces wrong behaviour on the hardware display.

A dedicated `EVENT_STATE_CHANGED` constant should be added to `EventDefinitions.h` (with a non-conflicting value such as `0x40000000`) and used for generic state change notifications.

---

### Issue 4 — Mandatory post-update sequence not consistently followed [CORRECTNESS]

CLAUDE.md requires after every state change: (1) publish MQTT, (2) push cache, (3) notify observers.

Checking the internal update methods:

| Method | Publish | PushCache | Notify |
|---|---|---|---|
| `updateLightsInternal` | ✅ | ✅ | ✗ |
| `updateScoreInternal` | ✅ | ✗ | ✗ |
| `updateClockInternal` | ✅ | ✗ | ✗ |
| `updateApparatusStateInternal` | ✅ | ✗ | ✗ |
| `updateMatchInternal` | ✅ | ✗ | ✗ |
| `updateUW2FInternal` | ✅ | ✗ | ✗ |

Only `updateLightsInternal` performs PushCache. None of them call notify. The cache push is partially compensated by an unconditional `PushCachedStatusToCyrano()` at the bottom of `update(FencingStateMachine*, uint32_t)` (line 980) — but that call is unconditional (even when nothing changed), runs for every FSM event including timer ticks, and doesn't cover the external update paths.

The missing `notify()` calls are the more serious gap. `FPA422Handler` is attached to both `FencingStateMachine` and `Opp2Handler` in `main.cpp` (lines 141 and 160) — this double attachment appears to be a workaround for the missing notify calls from Opp2Handler's internal update methods. The FSM attachment on line 141 should be removed and replaced with proper `notify()` calls inside the internal update methods.

---

### Issue 5 — Race condition: Publish methods read `m_State` without mutex [CORRECTNESS]

All Publish methods (e.g., `PublishLights()`, `PublishScore()`) directly access `m_State` fields without taking `m_StateMutex`. For example `PublishLights()` reads `m_State.lights.left.on_target` etc. These methods are called *after* the mutex is released by the update methods. Between the release and the publish call, another task could modify state.

In practice this race is narrow because all sensor and protocol work runs on Core 0. However, the architecture document claims mutex protection for dual-core safety, and the publish methods are a documented hole in that protection. The simplest fix is to snapshot the relevant field to a local variable under the mutex before publishing, or expand the mutex scope to cover the serialization.

---

### Issue 6 — Dead code: `update(CyranoHandler*, const std::string&)` is never called [CLEANLINESS]

`Opp2Handler` still observes `CyranoHandler` for string events (`CyranoHandler::attach(*MyOpp2Handler)` in `main.cpp:159`). The handler `Opp2Handler::update(CyranoHandler*, const std::string&)` (lines 1043–1279 of Opp2Handler.cpp) parses DISP/INFO fields and updates `m_State` directly. However, in the refactored `CyranoHandler`, `StateChanged(std::string)` is never called — the DISP case was replaced with `updateFromCyranoMessage`. This ~240-line method is dead code. It also bypasses the internal update methods (writes `m_State.fencers`, `m_State.score`, etc. directly after taking the mutex), violating the update sequence.

This dead code should be removed along with the `CyranoHandler::attach(*MyOpp2Handler)` line in `main.cpp`. If the string-notification path is ever needed again (e.g., for MQTT-routed Cyrano messages), it should be re-routed through `updateFromCyranoMessage`.

---

### Issue 7 — Protocol auto-detect initializes to OPP2-only, blocking Cyrano [BEHAVIOUR]

`Opp2Handler` constructor sets `m_ActiveInputProtocol = InputProtocol::OPP2` and `m_AutoDetectProtocol = true`. The auto-detect logic in `updateFencersExternal()` (lines 1705-1710) only switches TO OPP2, never to Cyrano. So if a Cyrano DISP arrives first (the common case with legacy software), the external update guards reject it.

The DISP path through `updateFromCyranoMessage` bypasses the external guards entirely (it writes directly into `m_State` without checking `m_ActiveInputProtocol`), which is actually what makes Cyrano work at all right now — but only by circumventing the guard system.

CLAUDE.md acknowledges this: *"Currently, auto-detect logic is not yet implemented — both protocols are accepted. Do not implement auto-detect unless explicitly asked to."* The current behaviour is acceptable as a known incomplete item. However, note that the external guard methods and the zero-copy DISP path are two separate, non-integrated update paths.

---

### Issue 8 — `update()` from FSM calls `PushCachedStatusToCyrano()` unconditionally [MINOR]

At the end of `update(FencingStateMachine*, uint32_t)` (line 980) there is an unconditional `PushCachedStatusToCyrano()` regardless of whether any state actually changed. This triggers a full `getStateCopy()` + OPP2-to-Cyrano conversion + 6-string rebuild on every FSM event. For high-frequency events this is wasteful. Once the internal update methods are made self-contained (Issue 4), this trailing call becomes redundant and should be removed.

---

## Assessment of the Architecture Document

`docs/ARCHITECTURE.md` is accurate, well-structured, and correctly captures the push-based cache architecture, the stack safety constraints, and the lessons learned. Two areas do not yet match the implementation:

1. **Section 6, Scenario 3 (DISP)** correctly states state should not change, but the current implementation violates this (Issue 2 above). The document is right; the code is wrong.

2. **Section 5.1 State Update Flow** shows `PushCachedStatusToCyrano()` and `notify(EVENT_SCORE_LEFT)` as part of `updateScoreInternal`, but the actual implementation does neither (Issue 4 above). The document describes the intended design correctly; the implementation doesn't match it.

3. **Section 3.1 observer diagram** shows `Opp2Handler` still observing `CyranoHandler`. Given that the string-notification path is dead code (Issue 6), this observer relationship should be removed from both the code and the diagram.

---

### Issue 9 — OPP2 is not yet a complete superset of Cyrano for team competitions [KNOWN GAP]

`OPP2::SystemState` covers all Cyrano and FPA422 fields for individual competitions. Two fields present in Cyrano are not represented in the OPP2 types:

**Gap A — Medical intervention count.** Cyrano fields R10/L10 carry a cumulative count of medical interventions per fencer (0–9). `OPP2::Medical` models one active timeout with a running timer (`active`, `duration_ms`, `remaining_ms`, `side`) but has no per-fencer cumulative count. FIE rules cap medical timeouts per fencer, so this count matters for rule enforcement.

Fix: add `uint8_t medical_count` per side to `OPP2::Medical` (or to `OPP2::SystemState` directly), and add a corresponding field to the OPP2 spec.

**Gap B — Reserve fencer flag.** Cyrano R11/L11 carry a persistent N/R flag in every INFO message indicating whether the reserve fencer for the current round has been introduced. `OPP2::Control` has a one-shot `RESERVE` command (apparatus→software), but `OPP2::SystemState` has no field recording "reserve fencer is currently active for side X." After the command is sent, the information is absent from state.

Fix: add `bool reserve_active` to `OPP2::FencerSide`, and add a corresponding field to the OPP2 spec.

Both gaps are team competition features. Individual competition support is unaffected. Neither requires architectural change — they are additive spec extensions to `opp2_types.h`. Do not implement until team competition support is explicitly in scope.

---

## Recommended Remediation Order

Items 1 and 2 must be fixed before any further development; the others are important but not blocking.

1. **Fix the compilation error** — add `OPP2::ApparatusState &outApparatusState` parameter to the `updateFromCyranoMessage()` implementation and populate it before returning (Issue 1).

2. **Fix DISP apparatus-state violation** — remove the State-field parsing block from `updateFromCyranoMessage()`. Apparatus state must not change on DISP (Issue 2).

3. **Add `EVENT_STATE_CHANGED` constant** — replace all `StateChanged(0)` / `notify(0)` calls with a properly-named constant that does not alias EVENT_LIGHTS (Issue 3).

4. **Complete the mandatory update sequence** — add `PushCachedStatusToCyrano()` and `notify(EVENT_*)` calls to all internal update methods that are missing them. Remove the unconditional trailing `PushCachedStatusToCyrano()` from the FSM event handler once each method is self-contained (Issue 4).

5. **Remove FPA422Handler from FSM observer list** — once Opp2Handler properly notifies observers, the `MyStatemachine->attach(*MyFPA422Handler)` in main.cpp is redundant and should be removed (Issue 4 follow-on).

6. **Delete dead code** — remove `Opp2Handler::update(CyranoHandler*, const std::string&)` and the corresponding `CyranoHandler::attach(*MyOpp2Handler)` in main.cpp (Issue 6).

7. **Address publish-method race condition** — snapshot relevant state fields to local variables under the mutex before publishing, or document clearly why the current Core 0-only execution makes this safe (Issue 5).

---

## What Does Not Need to Change

- The SSOT pattern with `m_State` in Opp2Handler and mutex protection
- The 6-string push-based cache in CyranoHandler for stack safety
- Zero-copy DISP forwarding using const reference + output parameter
- `ProcessUIEvents` in Opp2Handler routing buttons to canonical state, then notifying CyranoHandler
- The `EVENT_CYRANO_SEND_INFO/NEXT/PREV` event-driven send architecture
- Internal vs. external update method split with guards
- Not implementing auto-detect protocol selection until explicitly asked
