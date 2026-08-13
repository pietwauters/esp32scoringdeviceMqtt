# Heap Memory Analysis — OPP2/Cyrano Publish Path

**Date:** 2026-08-13
**Trigger:** Web remote button presses were crash-rebooting the device. Root-caused to
uncaught `std::bad_alloc` from heap allocation failures under a tight/fragmented heap.
A `reserve()` fix on one allocation site (`EFP1Message`'s constructor) reduced but did
not eliminate the crashes — a second, distinct allocation site crashed within the same
evening (`AsyncWebServerRequest::send()` itself), and a *third* trigger of the *same*
already-`reserve()`-fixed constructor crashed again later under continued use. This
document is the requested "stop and analyze before implementing further" pass: what
actually consumes heap, why, whether it's necessary, and what the alternatives are —
for both the Cyrano/EFP1 path and the OPP2 path, plus a broader sweep for other
consumers. No further code changes were made while writing this; two temporary
heap-delta logging statements added for measurement were reverted (see Methodology).

## TL;DR

- **This is not "two protocols is too much."** Both protocols allocate freely on the
  heap on every single state change (every score/card/clock/light update), and neither
  was written with this device's real headroom in mind.
- **The single biggest, most frequent offender is `EFP1Message::ToString()`** (Cyrano
  wire-string building) — an O(n) chain of `std::string` concatenations, unconditional
  on every state change regardless of whether a Cyrano CMS is even connected.
- **The OPP2 side is not the safe one.** Every OPP2 publish serializes through
  ArduinoJson's `StaticJsonDocument<N>`, which — despite the name — is a **heap-backed,
  dynamically-growing document in ArduinoJson v7**, not the fixed/stack buffer it was in
  v6. This project pins `^7.0.0`. This is very likely why "the JSON library chosen for
  memory performance" no longer delivers that: the v6→v7 upgrade silently changed what
  `StaticJsonDocument<N>` means.
- **The crashes are a fragmentation problem, not a single-bad-allocation problem.**
  `reserve()`-ing one call site reduced crash frequency but a repeat crash at the *same*,
  *already-fixed* site proves the heap can still be too fragmented for even the minimal
  required block, after enough churn. Fixing individual call sites is real progress but
  not a complete fix on its own.

## Methodology

- Read the actual library source shipped in `.pio/libdeps/esp32dev/` (ArduinoJson 7.4.3,
  ESPAsyncWebServer, AsyncTCP) rather than assuming behavior from documentation/memory.
- Grepped `src/*.cpp`/`src/*.h` for `std::string`, `std::vector`, Arduino `String`,
  `StaticJsonDocument`/`JsonDocument`, and manual `new`/`malloc` across the whole
  project to find every plausible hot-path heap consumer, not just the ones that had
  already crashed.
- Cross-checked against **two independently captured, decoded crash backtraces** from
  real hardware (both under `xtensa-esp32-elf-addr2line`) — this is the strongest
  evidence in this document, since it shows exactly which allocations failed in
  practice, not just which ones look risky on paper.
- Attempted a third, live heap-delta measurement pass (temporary
  `heap_caps_get_free_size()`/`heap_caps_get_minimum_free_size()` logging around
  `PushCachedStatusToCyrano()` and `PublishClock()`). This is reverted — not present in
  the current tree — because the attempt itself hit the exact crash under investigation
  before clean numbers could be captured, which is itself informative (see "Confirmed
  crash evidence" below) but doesn't give byte-level deltas. The qualitative/structural
  analysis below is corroborated by two real crash traces, so the lack of clean delta
  numbers doesn't leave this ungrounded.

## Confirmed crash evidence (real hardware, this session)

**Crash 1 — `AsyncWebServerResponse::addHeader()`:**
```
AsyncWebServerResponse::addHeader → std::list<AsyncWebHeader>::emplace_back
  → operator new → std::bad_alloc → uncaught → abort()
```
Triggered by concurrent HTTP connections (mitigated with a request-concurrency guard in
`WebRemoteHandler.cpp`, not by fixing this allocation itself — it's inside
ESPAsyncWebServer, not project code).

**Crash 2 — `EFP1Message::EFP1Message()` (first occurrence):**
```
WebRemoteHandler → InputChanged() → FencingStateMachine::update()
  → Opp2Handler::update() → updateClockInternal() → PushCachedStatusToCyrano()
  → EFP1Message::EFP1Message() → std::vector<std::string>::push_back()
  → operator new → std::bad_alloc → uncaught → abort()
```
Fixed with `reserve()` on the three vectors (`src/EFP1Message.cpp`) — turns ~6 growing
reallocations per vector into 1 allocation of the final size. Verified via 20 rounds of
concurrent-load + 60 rapid clock-toggle stress testing with **zero** crashes afterward.

**Crash 3 — `AsyncWebServerRequest::send()` (distinct from Crash 1):**
```
WebRemoteHandler::handleState() → AsyncWebServerRequest::send()
  → beginResponse() → operator new → std::bad_alloc → uncaught → abort()
```
This is inside ESPAsyncWebServer's own response construction — not a project
allocation, and not something `reserve()` can touch. Confirmed the crash surface isn't
limited to code this project owns.

**Crash 4 — `EFP1Message::EFP1Message()` again**, at the *same* already-`reserve()`-fixed
constructor, during a later, unrelated test session (plain `toggle_timer` presses, not a
concurrency stress test):
```
Opp2Handler::update() → updateClockInternal() → PushCachedStatusToCyrano()
  → EFP1Message::EFP1Message() → operator new → std::bad_alloc → abort()
```
This is the important one. `reserve()` reduces a vector's allocation from ~6 shrinking/
growing attempts to exactly 1, sized to fit. It does **not** guarantee that one
allocation succeeds — if the heap has fragmented enough that no free block of that
exact size exists, the single `reserve()`'d allocation fails just as the un-reserved
ones did. The fact that this happened again, at the same site, after the fix, under
*ordinary* single-button-press usage (no artificial concurrency), is direct evidence
that **the problem is cumulative fragmentation from continuous small alloc/free churn
across the whole publish pipeline — not any one call site.**

## Path 1: EFP1Message / Cyrano wire format

Every state change calls `Opp2Handler::PushCachedStatusToCyrano()`, which:

1. Constructs a local `EFP1Message` (3× `std::vector<std::string>`, 41 fields total,
   `reserve()`'d as of this session — 3 allocations, sized correctly).
2. Calls `CyranoHandler::updateCachedStatus()`, which:
   - `m_CachedStatus = status;` — `EFP1Message::operator=` copy-assigns all 41
     `std::string` fields one at a time (`(*this)[i] = rhs[i]`). Each assignment may
     reallocate if the incoming value's length exceeds the target's current
     capacity/SSO buffer (libstdc++ SSO is ~15 bytes — field values like fencer names,
     competition IDs, or multi-digit scores routinely exceed that).
   - `RebuildCachedStrings()` → `m_CachedStatus.ToString(m_CachedCyranoString)`:
     ```cpp
     Buffer = "|";
     for (int i = 0; i < GetNrOfGeneralFields(); i++)
       Buffer = Buffer + mGeneralFields[i] + "|";     // ← 41 times, across 3 loops
     ```
     **This is the single heaviest, most frequent allocator in the whole publish path.**
     Every `Buffer + X + "|"` constructs two temporary `std::string` objects before the
     result is assigned back — classic `std::string`-concatenation-in-a-loop, O(n)
     temporary allocations plus however many times `Buffer` itself needs to grow (it is
     never `reserve()`'d). This runs 41 times per call, every single state change,
     **unconditionally, regardless of whether a Cyrano CMS is even connected.**
   - `MakeNextMessageString()` / `MakePrevMessageString()` — same pattern, smaller (4
     fields each), lower impact.

**Is this necessary?** The *data model* (41 named fields, string-typed to match the
EFP1.1 wire format) is necessary — that's the protocol. The *mechanism* (heap-allocated
vectors of heap-allocated strings, rebuilt via unreserved concatenation, on every state
change, unconditionally) is not inherent to the protocol — it's an implementation
choice that predates this device's real memory constraints being understood.

**Alternatives, cheapest to most invasive:**

| Option | Effort | Removes what | Residual risk |
|---|---|---|---|
| **Skip the whole Cyrano push when no CMS is connected** (the "option 2" already discussed) | Small | All of the above, when Cyrano isn't in use | OPP2 path still allocates (see below); Cyrano-in-use sessions unaffected |
| **Rewrite `ToString()` to build directly into a fixed output buffer** (snprintf/append-style, no intermediate `std::string`s) | Medium | The single biggest churn source, unconditionally, whether or not Cyrano is active | Field-storage (`std::vector<std::string>`) still allocates on `operator=` |
| **Replace `std::vector<std::string>` field storage with fixed-size `char[N]` buffers** | Larger (touches `EFP1Message`'s public interface, `operator[]`, every caller) | Removes the `operator=` and constructor allocations too | Biggest EFP1Message churn eliminated entirely; largest diff |

Note CLAUDE.md's invariant #4 ("publish → push cache → notify, never skip one") means
the "skip when unused" option is a **protocol-adjacent behavior change**, not a pure bug
fix — this is why it was flagged for explicit permission rather than implemented
directly. The `ToString()` rewrite and the field-storage change are pure implementation
changes with no behavior/protocol impact and don't have that constraint.

## Path 2: OPP2 / ArduinoJson

Every OPP2 publish (`PublishLights`, `PublishClock`, `PublishScore`, `PublishFencers`,
`PublishMatch`, `PublishUW2F`, etc. — 12 message types in `opp2_serialize.h`) does:

```cpp
static SerializeError serialize(const Lights& msg, char* buf, size_t buf_size) {
    StaticJsonDocument<JSON_SIZE_LIGHTS> doc;   // JSON_SIZE_LIGHTS = 128
    JsonObject root = doc.to<JsonObject>();
    ... build the JSON tree ...
    return detail::finalize(doc, buf, buf_size); // serializes into caller's stack buf
}
```

The `JSON_SIZE_*` constants (96–512 bytes depending on message type) and the "Static"
naming clearly reflect a design written for **ArduinoJson v6**, where
`StaticJsonDocument<N>` really was a fixed-size buffer — frequently entirely
stack-resident for small N, genuinely zero heap. That's almost certainly the "chosen for
its memory performance" reasoning being recalled.

This project's `platformio.ini` pins `bblanchon/ArduinoJson @ ^7.0.0`. In ArduinoJson
v7, confirmed by reading `compatibility.hpp` in the bundled library:

```cpp
template <size_t N>
class ARDUINOJSON_DEPRECATED("use JsonDocument instead") StaticJsonDocument
    : public JsonDocument {
 public:
  using JsonDocument::JsonDocument;
  size_t capacity() const { return N; }   // ← purely cosmetic, reserves nothing
};
```

`StaticJsonDocument<N>` is now a **deprecated compatibility shim** — literally just
`JsonDocument` with a `capacity()` getter that reports `N` without reserving anything.
`JsonDocument`'s default allocator, confirmed in `Memory/Allocator.hpp`:

```cpp
class DefaultAllocator : public Allocator {
  void* allocate(size_t size) override { return malloc(size); }
  ...
};
```

**Every OPP2 publish mallocs and grows a JSON memory pool on the heap**, exactly the
same class of risk as the EFP1Message path — it just wasn't the specific site that
crashed first tonight. The `JSON_SIZE_*` sizing that looks like a safety budget doesn't
actually constrain anything in v7.

**One mitigating factor found:** `PublishClock()` (and presumably the other `Publish*`
functions — spot-checked this one) guards with `if (!mqttClient.isConnected()) return;`
— OPP2 publishing is skipped entirely when there's no broker connection. The Cyrano path
has no equivalent guard against "no CMS connected." This is the concrete asymmetry
behind "OPP2 already avoids needless work in one case, Cyrano doesn't in any case."

**Alternatives, cheapest to most invasive:**

| Option | Effort | Notes |
|---|---|---|
| **Pin ArduinoJson to v6.x** (`bblanchon/ArduinoJson @ ^6.21.0` or similar) | Very small (one line in `platformio.ini`) | The OPP2 library only uses the old-style `StaticJsonDocument<N>` API throughout — confirmed no v7-only features (`JsonDocument{allocator}`, `shrinkToFit()`, etc.) are used anywhere in `opp2_serialize.h`/`opp2_deserialize.h`, so this is very likely a clean drop-in. Restores genuine fixed/stack-resident buffers. Downside: ArduinoJson v6 is EOL upstream (no further updates), a real but modest cost for a vendored, rarely-updated dependency. |
| **Supply a custom fixed-buffer `Allocator` to ArduinoJson v7** | Medium | v7's `Allocator` is a clean 3-method abstract interface (`allocate`/`deallocate`/`reallocate`), confirmed in `Memory/Allocator.hpp`. A small static-arena allocator (bump allocator into a fixed `static uint8_t[N]`, reset per call) passed to each `JsonDocument`'s constructor would restore non-heap behavior while staying on the maintained v7 branch. Requires either wrapping every `StaticJsonDocument<N> doc;` call site in `opp2_serialize.h` or changing the OPP2 library's serialize signature to accept an allocator. |
| **Drop ArduinoJson for these messages, hand-roll with `snprintf`** | Larger (12 message types to rewrite) | Zero heap, zero library dependency for this path. `WebRemoteHandler::handleState()` (this session's own code) is a working proof of this exact pattern already in the codebase — fixed `char buf[512]` + `snprintf`, no allocation at all. Most invasive since it touches every message type, but the pattern is already proven to work and to be fast to write. |

## Other heap consumers surveyed

- **`network.cpp`'s provisioning/calibration HTML** (`getProvisionHtml`,
  `getCalibrationHtml`) — heavy Arduino `String` concatenation (28 occurrences), but
  this only runs during initial device setup/Tier-A provisioning, not during live
  scoring. Real but low-priority; not touched by this analysis.
- **`FPA422Handler`'s event queue** — `xQueueCreate(16, sizeof(uint32_t))`. Fixed-size
  items, no heap involvement per event. Confirmed clean; this is the good pattern
  CLAUDE.md already points to.
- **MQTT client (`AtlasAsyncMqttClient`)** — thin wrapper around ESP-IDF's own
  `esp_mqtt_client_publish()`, not a custom/AsyncMqttClient buffer implementation. This
  is core ESP-IDF, generally well-tested; out of scope for this project to fix and not
  flagged as a concern.
- **`TierAProvisioning.cpp`** — uses real (non-deprecated) `JsonDocument doc;` twice,
  both inside `HandleResponse()`, part of the one-time certificate-provisioning
  handshake. Same v7 heap-allocation characteristic as OPP2 messages, but fires rarely
  (once at provisioning, not per state change) — much lower priority.
- **`WS2812BLedStrip.cpp` / `TimeScoreDisplay.cpp`** — negligible `String`/heap usage,
  not a concern.
- **ESPAsyncWebServer/AsyncTCP internals** (response objects, header lists) — real
  consumers (Crash 1 and Crash 3 above came from here), but they're third-party library
  internals, not something to "fix" directly — the concurrency guard already added is
  the practical mitigation available without patching the library.

## What this means for next steps

The original "option 2" (skip Cyrano push when no CMS connected) is still worth doing —
it's the only option here that's a pure frequency reduction with no code-shape change —
but per the crash evidence above, it would **not** fully resolve the crash risk on its
own, because the OPP2/ArduinoJson path has the same underlying heap-allocation
characteristic and is always active. A durable fix needs to address both paths.

This document deliberately stops at analysis, per your request. Happy to turn any of
the rows in the two alternatives tables into an actual implementation plan once you've
decided which combination you want.
