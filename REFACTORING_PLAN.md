# OPP2 Canonical State Refactoring - Implementation Plan

Branch: `opp2-canonical-state`

## Objective

Refactor to use OPP2::SystemState as single source of truth, eliminating duplicate state storage and simplifying bidirectional protocol handling.

## Current Problems

1. **State duplication**: FencingStateMachine, CyranoHandler::m_MachineStatus, Opp2Handler::m_State
2. **Update complexity**: Multiple conversions, scattered logic
3. **Inconsistency risk**: Different protocols can disagree
4. **Memory waste**: Same data stored 3+ times

## Target Architecture

```
Internal Events (FSM/Sensor) → Opp2Handler (CANONICAL STATE) ← External Events (Protocols)
                                         ↓
                              Publish to ALL protocols
                              (OPP2, Cyrano, FPA, Remote)
```

## Implementation Phases

### Phase 1: Add Thread Safety & Protocol Selection ✅ READY TO START

**Files to modify:**
- `src/Opp2Handler.h`
- `src/Opp2Handler.cpp`

**Changes:**
1. Add mutex for m_State protection
2. Add InputProtocol enum and selection logic
3. Add `updateXxxInternal()` methods (no guards, for FSM)
4. Add `updateXxxExternal()` methods (with guards, for protocols)
5. Keep existing publish methods
6. Add equality helpers to detect changes

**New code:**
```cpp
enum class InputProtocol { OPP2, CYRANO };

class Opp2Handler {
private:
    OPP2::SystemState m_State;
    SemaphoreHandle_t m_StateMutex;
    InputProtocol m_ActiveInputProtocol = InputProtocol::OPP2;
    
public:
    void Begin(); // Initialize mutex
    
    // Internal updates (from FSM/sensor - no guards)
    void updateLightsInternal(const OPP2::Lights& lights);
    void updateScoreInternal(uint8_t left, uint8_t right);
    void updateClockInternal(uint32_t time_ms, bool running);
    void updateApparatusStateInternal(OPP2::ApparatusState state);
    
    // External updates (from protocols - with guards)
    void updateFencersExternal(const OPP2::Fencers& fencers, InputProtocol src);
    void updateMatchExternal(const OPP2::Match& match, InputProtocol src);
    void updateClockExternal(const OPP2::Clock& clock, InputProtocol src);
    
    // Thread-safe state access
    OPP2::SystemState getStateCopy() const;
    
private:
    bool lightsEqual(const OPP2::Lights& a, const OPP2::Lights& b);
    bool fencersEqual(const OPP2::Fencers& a, const OPP2::Fencers& b);
    // etc.
};
```

**Testing:**
- Verify mutex creation
- Test protocol selection
- Verify guards work (reject updates when not WAITING)
- Test change detection (no redundant publishes)

**Estimated effort**: 2-3 hours

---

### Phase 2: Route FSM Updates Through Opp2Handler

**Files to modify:**
- `src/FencingStateMachine.cpp`
- `src/FencingStateMachine.h`

**Changes:**
1. Replace direct state updates with `Opp2Handler::updateXxxInternal()` calls
2. Keep existing event notifications (observers still work)
3. Example:
   ```cpp
   // OLD:
   m_leftScore++;
   notify(EVENT_SCORE_CHANGED);
   
   // NEW:
   Opp2Handler::getInstance().updateScoreInternal(m_leftScore, m_rightScore);
   // Opp2Handler notifies observers internally
   ```

**Testing:**
- Verify lights still work during hits
- Verify score changes propagate
- Verify clock updates work
- Check for mutex contention under rapid hits

**Estimated effort**: 3-4 hours

---

### Phase 3: Add OPP2-to-Cyrano Converter

**Files to modify:**
- `src/CyranoHandler.h`
- `src/CyranoHandler.cpp`

**Changes:**
1. Add `convertOPP2ToCyrano()` method
2. Update `SendInfoMessage()` to read from OPP2 state
3. Keep `m_MachineStatus` temporarily (for comparison/testing)
4. Add DEBUG logs comparing old vs new approach

**New code:**
```cpp
class CyranoHandler {
private:
    EFP1Message m_MachineStatus;  // TEMPORARY - will be removed in Phase 4
    
    EFP1Message convertOPP2ToCyrano(const OPP2::SystemState& state);
    
public:
    void SendInfoMessage() {
        const auto& opp2State = Opp2Handler::getInstance().getStateCopy();
        EFP1Message msg = convertOPP2ToCyrano(opp2State);
        msg[Command] = "INFO";
        
        // Publish...
    }
};
```

