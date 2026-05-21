# Threading Strategy for OPP2 Canonical State Refactoring

## Core Architecture

**ESP32 Dual-Core Layout:**
- **Core 0 (PRO_CPU)**: WiFi driver, lwIP, StateMachine, all protocol handlers
- **Core 1 (APP_CPU)**: 3WeaponSensor (ESP_TIMER_TASK) - high-frequency sampling

## Thread Safety Requirements

### Shared State: `OPP2::SystemState`

The canonical state in `Opp2Handler::m_State` is accessed from **both cores**:

**Core 1 (Sensor) - WRITES**:
- Lights state (hit detection)
- Weapon detection
- High-frequency events

**Core 0 (Protocols) - READS/WRITES**:
- All protocol handlers (OPP2, Cyrano, FPA)
- Remote control
- Network updates
- Score, clock, fencers, match data

### Protection Strategy

#### Option 1: Mutex (Chosen for OPP2 State)
```cpp
class Opp2Handler {
private:
    OPP2::SystemState m_State;
    mutable SemaphoreHandle_t m_StateMutex;
    
public:
    void updateLightsInternal(const OPP2::Lights& lights) {
        if (xSemaphoreTake(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (!lightsEqual(m_State.lights, lights)) {
                m_State.lights = lights;
                m_State.lights.seq = NextSeq();
                
                xSemaphoreGive(m_StateMutex);
                PublishLights();  // Outside mutex to avoid deadlock
            } else {
                xSemaphoreGive(m_StateMutex);
            }
        }
    }
    
    const OPP2::SystemState getStateCopy() const {
        OPP2::SystemState copy;
        if (xSemaphoreTake(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            copy = m_State;
            xSemaphoreGive(m_StateMutex);
        }
        return copy;
    }
};
```

**Pros**: 
- Simple, robust
- Works for complex state updates
- FreeRTOS native

**Cons**: 
- Small overhead
- Potential for priority inversion (mitigated by short critical sections)

#### Critical Section Rules

1. **Keep mutex-protected sections SHORT**
   - Copy data in, update, release
   - Never call publish functions while holding mutex

2. **Order of operations**:
   ```cpp
   LOCK
   → Check if changed
   → Update state
   → UNLOCK
   → Publish to protocols (outside mutex)
   ```

3. **Timeout on acquire**: Always use timeout, log if acquisition fails

4. **No nesting**: Avoid taking multiple mutexes

## Core Communication Patterns

### Pattern 1: Sensor → Protocols (High Frequency)
```
3WeaponSensor (Core 1)
    ↓ detects hit
    ↓
Opp2Handler::updateLightsInternal() [mutex-protected]
    ↓ state changed
    ↓
Opp2Handler::PublishLights() [outside mutex]
    ↓ publishes
    ↓
OPP2 MQTT + notify observers
    ↓
CyranoHandler/FPAHandler (Core 0) read state and publish
```

### Pattern 2: Network → State (Low Frequency)
```
MQTT message arrives (Core 0)
    ↓
OPP2 Dispatcher parses
    ↓
Opp2Handler::updateFencersExternal() [mutex-protected]
    ↓ guards check
    ↓ state updated
    ↓
Publish to all protocols (outside mutex)
```

### Pattern 3: Timer Updates (Medium Frequency)
```
FencingTimer tick (Core 0)
    ↓
Opp2Handler::updateClockInternal() [mutex-protected]
    ↓ throttled updates (~1Hz)
    ↓
Publish to protocols
```

## Performance Considerations

**Mutex overhead**: ~1-5 microseconds per lock/unlock on ESP32
**Sensor frequency**: ~6.6 kHz (150 µs per scan)
**Update frequency**: Lights change <100 Hz typically

**Worst case**: Sensor tries to update lights while Core 0 holds mutex
- Core 1 blocks for <10ms timeout
- If timeout, sensor logs and continues (non-critical)

## Debugging

**Watchdog**: All tasks registered with ESP Task WDT
- Core 0: 10ms FSM tick resets watchdog
- Core 1: Sensor loop resets watchdog

**Deadlock prevention**:
- Always use timeouts
- Never hold mutex while calling network functions
- Log mutex acquisition failures

**Verification**:
- Monitor `uxTaskGetStackHighWaterMark()` for stack usage
- Log mutex contention at DEBUG level
- Use ESP-IDF trace facilities if issues arise

## Migration Notes

1. **Phase 1**: Add mutex to Opp2Handler, protect all state access
2. **Phase 2**: Route FencingStateMachine updates through protected methods
3. **Phase 3**: Remove duplicate state from CyranoHandler
4. **Phase 4**: Test under load (rapid hits, network updates, etc.)

## Testing Checklist

- [ ] Rapid fire hits (stress test sensor→state path)
- [ ] Simultaneous network updates (OPP2 + Cyrano messages)
- [ ] Remote commands during active bout
- [ ] Clock running while updates arrive
- [ ] Long-duration tests (check for deadlocks)
- [ ] Monitor mutex acquisition failures
- [ ] Verify no priority inversion issues
