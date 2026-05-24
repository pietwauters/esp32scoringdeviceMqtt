# Migration Plan — opp2-canonical-state branch

**Goal:** Close all correctness gaps identified in `ASSESSMENT.md` so the refactored
architecture is fully correct and the branch is ready for new feature work.

**Reference:** `docs/ASSESSMENT.md` for full analysis of each issue.

---

## Status legend
`[ ]` not started · `[~]` in progress · `[x]` done

---

## Phase 1 — Correctness blockers
*Must be complete before anything else. Phase 1 items are independent of each other.*

| # | Item | Issue | Status |
|---|------|-------|--------|
| 1.1 | Fix `updateFromCyranoMessage` signature: add `OPP2::ApparatusState& outApparatusState` parameter to the implementation in `Opp2Handler.cpp`; populate the output parameter before returning | Issue 1 | `[x]` |
| 1.2 | Remove State-field parsing block from `updateFromCyranoMessage`: the `if (EFP1Input[State] != "")` block that writes to `m_State.apparatus_state.state`. DISP must not change bout state | Issue 2 | `[x]` |
| 1.3 | Add `EVENT_STATE_CHANGED` constant (value `0x40000000`) to `EventDefinitions.h`; replace every `StateChanged(0)` / `notify(0)` call in `Opp2Handler` with `StateChanged(EVENT_STATE_CHANGED)` | Issue 3 | `[x]` |

---

## Phase 2 — Mandatory update sequence
*Complete the three-step post-update sequence (publish → push cache → notify) in every
internal update method. Items 2.1–2.6 are independent of each other; 2.7 and 2.8
depend on all of 2.1–2.6 being done.*

| # | Item | Issue | Status |
|---|------|-------|--------|
| 2.1 | `updateLightsInternal`: add `PushCachedStatusToCyrano()` | Issue 4 | `[x]` |
| 2.2 | `updateScoreInternal`: add `PushCachedStatusToCyrano()` + `notify(EVENT_CYRANO_SEND_INFO)` | Issue 4 | `[x]` |
| 2.3 | `updateClockInternal`: add `PushCachedStatusToCyrano()` + `notify(EVENT_CYRANO_SEND_INFO)` | Issue 4 | `[x]` |
| 2.4 | `updateApparatusStateInternal`: add `PushCachedStatusToCyrano()` + `notify(kCyranoStateEvent[state])` + `notify(EVENT_CYRANO_SEND_INFO)` | Issue 4 | `[x]` |
| 2.5 | `updateMatchInternal`: add `PushCachedStatusToCyrano()` + `notify(EVENT_CYRANO_SEND_INFO)` | Issue 4 | `[x]` |
| 2.6 | `updateUW2FInternal`: add `PushCachedStatusToCyrano()` + `notify(EVENT_CYRANO_SEND_INFO)` | Issue 4 | `[x]` |
| 2.7 | Remove unconditional trailing `PushCachedStatusToCyrano()` at end of `update(FencingStateMachine*, uint32_t)` | Issue 8 | `[x]` |
| 2.8 | ~~Remove `MyStatemachine->attach(*MyFPA422Handler)`~~ — **KEPT**: FPA422Handler needs raw FSM events (score/time/cards/lights) with data embedded in the event; machine status comes from Opp2Handler separately | Issue 4 follow-on | `[x]` |

---

## Phase 3 — Observer Rewiring (architectural cleanup)

| # | Item | Status |
|---|------|--------|
| 3.1 | Remove `MyStatemachine->attach(*MyCyranoHandler)` — CyranoHandler no longer observes FSM | `[x]` |
| 3.2 | Remove `MyCyranoHandler->attach(*MyOpp2Handler)` — Opp2Handler no longer observes CyranoHandler | `[x]` |
| 3.3 | Remove `Opp2Handler::update(CyranoHandler*, uint32_t)` | `[x]` |
| 3.4 | Remove `Opp2Handler::update(CyranoHandler*, const std::string&)` (~240 lines dead code) | `[x]` |
| 3.5 | Remove `CyranoHandler::update(FencingStateMachine*, uint32_t)` | `[x]` |
| 3.6 | Fix `CyranoHandler::ProcessMessageFromSoftware`: HELLO no longer resets bout state; DISP fires only LOCKED; ACK calls `Opp2Handler::ProcessCyranoACK()` directly | `[x]` |
| 3.7 | Add `Opp2Handler::ProcessCyranoACK()` — direct method for Cyrano ACK path | `[x]` |
| 3.8 | Add `notify(EVENT_CYRANO_SEND_INFO)` to `updateLightsInternal` | `[x]` |
| 3.9 | Fix publish-method mutex gap (document or snapshot under mutex) | `[ ]` |

---

## Deferred — Do not start

| Item | Reason |
|------|--------|
| Protocol auto-detect (Issue 7) | Not in scope until explicitly requested |
| Medical intervention count in OPP2 (Issue 9A) | Team competition only; additive spec change |
| Reserve fencer flag in OPP2 (Issue 9B) | Team competition only; additive spec change |

---

## Completion criteria for the branch

The branch is ready for new feature work when:
- All Phase 1 and Phase 2 items are `[x]`
- The code compiles without errors
- Phase 3 items are `[x]` or explicitly deferred with documented rationale