**Testing:**
- Verify Cyrano messages still correct
- Compare old vs new Cyrano output
- Test periodic updates
- Verify no data loss in conversion

**Estimated effort**: 2-3 hours

---

### Phase 4: Route Cyrano Input Through Opp2Handler

**Files to modify:**
- `src/CyranoHandler.cpp`

**Changes:**
1. Update `ProcessMessageFromSoftware()` to call `Opp2Handler::updateXxxExternal()`
2. Convert incoming Cyrano messages to OPP2 format
3. Let guards handle filtering

**New code:**
```cpp
void CyranoHandler::ProcessMessageFromSoftware(const EFP1Message &input, bool verify) {
    auto& opp2 = Opp2Handler::getInstance();
    
    switch (input.GetType()) {
        case DISP: {
            // Convert Cyrano → OPP2 format
            OPP2::Fencers fencers = opp2.getStateCopy().fencers;  // Start with current
            
            if (input[LeftFencerName] != "") {
                strncpy(fencers.left.fencer.name, input[LeftFencerName].c_str(), 63);
                fencers.left.fencer.present = true;
            }
            // ... etc
            
            // Route through Opp2Handler with guards
            opp2.updateFencersExternal(fencers, InputProtocol::CYRANO);
            break;
        }
    }
}
```

**Testing:**
- Send Cyrano DISP, verify state updates
- Verify guards work (reject when clock running)
- Test protocol auto-detection
- Test both OPP2 and Cyrano inputs

**Estimated effort**: 2-3 hours

---

### Phase 5: Update OPP2 Dispatcher Callbacks

**Files to modify:**
- `src/Opp2Handler.cpp` (Begin() method)

**Changes:**
1. Route dispatcher callbacks through `updateXxxExternal()` methods
2. Already partially done - just need to add remaining message types

**Testing:**
- Send OPP2 fencers message, verify update
- Send OPP2 match message, verify update
- Verify guards reject invalid updates
- Test with retained MQTT messages

**Estimated effort**: 1-2 hours

---

### Phase 6: Remove Duplicate State from CyranoHandler

**Files to modify:**
- `src/CyranoHandler.h`
- `src/CyranoHandler.cpp`

**Changes:**
1. **REMOVE**: `EFP1Message m_MachineStatus`
2. All Cyrano methods use `convertOPP2ToCyrano(Opp2Handler::getInstance().getStateCopy())`
3. Clean up any remaining direct state references

**Testing:**
- Full regression test
- Verify Cyrano still works completely
- Check memory usage reduction
- Long-duration stability test

**Estimated effort**: 2-3 hours

---

### Phase 7: Final Testing & Documentation

**Activities:**
1. Stress testing (rapid hits, network floods)
2. Multi-protocol testing (OPP2 + Cyrano simultaneously)
3. Remote control testing
4. Memory leak check
5. Update documentation
6. Performance profiling

**Testing scenarios:**
- Rapid fire hits while network updates arrive
- Switch between OPP2 and Cyrano input modes
- Clock running with periodic updates
- Remote control commands during bout
- Leave running for extended period

**Estimated effort**: 3-4 hours

---

## Total Estimated Effort

**Development**: 15-22 hours
**Testing**: 3-4 hours
**Total**: ~20-26 hours (3-4 work days)

## Risk Mitigation

1. **Mutex deadlocks**: Always use timeouts, keep critical sections short
2. **State inconsistency**: Add assertions, compare old vs new in Phase 3
3. **Performance**: Profile mutex contention, optimize if needed
4. **Breaking changes**: Incremental phases allow rollback

## Success Criteria

✅ Single source of truth (OPP2::SystemState)
✅ Thread-safe multi-core operation
✅ All protocols (OPP2, Cyrano, FPA) working
✅ Protocol selection (OPP2/Cyrano input) working
✅ Guards prevent invalid state changes
✅ No memory leaks
✅ No performance degradation
✅ Reduced memory usage (removed duplicate state)

## Rollback Plan

Each phase is independently testable. If issues arise:
1. Commit after each working phase
2. Can revert to last good commit
3. Can merge partial work back to main if needed
