# ESP32 Fencing Scoring Device - Complete Architecture Documentation

**Date:** May 22, 2026  
**Document Purpose:** Comprehensive architecture reference capturing state management, communication patterns, stack safety lessons, and implementation roadmap.

---

## Table of Contents

1. [System Overview](#system-overview)
2. [State Ownership & Threading Model](#state-ownership--threading-model)
3. [Communication Patterns](#communication-patterns)
4. [Critical Stack Safety Constraints](#critical-stack-safety-constraints)
5. [Protocol Handlers Architecture](#protocol-handlers-architecture)
6. [Complete Data Flow Scenarios](#complete-data-flow-scenarios)
7. [Problems Encountered & Solutions](#problems-encountered--solutions)
8. [Open Items & Decisions](#open-items--decisions)
9. [Implementation Roadmap](#implementation-roadmap)

---

## 1. System Overview

### 1.1 Platform Architecture

```
ESP32 Dual-Core System
├── Core 0 (PRO_CPU) - Protocol & Network Processing
│   ├── WiFi/lwIP stack
│   ├── MQTT Client (AtlasAsyncMqttClient singleton)
│   ├── async_udp task (~4KB stack) ← CRITICAL CONSTRAINT
│   ├── FencingStateMachine (10ms tick)
│   ├── Protocol Handlers:
│   │   ├── Opp2Handler (Canonical State Owner)
│   │   ├── CyranoHandler (Cyrano/EFP1.1 protocol)
│   │   └── FPA422Handler (RS422 serial protocol)
│   └── UDPIOHandler (Remote control buttons)
│
└── Core 1 (APP_CPU) - Real-Time Weapon Sensing
    └── 3WeaponSensor (150µs scan, ~6.6kHz ADC sampling)
```

**Key Constraint:** async_udp task has only ~4KB stack. UDP packet callbacks execute in this context and CANNOT safely allocate large stack objects (>100 bytes).

### 1.2 Protocol Support

| Protocol | Purpose | Transport | State Owner? |
|----------|---------|-----------|--------------|
| **OPP2** | Modern competition management (OpenPiste) | MQTT | YES ✓ |
| **Cyrano/EFP1.1** | Legacy competition management | UDP + MQTT | No (reads from OPP2) |
| **FPA-422** | Hardware displays/scoreboards | RS422 Serial | No (reads from OPP2) |

---

## 2. State Ownership & Threading Model

### 2.1 Canonical State (Single Source of Truth)

**Owner:** `Opp2Handler`  
**Structure:** `OPP2::SystemState m_State` (~400-600 bytes)  
**Protection:** `SemaphoreHandle_t m_StateMutex` (FreeRTOS mutex)

```cpp
class Opp2Handler {
private:
  OPP2::SystemState m_State;      // ← CANONICAL STATE (SSOT)
  SemaphoreHandle_t m_StateMutex; // Thread-safe dual-core access
  
public:
  // Thread-safe read (makes ~400-600 byte stack copy)
  OPP2::SystemState getStateCopy();
  
  // Lightweight reads (no mutex, single fields)
  void getPisteId(char* out);
  // ... other lightweight accessors
};
```

**State Contents:**
```cpp
struct SystemState {
  char piste_id[32];              // Piste identifier
  ApparatusStateMsg apparatus_state; // W/H/F/P/E state
  Connection connection;          // Online/offline status
  Lights lights;                  // Hit lights (red/green/white)
  Clock clock;                    // Timer (running/stopped, time_ms)
  Score score;                    // Scores, cards, priority
  Fencers fencers;               // Names, nationalities, clubs
  Match match;                    // Weapon, type, phase, round
  UW2F uw2f;                     // UW2F timer state
  // ... plus medical, video_review, control
};
```

### 2.2 State Update Patterns

#### Pattern A: Internal Updates (FROM FSM or Local Logic)
```cpp
void Opp2Handler::updateLightsInternal(const OPP2::Lights& lights) {
  xSemaphoreTake(m_StateMutex, pdMS_TO_TICKS(10));
  m_State.lights = lights;
  xSemaphoreGive(m_StateMutex);
  
  PublishLights();           // Send to MQTT
  PushCachedStatusToCyrano(); // Update protocol handler caches
  notify(EVENT_LIGHTS);      // Notify observers
}
```

**Used by:**
- FencingStateMachine → scores, cards, timer, lights
- Local button handlers → apparatus state changes

**Characteristics:**
- Direct mutex-protected write
- Pushes updates to protocol handlers
- Always accepted (no conflict checking)

#### Pattern B: External Updates (FROM Protocols)
```cpp
void Opp2Handler::updateApparatusStateExternal(
    const OPP2::ApparatusStateMsg& msg, 
    InputProtocol source) {
  
  // 1. Check input protocol priority
  if (!isProtocolAllowed(source)) {
    ESP_LOGW("Update rejected - wrong protocol");
    return; // Guard rejected
  }
  
  // 2. Update canonical state
  xSemaphoreTake(m_StateMutex, pdMS_TO_TICKS(10));
  m_State.apparatus_state = msg;
  xSemaphoreGive(m_StateMutex);
  
  // 3. Propagate to other systems
  PublishApparatusState();    // MQTT
  PushCachedStatusToCyrano(); // Cyrano cache
  notify(EVENT_STATE_CHANGED); // Observers
}
```

**Used by:**
- DISP messages from Cyrano
- Control messages from OPP2 MQTT
- (Future) OPP2 Fencers/Match/Score updates

**Characteristics:**
- Protocol priority checking (conflict resolution)
- Mutex-protected write
- Can be rejected by guards
- Auto-detect mode tracks which protocol is active

### 2.3 Three Types of "State" (Clarified)

**CRITICAL:** The word "state" has three different meanings in this codebase:

#### Type 1: FSM Internal State (FencingStateMachine)
- **Scope:** Internal to apparatus hardware
- **Purpose:** Controls lights, timers, weapon detection logic
- **Examples:** Waiting for hit, ignoring lockout, debouncing
- **Independence:** Works without any software connection
- **Not visible to:** Competition management software

#### Type 2: Apparatus State (Protocol-Level FSM State)
- **Scope:** Visible to competition management software
- **Purpose:** Match flow control (W=WAITING, H=HALT, F=FENCING, P=PAUSE, E=ENDING)
- **Owner:** OPP2::SystemState.apparatus_state
- **Controls:** When referee can start/stop timer, when match is active
- **Transitions:** 
  - Software DISP → stays WAITING (does NOT change state)
  - BEGIN button → WAITING → HALT
  - START button → HALT → FENCING
  - HALT button → FENCING → HALT
  - END button → any → ENDING
  - Software ACK → ENDING → WAITING

#### Type 3: Complete System State (Overall State)
- **Scope:** Everything the software needs to know
- **Structure:** `OPP2::SystemState` (entire struct)
- **Includes:**
  - Apparatus state (W/H/F/P/E)
  - Fencer info (names, nationalities, clubs)
  - Match metadata (weapon, type, phase, bout ID)
  - Scores, cards, priority
  - Timer state
  - Lights
  - Connection status

---

## 3. Communication Patterns

### 3.1 Observer Pattern (Loose Coupling)

**Used For:** Event notification across loosely-coupled components

```cpp
// Subject (event source)
class Opp2Handler : public Subject<Opp2Handler> {
  void someStateChange() {
    // ... update state ...
    notify(EVENT_SOMETHING_CHANGED); // Fire event
  }
};

// Observer (event consumer)
class CyranoHandler : public Observer<Opp2Handler> {
  void update(Opp2Handler* subject, uint32_t eventtype) {
    switch (eventtype) {
    case EVENT_CYRANO_SEND_INFO:
      SendInfoMessage(); // React to event
      break;
    }
  }
};

// Wiring (in main.cpp)
MyOpp2Handler->attach(*MyCyranoHandler);
```

**Current Observer Relationships:**
```
FencingStateMachine (Subject)
  ├─► Opp2Handler (scores, cards, lights, timer)
  ├─► CyranoHandler (protocol state sync)
  └─► FPA422Handler (display updates)

UDPIOHandler (Subject)
  └─► Opp2Handler (button presses)

Opp2Handler (Subject)
  ├─► CyranoHandler (message send requests)
  └─► FPA422Handler (state changed notifications)
```

**When to Use:**
- One-to-many notifications
- Loose coupling desired
- Event-driven architectures
- No return value needed

### 3.2 Direct Method Calls (Tight Coupling)

**Used For:** Direct state queries and updates where tight coupling is acceptable

```cpp
// Lightweight read (no mutex, small data)
char pisteId[OPP2::PISTE_ID_MAX];
Opp2Handler::getInstance().getPisteId(pisteId);

// Push-based cache update (called by state owner)
CyranoHandler::getInstance().updateCachedStatus(status);
```

**When to Use:**
- Performance-critical paths
- Small data transfers
- Push-based architectures
- Clear ownership hierarchy

### 3.3 Cache-Push Architecture (Stack Safety Pattern)

**Problem:** UDP callbacks in async_udp task (~4KB stack) cannot safely call `getStateCopy()` which allocates ~400-600 bytes on stack.

**Solution:** Opp2Handler **pushes** pre-built data to protocol handlers when state changes.

```cpp
// Opp2Handler: Push cache to CyranoHandler
void Opp2Handler::PushCachedStatusToCyrano() {
  EFP1Message status = convertOpp2ToCyrano(m_State); // Build once
  CyranoHandler::getInstance().updateCachedStatus(status);
}

// CyranoHandler: Receive push, build strings
void CyranoHandler::updateCachedStatus(const EFP1Message& status) {
  m_CachedStatus = status;
  RebuildCachedStrings(); // Build all 6 strings
}

void CyranoHandler::RebuildCachedStrings() {
  // Build INFO message strings
  m_CachedStatus[Command] = "INFO";
  m_CachedStatus[CompetitionId] = m_CompetitionId;
  m_CachedStatus.ToString(m_CachedCyranoString);
  m_CachedJsonString = convert_cyrano_to_json_string(m_CachedCyranoString);
  
  // Build NEXT message strings
  m_CachedNextCyrano = m_CachedStatus.MakeNextMessageString();
  m_CachedNextJson = convert_cyrano_to_json_string(m_CachedNextCyrano);
  
  // Build PREV message strings
  m_CachedPrevCyrano = m_CachedStatus.MakePrevMessageString();
  m_CachedPrevJson = convert_cyrano_to_json_string(m_CachedPrevCyrano);
  
  m_CachedStatusValid = true;
}

// UDP callback: Use cached strings - ZERO stack allocations
void CyranoHandler::SendInfoMessage() {
  if (!m_CachedStatusValid) return;
  
  // Direct pointer access - no copies, no stack objects
  const char* pCyranoMsg = m_CachedCyranoString.c_str();
  size_t cyranoLen = m_CachedCyranoString.length();
  
  CyranoHandlerudpRcv.writeTo((uint8_t*)pCyranoMsg, cyranoLen, ...);
}
```

**Key Points:**
- State owner (Opp2Handler) pushes updates
- Protocol handler (CyranoHandler) caches final strings
- UDP callbacks use cached strings only
- No mutex calls in UDP callback context
- No stack allocations in UDP callback context

---

## 4. Critical Stack Safety Constraints

### 4.1 The async_udp Task Limitation

**Context:** ESP-IDF's AsyncUDP library processes incoming UDP packets in a dedicated `async_udp` task with limited stack (~4KB typical).

**Constraint:** UDP packet callbacks execute in this task context.

**What You CANNOT Do:**
```cpp
// ❌ DANGEROUS - Stack overflow in async_udp task!
void ProcessCyranoPacket(AsyncUDPPacket packet) {
  OPP2::SystemState state = Opp2Handler::getInstance().getStateCopy(); // ~600 bytes!
  std::string msg = BuildMessage(state); // More allocations!
  std::string json = ConvertToJson(msg); // Even more!
  // Total: ~1500+ bytes → STACK OVERFLOW → DEVICE REBOOT
}
```

**What You MUST Do:**
```cpp
// ✓ SAFE - Zero stack allocations
void ProcessCyranoPacket(AsyncUDPPacket packet) {
  // Use cached strings built ahead of time
  CyranoHandler::getInstance().ProcessMessageFromSoftware(
    EFP1Message((char*)packet.data()));
}

void CyranoHandler::SendInfoMessage() {
  // Use pre-built cached strings - ZERO stack allocations
  const char* pMsg = m_CachedCyranoString.c_str(); // Just a pointer!
  size_t len = m_CachedCyranoString.length();
  CyranoHandlerudpRcv.writeTo((uint8_t*)pMsg, len, ...);
}
```

### 4.2 Stack Allocation Rules

**Safe Contexts (Large Stack Available):**
- Main loop (Core 0)
- FreeRTOS tasks with explicit large stack allocation
- Observer update() methods (called from Core 0 contexts)
- Direct method calls from main.cpp

**Unsafe Contexts (Limited Stack):**
- async_udp task callbacks (~4KB)
- ISR (Interrupt Service Routine) handlers
- Timer callbacks (if not explicitly given large stack)

**Safe Operations in UDP Callbacks:**
```cpp
✓ Pointer access (const char* p = str.c_str())
✓ Primitive types (int, bool, char, etc.)
✓ Small structs (<50 bytes)
✓ Pass-by-reference to existing objects
✓ Calling methods that use cached data
```

**NEVER Do in UDP Callbacks:**
```cpp
❌ getStateCopy() - 400-600 byte stack allocation
❌ std::string concatenation/building
❌ JSON serialization/deserialization
❌ Large struct copies
❌ std::vector/std::map operations
❌ String format operations (sprintf with large buffers)
```

### 4.3 Zero-Copy Pattern for DISP Messages

**Problem:** DISP messages arrive via UDP and need to update canonical state.

**Wrong Approach (Stack Overflow):**
```cpp
case DISP:
  // ❌ Creates 600-byte stack copy in UDP callback!
  OPP2::SystemState state = Opp2Handler::getInstance().getStateCopy();
  // ... parse DISP fields ...
  // ... update state ...
```

**Correct Approach (Zero-Copy):**
```cpp
case DISP:
  // ✓ Pass const reference - NO COPY
  OPP2::ApparatusState apparatusState;
  Opp2Handler::getInstance().updateFromCyranoMessage(input, apparatusState);
  // Uses output parameter for lightweight state return
```

**Implementation:**
```cpp
bool Opp2Handler::updateFromCyranoMessage(
    const EFP1Message& msg,           // ← const reference (no copy)
    OPP2::ApparatusState& outApparatusState) { // ← output param (no copy)
  
  // Parse fields directly from const reference
  const std::string& leftName = msg[LeftName];
  
  // Update canonical state with mutex
  xSemaphoreTake(m_StateMutex, pdMS_TO_TICKS(10));
  strncpy(m_State.fencers.left.name, leftName.c_str(), ...);
  // ... update all fields ...
  outApparatusState = m_State.apparatus_state.state; // Return lightweight value
  xSemaphoreGive(m_StateMutex);
  
  return true;
}
```

---

## 5. Protocol Handlers Architecture

### 5.1 Opp2Handler (Canonical State Owner)

**Role:** Single source of truth for all piste state

**Responsibilities:**
1. **State Storage:** Owns `OPP2::SystemState m_State`
2. **Thread Safety:** Protects state with `m_StateMutex`
3. **State Updates:** Processes updates from FSM and protocols
4. **Protocol Priority:** Enforces input protocol conflict resolution
5. **MQTT Publishing:** Publishes OPP2 messages to competition software
6. **Cache Management:** Pushes state updates to protocol handler caches
7. **Button Handling:** Processes remote control button presses
8. **Event Routing:** Notifies observers of state changes

**Key Methods:**
```cpp
// Internal updates (from FSM) - always accepted
void updateLightsInternal(const OPP2::Lights& lights);
void updateScoreInternal(const OPP2::Score& score);
void updateClockInternal(const OPP2::Clock& clock);
void updateApparatusStateInternal(const OPP2::ApparatusStateMsg& msg);

// External updates (from protocols) - conflict checking
void updateApparatusStateExternal(const OPP2::ApparatusStateMsg& msg, InputProtocol source);
void updateFencersExternal(const OPP2::Fencers& fencers, InputProtocol source);
void updateMatchExternal(const OPP2::Match& match, InputProtocol source);

// Zero-copy update for UDP callback context
bool updateFromCyranoMessage(const EFP1Message& msg, OPP2::ApparatusState& outApparatusState);

// Thread-safe reads
OPP2::SystemState getStateCopy(); // Heavy - 600 bytes on stack!
void getPisteId(char* out);       // Lightweight - safe in callbacks

// Cache push
void PushCachedStatusToCyrano();

// Button handling
void ProcessUIEvents(uint32_t event);
```

**State Update Flow:**
```
FencingStateMachine
    ↓ (EVENT_SCORE_LEFT)
Opp2Handler::update(FSM*, event)
    ↓
updateScoreInternal(score)
    ↓
├─► m_State.score = score (with mutex)
├─► PublishScore() → MQTT
├─► PushCachedStatusToCyrano()
└─► notify(EVENT_SCORE_LEFT) → Observers
```

### 5.2 CyranoHandler (Cyrano/EFP1.1 Protocol)

**Role:** Cyrano protocol communication (legacy competition management)

**Responsibilities:**
1. **UDP Communication:** Listen on port 50100, send to port 50101
2. **MQTT Bridge:** Publish/subscribe to Cyrano MQTT topics
3. **Cache Management:** Store 6 pre-built strings for stack safety
4. **Message Routing:** Process HELLO/DISP/ACK/NAK from software
5. **Zero-Copy Routing:** Forward DISP to Opp2Handler for parsing
6. **Event Handling:** React to Opp2Handler send events
7. **CompetitionId Storage:** Track Cyrano-specific competition ID

**Cache Architecture:**
```cpp
class CyranoHandler {
private:
  // Base cached state (pushed from Opp2Handler)
  EFP1Message m_CachedStatus;
  bool m_CachedStatusValid;
  
  // Cyrano-specific field
  std::string m_CompetitionId;
  
  // 6 pre-built strings (stack-safe for UDP callbacks)
  std::string m_CachedCyranoString; // INFO Cyrano format
  std::string m_CachedJsonString;   // INFO JSON format
  std::string m_CachedNextCyrano;   // NEXT Cyrano format
  std::string m_CachedNextJson;     // NEXT JSON format
  std::string m_CachedPrevCyrano;   // PREV Cyrano format
  std::string m_CachedPrevJson;     // PREV JSON format
};
```

**Message Send Flow (Stack-Safe):**
```
Button Press (UI_INPUT_CYRANO_NEXT)
    ↓
UDPIOHandler::notify(UI_INPUT_CYRANO_NEXT)
    ↓
Opp2Handler::ProcessUIEvents()
    ├─► PushCachedStatusToCyrano() (update cache)
    └─► notify(EVENT_CYRANO_SEND_NEXT) (fire event)
            ↓
CyranoHandler::update(Opp2Handler*, EVENT_CYRANO_SEND_NEXT)
    ↓
// Use cached strings - ZERO stack allocations
const char* pMsg = m_CachedNextCyrano.c_str(); // Just pointer!
CyranoHandlerudpRcv.writeTo((uint8_t*)pMsg, len, ...);
```

**DISP Message Flow (Zero-Copy):**
```
UDP Packet Arrives (async_udp task)
    ↓
ProcessCyranoPacket(packet) [UDP callback - LIMITED STACK]
    ↓
CyranoHandler::ProcessMessageFromSoftware(EFP1Message)
    ↓
case DISP:
  // Zero-copy forwarding
  OPP2::ApparatusState apparatusState;
  Opp2Handler::getInstance().updateFromCyranoMessage(input, apparatusState);
  // ↑ No getStateCopy() - uses const reference + output param
  
  // Sync local protocol state
  switch (apparatusState) { ... }
```

**Key Methods:**
```cpp
// UDP message processing (called from async_udp task)
void ProcessMessageFromSoftware(const EFP1Message& input, bool bVerifyPisteID = true);

// Cache management (called from Core 0 context)
void updateCachedStatus(const EFP1Message& status);
void RebuildCachedStrings();

// Message sending (uses cached strings - stack-safe)
void SendInfoMessage();

// Observer pattern event handling
void update(Opp2Handler* subject, uint32_t eventtype);
void update(FencingStateMachine* subject, uint32_t eventtype);

// UI event handling (DEPRECATED - now handled by Opp2Handler)
void ProcessUIEvents(uint32_t event); // Now empty stub
```

### 5.3 FPA422Handler (RS422 Serial Protocol)

**Role:** Drive hardware displays and scoreboards via RS422

**Responsibilities:**
1. **Serial Communication:** Send FPA protocol messages over RS422
2. **State Reading:** Read canonical state from Opp2Handler
3. **Format Conversion:** Convert OPP2 state to FPA messages
4. **Display Control:** Update match/score/timer displays

**Architecture:**
```cpp
class FPA422Handler : public Observer<Opp2Handler> {
  void update(Opp2Handler* subject, uint32_t eventtype) {
    // Read canonical state (safe - called from Core 0)
    OPP2::SystemState state = Opp2Handler::getInstance().getStateCopy();
    
    // Convert to FPA messages
    FPA422Message msg = ConvertStateToFPA(state);
    
    // Send via RS422
    SendFPAMessage(msg);
  }
};
```

**Observes:** Opp2Handler (changed from CyranoHandler in Phase 6)

---

## 6. Complete Data Flow Scenarios

### Scenario 1: Referee Presses NEXT Button

```
1. Physical Button Press
   ↓
2. UDPIOHandler detects button
   ↓
3. notify(UI_INPUT_CYRANO_NEXT)
   ↓
4. Opp2Handler::ProcessUIEvents(UI_INPUT_CYRANO_NEXT)
   ├─► PushCachedStatusToCyrano() - ensures cache current
   └─► notify(EVENT_CYRANO_SEND_NEXT)
       ↓
5. CyranoHandler::update(Opp2Handler*, EVENT_CYRANO_SEND_NEXT)
   ├─► Get cached strings (ZERO stack allocation)
   ├─► const char* pMsg = m_CachedNextCyrano.c_str();
   └─► Send UDP: |EFP1.1|NEXT|3|competitionId|%|
```

**State Changes:** NONE (NEXT does not change apparatus state)  
**MQTT Publish:** NEXT message (Cyrano format JSON)  
**UDP Broadcast:** NEXT message (Cyrano protocol string)

### Scenario 2: Referee Presses BEGIN Button

```
1. Physical Button Press
   ↓
2. UDPIOHandler detects button
   ↓
3. notify(UI_INPUT_CYRANO_BEGIN)
   ↓
4. Opp2Handler::ProcessUIEvents(UI_INPUT_CYRANO_BEGIN)
   ├─► updateApparatusStateInternal(HALT) - WAITING → HALT
   │   ├─► Take mutex
   │   ├─► m_State.apparatus_state.state = HALT
   │   ├─► Release mutex
   │   └─► PublishApparatusState() → MQTT
   ├─► PushCachedStatusToCyrano() - update with new state
   │   └─► CyranoHandler::updateCachedStatus(status)
   │       └─► RebuildCachedStrings() (6 strings)
   ├─► notify(EVENT_CYRANO_SEND_INFO)
   ├─► notify(EVENT_CYRANO_STATE_H)
   └─► notify(EVENT_CYRANO_STATE_UNLOCKED)
       ↓
5. CyranoHandler::update(Opp2Handler*, EVENT_CYRANO_SEND_INFO)
   └─► SendInfoMessage() with State=H
       └─► Send: |EFP1.1|INFO|3|...|State:H|...|%|
```

**State Changes:** WAITING → HALT  
**MQTT Publishes:**
- OPP2 apparatus_state message
- Cyrano INFO message (JSON)  
**UDP Broadcast:** INFO message with State=H  
**FSM:** Unlocked (can now start timer)

### Scenario 3: Software Sends DISP Message

```
1. UDP Packet Arrives (async_udp task - LIMITED STACK!)
   ↓
2. ProcessCyranoPacket(packet) [UDP callback]
   ├─► Verify software IP
   └─► CyranoHandler::ProcessMessageFromSoftware(EFP1Message)
       ↓
3. case DISP: (Zero-Copy Path)
   ├─► OPP2::ApparatusState apparatusState;
   ├─► Opp2Handler::getInstance().updateFromCyranoMessage(
   │       input,          // ← const reference (no copy)
   │       apparatusState) // ← output param (no copy)
   │   ↓
   │   [Inside Opp2Handler - Safe Context]
   │   ├─► Parse all fields from EFP1Message:
   │   │   - Fencers (names, nationalities, clubs)
   │   │   - Match (weapon, type, phase, round)
   │   │   - Scores
   │   │   - Cards (yellow, red, black)
   │   │   - Priority
   │   │   - Clock
   │   ├─► Take m_StateMutex
   │   ├─► Update m_State with all fields
   │   ├─► Release m_StateMutex
   │   ├─► PushCachedStatusToCyrano()
   │   └─► notify(EVENT_STATE_CHANGED)
   │           ↓
   │       FPA422Handler gets notification
   │           ↓
   │       Reads state, updates displays
   │
   └─► Sync local Cyrano protocol state
       └─► switch (apparatusState) { ... }
```

**State Changes:** NONE (stays in WAITING per Cyrano spec)  
**State Updates:** Fencers, match, scores, cards, priority, clock  
**MQTT Publishes:** OPP2 messages for changed fields  
**Stack Safety:** Zero copies in UDP callback, all parsing in Opp2Handler with mutex

### Scenario 4: Hit Detected During Match

```
1. 3WeaponSensor detects hit (Core 1, ~6.6kHz ADC)
   ↓
2. notify(EVENT_LIGHTS) → FencingStateMachine
   ↓
3. FencingStateMachine processes hit logic
   ├─► Determine valid hit (lockout, priority, etc.)
   ├─► Update internal FSM state
   └─► notify(EVENT_SCORE_LEFT, new_score)
       ↓
4. Opp2Handler::update(FencingStateMachine*, EVENT_SCORE_LEFT)
   ├─► Extract score from event data
   ├─► updateScoreInternal(score)
   │   ├─► Take m_StateMutex
   │   ├─► m_State.score = score
   │   ├─► Release m_StateMutex
   │   ├─► PublishScore() → MQTT
   │   └─► PushCachedStatusToCyrano()
   │       └─► CyranoHandler::updateCachedStatus(status)
   │           └─► RebuildCachedStrings()
   └─► notify(EVENT_SCORE_LEFT) → Observers
       ├─► CyranoHandler::update(FSM*, EVENT_SCORE_LEFT)
       │   └─► SendInfoMessage() (uses cached strings)
       └─► FPA422Handler::update(Opp2*, EVENT_SCORE_LEFT)
           └─► Update RS422 displays
```

**State Changes:** Score incremented  
**MQTT Publishes:**
- OPP2 score message
- Cyrano INFO message  
**UDP Broadcast:** INFO with updated score  
**Displays:** RS422 scoreboard updated

### Scenario 5: Timer Expiration (UW2F Timer)

```
1. UW2F Timer expires (FencingStateMachine 10ms tick)
   ↓
2. notify(EVENT_UW2F_TIMER, time_remaining)
   ↓
3. Opp2Handler::update(FencingStateMachine*, EVENT_UW2F_TIMER)
   ├─► updateUW2FInternal(uw2f_state)
   │   ├─► Take m_StateMutex
   │   ├─► m_State.uw2f = uw2f_state
   │   ├─► Release m_StateMutex
   │   └─► PublishUW2F() → MQTT
   └─► PushCachedStatusToCyrano()
       ↓
4. CyranoHandler::update(FencingStateMachine*, EVENT_UW2F_TIMER)
   └─► Build UW2F JSON and publish to MQTT
       (Cyrano protocol has no UW2F message type)
```

**State Changes:** UW2F timer value  
**MQTT Publishes:**
- OPP2 UW2F message
- Cyrano UW2F JSON (custom format)  
**Stack Safety:** Timer updates happen in FSM context (safe)

---

## 7. Problems Encountered & Solutions

### Problem 1: Stack Overflow in async_udp Task After SNTP Sync

**Symptom:** Device reboots after NTP time sync completes (10-30 seconds after boot).

**Root Cause:** Phase 6 refactoring changed protocol handlers to read canonical state on-demand via `getStateCopy()`, which allocates ~400-600 bytes on stack. UDP packet callbacks run in async_udp task with only ~4KB stack. Combined with string building operations, stack usage exceeded available space.

**Debugging Process:**
The root cause was identified through systematic analysis of:
1. Stack overflow timing (occurred after NTP sync, during HELLO message handling)
2. Call chain analysis showing getStateCopy() allocations in UDP callback context
3. String building operations compounding the stack usage

**Actual Cause Chain:**
```
SNTP sync completes
    ↓
Software sends HELLO message
    ↓
ProcessCyranoPacket() [async_udp task, ~4KB stack]
    ↓
ProcessMessageFromSoftware()
    ↓
SendInfoMessage()
    ├─► OPP2::SystemState state = getStateCopy();  // ~600 bytes!
    ├─► std::string msg = status.ToString();       // ~200 bytes
    └─► std::string json = convert_to_json(msg);   // ~300 bytes
    ↓
Total: ~1500 bytes → STACK OVERFLOW → Watchdog → REBOOT
```

**Solution:** **Push-Based Cache Architecture**

1. Opp2Handler owns canonical state
2. When state changes, Opp2Handler **pushes** update to CyranoHandler
3. CyranoHandler builds and caches 6 final strings:
   - INFO Cyrano format
   - INFO JSON format
   - NEXT Cyrano format
   - NEXT JSON format
   - PREV Cyrano format
   - PREV JSON format
4. UDP callbacks use cached strings only - zero stack allocations

**Code Changes:**
```cpp
// Before (DANGEROUS)
void SendInfoMessage() {
  OPP2::SystemState state = getStateCopy(); // ~600 bytes on stack!
  std::string msg = BuildMessage(state);    // More allocations
  SendUDP(msg.c_str(), msg.length());
}

// After (SAFE)
void SendInfoMessage() {
  if (!m_CachedStatusValid) return;
  const char* pMsg = m_CachedCyranoString.c_str(); // Just pointer!
  size_t len = m_CachedCyranoString.length();
  SendUDP(pMsg, len); // ZERO stack allocations
}
```

**Commits:** ffd9792, 3a386c7, 5618c33, 52c7d5b

**Lesson Learned:** In resource-constrained contexts (limited stack), ALWAYS cache final output instead of building on-demand. Push-based architectures are safer than pull-based.

### Problem 2: Protocol Identifier "OPP2" Instead of "EFP1.1"

**Symptom:** Competition management software not responding to NEXT/PREV/INFO messages.

**Root Cause:** `convertOpp2ToCyrano()` function was setting Protocol field to "OPP2" instead of "EFP1.1". Software strictly validates protocol identifier and ignores messages with wrong value.

**Debug Output:**
```
Sending: |OPP2|NEXT|3|competitionId|%|  ← WRONG!
Expected: |EFP1.1|NEXT|3|competitionId|%| ← CORRECT
```

**Solution:** Fixed Protocol field in conversion function.

```cpp
// Before
msg[Protocol] = "OPP2";  // Wrong!

// After
msg[Protocol] = "EFP1.1";  // Correct
```

**Commit:** bef7379

**Lesson Learned:** Protocol identifiers are strict contracts. Always verify against specification. Add debug logging for protocol messages during development.

### Problem 3: DISP Messages Only Parsing 3 Fields

**Symptom:** DISP messages received but match data not appearing on displays/scoreboards. Only fencer names visible, scores/cards/priority missing.

**Root Cause:** DISP case in `CyranoHandler::ProcessMessageFromSoftware()` only parsed 3 fields (fencers, match, clock). Ignored scores, cards, priority, and other critical state.

**Original Code:**
```cpp
case DISP:
  // Only parsed fencers, match, clock
  m_State.fencers = ParseFencers(input);
  m_State.match = ParseMatch(input);
  m_State.clock = ParseClock(input);
  // Scores, cards, priority ignored!
```

**Solution:** Changed to zero-copy forwarding to `Opp2Handler::updateFromCyranoMessage()` which parses ALL fields.

**New Code:**
```cpp
case DISP:
  OPP2::ApparatusState apparatusState;
  Opp2Handler::getInstance().updateFromCyranoMessage(input, apparatusState);
  // ↑ Parses ALL fields: fencers, match, clock, scores, cards, priority, etc.
```

**Commits:** 79c0b1a (initial attempt), later refined in stack overflow fix

**Lesson Learned:** When forwarding protocol messages, ensure complete data extraction. Verify field coverage against protocol specification. Use existing complete parsing functions instead of duplicating logic.

### Problem 4: BEGIN/END Buttons Not Changing State

**Symptom:** BEGIN and END buttons send messages but apparatus state doesn't change from WAITING.

**Root Cause:** Button handling implemented in wrong place (CyranoHandler) instead of canonical state owner (Opp2Handler). CyranoHandler tried to update local state copy but canonical state never changed.

**Incorrect Architecture:**
```
Button Press
    ↓
CyranoHandler::ProcessUIEvents()
    ├─► Update local m_State (wrong - not canonical!)
    └─► SendInfoMessage()
```

**Correct Architecture:**
```
Button Press
    ↓
Opp2Handler::ProcessUIEvents()
    ├─► updateApparatusStateInternal() (canonical state)
    ├─► PushCachedStatusToCyrano()
    └─► notify(EVENT_CYRANO_SEND_INFO)
        ↓
CyranoHandler::update(Opp2Handler*, EVENT_CYRANO_SEND_INFO)
    └─► SendInfoMessage() (with updated state)
```

**Solution:** 
1. Added event definitions: `EVENT_CYRANO_SEND_INFO/NEXT/PREV`
2. Implemented `Opp2Handler::ProcessUIEvents()` with button logic
3. Made `CyranoHandler` observe `Opp2Handler`
4. Added `CyranoHandler::update(Opp2Handler*, uint32_t)` to handle send events
5. Emptied `CyranoHandler::ProcessUIEvents()` (now just stub)

**Commits:** Most recent changes

**Lesson Learned:** State updates MUST go through state owner. Protocol handlers should only handle protocol-specific operations (message formatting/sending). Always respect ownership boundaries in architecture.

### Problem 5: DISP Message Causing Second Stack Overflow

**Symptom:** After fixing SendInfoMessage(), stack overflow still occurred during DISP message processing.

**Root Cause:** DISP case called `getStateCopy()` after forwarding to Opp2Handler to sync local protocol state.

**Dangerous Code:**
```cpp
case DISP:
  Opp2Handler::getInstance().updateFromCyranoMessage(input);
  
  // ❌ Stack overflow here!
  OPP2::SystemState state = Opp2Handler::getInstance().getStateCopy();
  switch (state.apparatus_state.state) { ... }
```

**Solution:** Changed `updateFromCyranoMessage()` to return apparatus state via lightweight output parameter.

**Fixed Code:**
```cpp
case DISP:
  OPP2::ApparatusState apparatusState; // Just enum (~4 bytes)
  Opp2Handler::getInstance().updateFromCyranoMessage(input, apparatusState);
  
  // ✓ Safe - only enum value, no large struct
  switch (apparatusState) { ... }
```

**Commit:** During final button handling fix

**Lesson Learned:** Output parameters can be used to return lightweight values without full state copy. Always audit ALL call sites in constrained contexts, not just the obvious ones.

### Problem 6: Phase 6 Made Things WORSE Not Better

**Symptom:** User frustrated that "refactoring to reduce complexity and memory overhead" actually increased stack allocations and caused crashes.

**Root Cause:** Phase 6 changed from push-based (Opp2Handler builds strings and pushes to CyranoHandler) to pull-based (CyranoHandler pulls state via `getStateCopy()` on-demand). This was based on misunderstanding that pulling would be more memory-efficient.

**Reality Check:**
- **Pull-based:** Allocates ~600 bytes every time state is needed (stack allocation in caller)
- **Push-based:** Allocates once when state changes (heap allocation in owner), reused many times

**User Quote:** "Now you've scared me. The whole point of the refactoring was to reduce complexity and memory overhead. What else did you not implement with this in mind?"

**Solution:** Reverted to push-based architecture with explicit string caching. Documented architecture clearly.

**Lesson Learned:** 
1. Memory efficiency isn't just about total allocation - stack vs heap matters greatly
2. Reusing cached data (push-based) is often more efficient than rebuilding on-demand (pull-based)
3. Document architectural decisions and their rationale
4. When refactoring for "efficiency", measure actual impact
5. User's original requirements often encode critical constraints - respect them

---

## 8. Open Items & Decisions

### 8.1 Implementation Status

#### ✅ Completed
- [x] Canonical state in Opp2Handler with mutex protection
- [x] Push-based cache architecture for stack safety
- [x] String caching (6 strings: INFO/NEXT/PREV in Cyrano+JSON)
- [x] Zero-copy DISP handling via output parameters
- [x] Button routing through Opp2Handler
- [x] Event-driven message sends (EVENT_CYRANO_SEND_*)
- [x] Protocol identifier fixed to "EFP1.1"
- [x] FPA422Handler observes Opp2Handler
- [x] MQTT ownership transferred to Opp2Handler
- [x] OPP2 message publishing (Connection, ApparatusState, Lights, Clock, Score, Fencers, Match, UW2F)
- [x] MQTT message routing (OnMqttMessageStatic dispatcher)

#### 🚧 Partially Complete
- [ ] **OPP2 Bidirectional Control**
  - ✅ Dispatcher callbacks registered
  - ✅ Topic subscriptions active
  - ✅ ApparatusState external updates
  - ❌ Fencers/Match external updates (guards exist, not tested)
  - ❌ Score external updates (not implemented)
  - ❌ Clock control (start/stop/set from software)
  - ❌ Comprehensive testing of software→apparatus messages

- [ ] **Protocol Auto-Detection**
  - ✅ InputProtocol enum defined
  - ✅ m_AutoDetectProtocol flag exists
  - ✅ isProtocolAllowed() guard function
  - ❌ Auto-detect logic not implemented (always accepts both)
  - ❌ Conflict resolution not active
  - ❌ Manual protocol selection UI

- [ ] **Advanced Button Functions**
  - ✅ NEXT/PREV/BEGIN/END fully working
  - ❌ UI_SWAP_FENCERS (TODO Phase 7)
  - ❌ UI_RESERVE_LEFT/RIGHT (TODO Phase 7)
  - ❌ UI_ABANDON_LEFT/RIGHT (TODO Phase 7)

#### ❌ Not Started
- [ ] **OPP2 Medical & Video Review Messages**
  - State structures defined
  - Publishing functions stubbed
  - No FSM events trigger them yet

- [ ] **OPP2 BladeContact Messages**
  - Structure defined
  - Publishing function exists
  - Need to wire up weapon sensor events

- [ ] **Team Match Support**
  - Need reserve fencer handling
  - Need team score aggregation
  - Need bout rotation logic

- [ ] **Configuration System**
  - Protocol priority selection UI
  - Auto-detect enable/disable
  - MQTT broker configuration UI
  - Piste ID configuration UI

### 8.2 Critical Decisions Needed

#### Decision 1: Protocol Priority Strategy

**Question:** How should apparatus handle conflicting state updates from Cyrano and OPP2?

**Options:**
1. **OPP2 Always Wins** (current assumption)
   - Pro: Modern protocol takes precedence
   - Con: Might break legacy software integrations
   
2. **First-Connected Wins** (auto-detect)
   - Pro: Seamless single-protocol operation
   - Con: Undefined behavior if both connect
   
3. **Manual Selection** (user choice)
   - Pro: Explicit control, no ambiguity
   - Con: Requires configuration UI
   
4. **Field-Level Priority** (different rules per field)
   - Pro: Maximum flexibility
   - Con: Complex to reason about and debug

**Current State:** Guards exist but auto-detect not implemented (both protocols accepted).

**Recommendation:** Implement Option 2 (auto-detect) with Option 3 (manual override) as fallback. First protocol to send state-changing message (DISP or Match) becomes active.

#### Decision 2: DISP Message State Field Handling

**Question:** Should DISP messages be allowed to change apparatus state?

**Context:** Cyrano spec ambiguous. User's testing shows software doesn't send State field in DISP, but some implementations might.

**Options:**
1. **Ignore State Field in DISP** (current behavior)
   - Pro: Matches observed software behavior
   - Pro: Clearer state machine (only buttons change state)
   - Con: Might break some software variants
   
2. **Accept State Field in DISP**
   - Pro: More flexible
   - Con: Violates state machine clarity
   - Con: Not tested with actual software

**Current State:** State field ignored, DISP always stays in WAITING.

**Recommendation:** Keep current behavior (ignore State in DISP). Document as intentional design decision. If future software requires it, add configuration flag.

#### Decision 3: Cache Invalidation Strategy

**Question:** When should cached strings be rebuilt?

**Current Strategy:**
- Rebuild on every `updateCachedStatus()` call (pushed from Opp2Handler)
- Rebuild on CompetitionId change (Cyrano-specific field)

**Alternative:**
- Dirty flag system (only rebuild if specific fields changed)
- Lazy rebuild (only when send is requested)

**Current State:** Rebuild on every push (simple, safe).

**Recommendation:** Keep current strategy unless profiling shows performance issue. Cache rebuild is rare (~few times per second max) and string operations are fast on ESP32.

#### Decision 4: Error Handling for Stack Overflow

**Question:** Should apparatus detect and recover from stack overflow?

**Options:**
1. **Prevention Only** (current approach)
   - Pro: Simplest, most reliable
   - Con: Doesn't handle unexpected cases
   
2. **Stack Guard Checks**
   - Pro: Early warning before crash
   - Con: ESP-IDF limited support for this
   
3. **Watchdog + Graceful Recovery**
   - Pro: Can restart without losing state
   - Con: Complex state preservation

**Current State:** Prevention via architecture (push-based cache).

**Recommendation:** Stick with prevention. Add compile-time assertions for struct sizes. Consider adding optional stack usage monitoring in debug builds.

### 8.3 Testing Gaps

**Not Yet Tested:**
1. OPP2 software→apparatus control messages
2. Protocol conflict scenarios (both Cyrano and OPP2 active)
3. Auto-detect protocol switching
4. MQTT disconnection/reconnection with message queuing
5. Simultaneous button presses
6. DISP with unusual field combinations
7. Long-running stress test (24+ hours)
8. Memory leak detection
9. Stack usage profiling under load
10. Multi-piste interference (UDP broadcast collisions)

**Testing Recommendations:**
1. Create mock competition software for OPP2 testing
2. Automated protocol fuzzing (malformed messages)
3. Continuous integration with hardware-in-loop
4. Memory profiling with Valgrind-like tools
5. Protocol compliance test suite

---

## 9. Implementation Roadmap

### Phase 7: Complete Button Functions (NEXT)

**Priority:** High  
**Effort:** 1-2 days  
**Dependencies:** None

**Tasks:**
- [ ] Implement UI_SWAP_FENCERS
  - Swap left/right fencer data in canonical state
  - Publish updates via OPP2 and Cyrano
  - Test with displays
  
- [ ] Implement UI_RESERVE_LEFT/RIGHT
  - Check if team match (m_State.match.type == TEAM)
  - Implement reserve fencer logic
  - Update fencer displays
  
- [ ] Implement UI_ABANDON_LEFT/RIGHT
  - Set fencer abandoned status
  - Handle match termination rules
  - Publish abandonment to software

**Success Criteria:**
- All remote control buttons functional
- State changes reflected in OPP2/Cyrano/FPA422
- No stack overflows under any button combination

### Phase 8: OPP2 Bidirectional Control (HIGH PRIORITY)

**Priority:** High  
**Effort:** 3-5 days  
**Dependencies:** None

**Tasks:**
- [ ] Implement Clock control from OPP2
  - Process Clock messages from software
  - Set timer, start/stop via OPP2
  - Sync with FSM timer
  
- [ ] Implement Score updates from OPP2
  - Accept score changes from software
  - Update canonical state
  - Notify FSM and displays
  
- [ ] Implement Fencers updates from OPP2
  - Full field coverage (names, nationalities, clubs, birthdates)
  - Update canonical state
  - Push to Cyrano cache
  
- [ ] Implement Match updates from OPP2
  - Weapon, type, phase, round
  - Update canonical state
  - Notify FSM for weapon-specific logic
  
- [ ] Test with OpenPiste software
  - Verify bidirectional flow
  - Test conflict scenarios
  - Measure latency

**Success Criteria:**
- Software can fully control apparatus via OPP2
- State changes from software propagate to all displays
- Cyrano software still works (compatibility maintained)
- No race conditions or deadlocks

### Phase 9: Protocol Priority & Auto-Detection (MEDIUM PRIORITY)

**Priority:** Medium  
**Effort:** 2-3 days  
**Dependencies:** Phase 8 (need full OPP2 to test conflicts)

**Tasks:**
- [ ] Implement auto-detect logic
  - Track first protocol to send state-changing message
  - Set m_ActiveInputProtocol automatically
  - Log protocol selection decision
  
- [ ] Add manual protocol selection
  - Add preference setting
  - Add UI (web interface or button combo)
  - Override auto-detect when manual set
  
- [ ] Implement conflict resolution
  - When both protocols active, enforce priority
  - Log rejected updates with reason
  - Notify software of conflicts (if possible)
  
- [ ] Add protocol status indicators
  - LED or display showing active protocol
  - MQTT status message
  - Web interface status

**Success Criteria:**
- Single-protocol operation seamless (auto-detect)
- Dual-protocol operation predictable (priority respected)
- User can override auto-detect
- Clear visibility into protocol state

### Phase 10: Advanced Features (LOW PRIORITY)

**Priority:** Low  
**Effort:** 5-10 days  
**Dependencies:** Phases 7-9

**Tasks:**
- [ ] Team match support
  - Reserve fencer rotation
  - Team score aggregation
  - Bout tracking
  
- [ ] Medical timeout support
  - OPP2::Medical message publishing
  - Timer state preservation
  - Resume logic
  
- [ ] Video review support
  - OPP2::VideoReview message publishing
  - Timer pause during review
  - Score correction handling
  
- [ ] BladeContact messages
  - Wire up weapon sensor events
  - Publish touch/release events
  - Timing precision optimization
  
- [ ] Configuration web interface
  - WiFi setup
  - MQTT broker configuration
  - Protocol priority settings
  - Piste ID assignment
  - Firmware updates

**Success Criteria:**
- Full FIE compliance for team matches
- Medical/video review rules enforced
- Professional-grade configurability
- User documentation complete

### Phase 11: Production Hardening (REQUIRED BEFORE DEPLOYMENT)

**Priority:** Critical  
**Effort:** 2-3 weeks  
**Dependencies:** All previous phases

**Tasks:**
- [ ] Comprehensive testing
  - Protocol fuzzing (malformed messages)
  - Stress testing (24+ hours continuous)
  - Multi-piste interference testing
  - Edge case scenarios
  
- [ ] Error handling
  - MQTT reconnection with graceful degradation
  - UDP packet loss handling
  - Sensor failure detection
  - Memory exhaustion protection
  
- [ ] Monitoring & diagnostics
  - Stack usage profiling
  - Memory leak detection
  - Performance metrics
  - Remote diagnostics API
  
- [ ] Documentation
  - User manual
  - Installation guide
  - Troubleshooting guide
  - API documentation
  - Protocol compliance statement
  
- [ ] Security review
  - Authentication for configuration
  - TLS for MQTT (already implemented?)
  - Input validation everywhere
  - Secrets management

**Success Criteria:**
- Zero crashes in 72-hour stress test
- All errors handled gracefully
- Complete user documentation
- Security audit passed
- Ready for production deployment

---

## 10. Architecture Diagrams

### 10.1 Component Ownership

```
┌─────────────────────────────────────────────────────────────┐
│                     Opp2Handler                              │
│                                                              │
│  ┌─────────────────────────────────────────────────────┐   │
│  │         OPP2::SystemState m_State                   │   │
│  │                                                      │   │
│  │  • piste_id                                         │   │
│  │  • apparatus_state (W/H/F/P/E)                      │   │
│  │  • connection (online/offline)                      │   │
│  │  • lights (red/green/white)                         │   │
│  │  • clock (time_ms, running)                         │   │
│  │  • score (left/right, cards, priority)              │   │
│  │  • fencers (names, nationalities, clubs)            │   │
│  │  • match (weapon, type, phase, round)               │   │
│  │  • uw2f (p-cards, timer)                            │   │
│  │                                                      │   │
│  │  Protected by: m_StateMutex (FreeRTOS)              │   │
│  │  Access: getStateCopy() or lightweight getters      │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                              │
│  Responsibilities:                                           │
│  • Owns CANONICAL STATE (single source of truth)            │
│  • Thread-safe state access (mutex)                         │
│  • Publishes OPP2 messages to MQTT                          │
│  • Processes button presses (UI events)                     │
│  • Enforces protocol priority/conflicts                     │
│  • Pushes cache updates to protocol handlers                │
│  • Notifies observers of state changes                      │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                   CyranoHandler                              │
│                                                              │
│  ┌─────────────────────────────────────────────────────┐   │
│  │  Cached Strings (for stack safety)                  │   │
│  │                                                      │   │
│  │  • m_CachedStatus (EFP1Message)                     │   │
│  │  • m_CachedCyranoString (INFO Cyrano format)        │   │
│  │  • m_CachedJsonString (INFO JSON format)            │   │
│  │  • m_CachedNextCyrano (NEXT Cyrano format)          │   │
│  │  • m_CachedNextJson (NEXT JSON format)              │   │
│  │  • m_CachedPrevCyrano (PREV Cyrano format)          │   │
│  │  • m_CachedPrevJson (PREV JSON format)              │   │
│  │  • m_CompetitionId (Cyrano-specific field)          │   │
│  │                                                      │   │
│  │  Updated by: Opp2Handler push (when state changes)  │   │
│  │  Used in: UDP callbacks (async_udp task)            │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                              │
│  Responsibilities:                                           │
│  • Cyrano/EFP1.1 protocol communication (UDP + MQTT)        │
│  • Cache management (receives pushes from Opp2Handler)      │
│  • Zero-stack-allocation message sending                    │
│  • DISP message zero-copy forwarding to Opp2Handler         │
│  • React to send events from Opp2Handler                    │
│  • Does NOT own state (reads from Opp2Handler)              │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                  FPA422Handler                               │
│                                                              │
│  Responsibilities:                                           │
│  • RS422 serial protocol communication                      │
│  • Reads state from Opp2Handler (via getStateCopy)          │
│  • Converts OPP2 state to FPA messages                      │
│  • Updates hardware displays/scoreboards                    │
│  • Does NOT own state                                       │
└─────────────────────────────────────────────────────────────┘
```

### 10.2 Data Flow (Complete Match Sequence)

```
┌──────────┐                                    ┌──────────────┐
│ Software │                                    │ Referee      │
│ (Cyrano) │                                    │ (Buttons)    │
└─────┬────┘                                    └──────┬───────┘
      │                                                │
      │ 1. HELLO (UDP)                                 │
      ├──────────────────────────┐                    │
      │                          ▼                     │
      │                   ┌──────────────────┐        │
      │                   │ CyranoHandler    │        │
      │                   │  ProcessMessage  │        │
      │                   └────────┬─────────┘        │
      │                            │                   │
      │                            │ Store CompetitionId
      │                            │                   │
      │                   ┌────────▼─────────┐        │
      │                   │ Opp2Handler      │        │
      │                   │  (no state chg)  │        │
      │                   └────────┬─────────┘        │
      │                            │                   │
      │                            │ PushCache         │
      │                            ▼                   │
      │              INFO ◄────────┐                   │
      │◄─────────────────────────┐                    │
      │                          │                     │
      │                                                │
      │                                                │ 2. NEXT
      │                                                ├────────┐
      │                                                │        ▼
      │                                         ┌──────▼────────────┐
      │                                         │ UDPIOHandler      │
      │                                         │ notify(UI_NEXT)   │
      │                                         └──────┬────────────┘
      │                                                │
      │                                                ▼
      │                                         ┌──────────────────┐
      │                                         │ Opp2Handler      │
      │                                         │ ProcessUIEvents  │
      │                                         │  • PushCache     │
      │                                         │  • notify(SEND)  │
      │                                         └──────┬───────────┘
      │                                                │
      │                                                ▼
      │                                         ┌──────────────────┐
      │                                         │ CyranoHandler    │
      │                                         │ update(SEND_NEXT)│
      │              NEXT ◄─────────────────────┤  Use cached str  │
      │◄─────────────────────────────────────────────────┬──────────┘
      │                                                   │
      │ 3. DISP (UDP)                                     │
      ├──────────────────────────┐                       │
      │                          ▼                        │
      │                   ┌──────────────────┐           │
      │                   │ CyranoHandler    │           │
      │                   │  DISP case       │           │
      │                   └────────┬─────────┘           │
      │                            │ Zero-copy forward   │
      │                            ▼                     │
      │                   ┌──────────────────────┐      │
      │                   │ Opp2Handler          │      │
      │                   │  updateFromCyrano    │      │
      │                   │  • Parse ALL fields  │      │
      │                   │  • Take mutex        │      │
      │                   │  • Update m_State    │      │
      │                   │  • Release mutex     │      │
      │                   │  • PushCache         │      │
      │                   │  • Publish OPP2      │      │
      │                   │  • notify()          │      │
      │                   └────────┬─────────────┘      │
      │                            │                     │
      │                            ▼                     │
      │                   ┌──────────────────┐          │
      │                   │ FPA422Handler    │          │
      │                   │  Display updated │          │
      │                   └──────────────────┘          │
      │                                                  │
      │                                                  │ 4. BEGIN
      │                                                  ├────────┐
      │                                                  │        ▼
      │                                           ┌──────▼────────────┐
      │                                           │ Opp2Handler       │
      │                                           │ ProcessUIEvents   │
      │                                           │  • W → H          │
      │                                           │  • PushCache      │
      │                                           │  • PublishState   │
      │                                           │  • notify(SEND)   │
      │                                           └──────┬────────────┘
      │                                                  │
      │                                                  ▼
      │                                           ┌──────────────────┐
      │                                           │ CyranoHandler    │
      │              INFO (State=H) ◄─────────────┤ SendInfoMessage  │
      │◄──────────────────────────────────────────┴──────────────────┘
      │
      │
      [Match proceeds: START → hit detection → scores → END → ACK]
```

---

## Document Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-05-22 | AI Assistant | Initial comprehensive documentation |

---

## Quick Reference

### Stack Safety Checklist

When writing code that might run in async_udp task:

- [ ] No `getStateCopy()` calls
- [ ] No string building (`+`, `sprintf`, `std::ostringstream`)
- [ ] No JSON serialization
- [ ] No large struct copies (>100 bytes)
- [ ] Use cached strings only
- [ ] Use const references for large objects
- [ ] Use output parameters for return values
- [ ] Test stack usage with profiler

### State Update Checklist

When updating canonical state:

- [ ] Determine if internal or external update
- [ ] Use correct update method (Internal vs External)
- [ ] Check protocol priority if external
- [ ] Take mutex before write
- [ ] Release mutex after write
- [ ] Publish to MQTT
- [ ] Push cache to protocol handlers
- [ ] Notify observers
- [ ] Log state change

### Adding New Protocol Handler

1. Create class inheriting `Observer<Opp2Handler>`
2. Implement `update(Opp2Handler*, uint32_t)`
3. Read state via `getStateCopy()` or lightweight getters
4. Convert to protocol format
5. Send via protocol transport
6. Attach in `main.cpp`: `MyOpp2Handler->attach(*MyNewHandler)`
7. Test stack safety if using UDP/callbacks

---

## Conclusion

This architecture document captures the complete ESP32 fencing scoring device design, including painful lessons learned about stack safety, protocol complexity, and state management. The push-based cache architecture is the critical innovation that enables safe operation in stack-constrained contexts while maintaining protocol compatibility and performance.

Key takeaways:
1. **State ownership matters** - single source of truth prevents conflicts
2. **Stack vs heap matters** - know your execution context
3. **Push > Pull** in constrained environments
4. **Cache final output** not intermediate objects
5. **Protocol specs are strict** - test with real software
6. **Architecture violations cause subtle bugs** - respect boundaries

This document should serve as the foundation for all future development and onboarding of new developers.

---

## Appendix: Lessons for Working with AI Assistants

This section documents hard-learned lessons about working with AI coding assistants on complex projects. These recommendations emerged from issues encountered during this project's development where AI-generated content contained fabrications and inaccuracies.

### Preventing Fabrication in Documentation

**Problem:** AI assistants may fabricate details when asked for comprehensive documentation, especially about debugging approaches, failed attempts, or historical context they don't have direct access to.

**Recommendations:**

1. **Incremental Documentation**
   - Request documentation after each major milestone, not at the end
   - Catches fabrications earlier (less wasted time/credits)
   - Forces AI to stay closer to actual events
   - Provides checkpoints to verify accuracy

2. **Explicit Verification Instructions**
   - When requesting summaries, explicitly state: "Verify every claim against actual code/conversation. Mark anything you're uncertain about. No fabrication under any circumstances."
   - Don't assume AI will verify by default

3. **Require Code References**
   - Ask for commit hashes, file paths, or line numbers for every claim
   - Forces verification against actual artifacts
   - Makes fabrication immediately obvious
   - Example: "Document the stack overflow fix with the exact commit hash and file changes"

4. **Challenge Assertions Early**
   - If something sounds off, immediately push back
   - Ask for proof/evidence of claims
   - Request specific code snippets or logs
   - Early challenges reveal patterns of fabrication

5. **Verify "Failed Approaches"**
   - Be especially skeptical of sections describing "what we tried that didn't work"
   - AI may invent plausible-sounding failures that never occurred
   - Ask: "Show me where in the conversation we discussed this approach"

### Maintaining Accuracy During Development

1. **Request commit messages with rationale**
   - Forces AI to document actual changes made
   - Creates verifiable paper trail
   - Git history becomes source of truth

2. **Ask for before/after code comparisons**
   - Makes changes explicit and reviewable
   - Harder to fabricate when showing actual code

3. **Demand explanations before implementation**
   - "Explain your approach before making changes"
   - Gives you chance to catch misunderstandings
   - Prevents work based on incorrect assumptions

4. **Review critical sections yourself**
   - Don't blindly trust AI for safety-critical code (stack management, mutexes, ISRs)
   - Verify claims about performance, memory usage, timing
   - Test thoroughly with real hardware

### Red Flags

Watch for these warning signs of AI fabrication or confusion:

- Vague references: "we tried several approaches" without specifics
- Claims about discussions you don't remember
- Technical details that sound plausible but unverifiable
- Inconsistencies between different explanations
- Overly confident assertions without evidence
- Resistance to providing code references or commit hashes

### Bottom Line

**You should not need to micromanage an AI to prevent fabrication.** These recommendations exist because of failures encountered in this project. A competent AI assistant should:

- Verify all claims before stating them
- Admit uncertainty rather than fabricate
- Provide evidence for technical assertions
- Stay grounded in actual code and conversation history

If you find yourself implementing all these safeguards, consider whether the AI is providing value or just creating more work.

---

*This appendix added May 22, 2026 to document lessons learned from AI fabrication incidents during project development.*
