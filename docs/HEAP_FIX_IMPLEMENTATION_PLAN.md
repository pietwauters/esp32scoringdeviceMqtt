# Heap Fix Implementation Plan — EFP1/Cyrano + OPP2/ArduinoJson

**Status:** Plan only — nothing in this document has been implemented yet.
**Depends on:** `docs/HEAP_MEMORY_ANALYSIS.md` (root-cause analysis; read that first).

## Goal

Eliminate heap allocation from the two hot paths identified in the analysis —
`EFP1Message`/`CyranoHandler`'s Cyrano wire-string building, and OPP2's
`StaticJsonDocument`-based JSON serialization — by moving both to fixed-size buffers
sized from known maximums, matching the pattern `OPP2::FencerSide` and friends already
use (`char name[64]`, `char nation[5]`, etc. in `opp2_types.h`). Also reclaim heap that's
permanently tied up for an unrelated reason (Branch 0, below) — found while answering a
question about it, but directly relevant: it increases the margin available while
developing and soak-testing the other two branches.

## Why three branches, not one

The three fixes are structurally independent — different files, different subsystems.
Neither of the two main fixes depends on the other landing first, and the flash-vs-heap
change (Branch 0) is unrelated to both except that it improves headroom for testing them.
Splitting them means:

- A regression in one is isolated and bisectable — doesn't block or muddy the others.
- Any of the three can be reverted independently without losing the others' work.
- Testing stays scoped: the EFP1 branch's soak test doesn't need OPP2 JSON traffic to
  be meaningful, and vice versa; Branch 0 is testable in isolation with the existing
  reproduction steps from this session (concurrent load + rapid clock toggles).

**Branch 0: `fix/web-remote-progmem`** — `src/WebRemoteHandler.cpp`, `strip_web_assets.py`,
`platformio.ini` (drops the `spiffs` partition). Cheapest and lowest-risk of the three;
do this one first since it buys headroom for the other two. See its own section below.

**Branch 1: `fix/efp1-fixed-buffers`** — `src/EFP1Message.h/.cpp`, `src/CyranoHandler.h/.cpp`,
call-site updates in `src/Opp2Handler.cpp`. Entirely within this repo.

**Branch 2: `fix/opp2-json-heap`** — scope depends on which option is chosen (see below);
in the cheapest case, a single line in this repo's `platformio.ini`; in the others, work
in a separate clone of `opp2-library` (no version pin exists today on that dependency —
worth noting as a separate, pre-existing reproducibility gap, unrelated to this plan but
visible while looking at `platformio.ini`).

Both branch off current `main` (which already has this session's `reserve()` fix and the
request-concurrency guard — real improvements, not being reverted). Merge order doesn't
matter between them; a combined soak test (see "Integration testing" below) happens
after both are individually verified, since in real operation both paths run and
allocate concurrently.

---

## Branch 0: `fix/web-remote-progmem`

Not part of the original heap-churn investigation — surfaced by a direct question
("shouldn't the web remote's html/js/css live in flash, not SPIFFS?") that turns out to
matter a lot here, not just as an independent cleanup.

### What's there today

`WebRemoteHandler::serveSpiffsFile()` loads each of the three web-remote files
(`index.html.gz`, `style.css.gz`, `app.js.gz` — ~1.6KB + 1.7KB + 2.4KB gzipped) from
SPIFFS **once at boot**, into a `new (std::nothrow) char[size]` heap buffer that is
**never freed**, and serves that same buffer on every request via
`request->beginResponse(200, contentType, (const uint8_t*)buf, n)`.

### Why this is a heap problem, not just a SPIFFS-vs-flash preference

That `beginResponse(..., const uint8_t*, size_t)` overload already resolves to
`AsyncProgmemResponse` (confirmed by reading `WebRequest.cpp`/`WebResponseImpl.h` in the
vendored ESPAsyncWebServer source) — a response class that reads via `memcpy_P`, which on
ESP32 is just `memcpy` (flash is memory-mapped on this chip, unlike AVR — there's no
special access pattern needed). **It does not care where the pointer points.** Right now
it's a heap pointer. It could just as easily be a `static const uint8_t[]` array living
in flash `.rodata`, set at compile time — the response-serving code doesn't change at
all, only where `buf`/`n` come from.

That means roughly **5.7KB of heap is permanently reserved for the device's entire
uptime**, for no reason connected to the web remote actually working — it's exactly as
functional served from flash. That 5.7KB is heap the OPP2/EFP1 hot paths (Branches 1
and 2) don't get to use. Freeing it doesn't fix the churn those branches address, but it
directly increases the margin before churn becomes fatal — which is why this is worth
doing first, not just whenever there's spare time.

**Bonus, checked while looking at this:** `grep -rl "SPIFFS\." src/*.cpp` finds only
`WebRemoteHandler.cpp` — nothing else in this codebase touches SPIFFS. This fix removes
the SPIFFS partition dependency entirely, not just the boot-time heap buffer.

### Implementation

- `strip_web_assets.py` already does the minify+gzip work needed (`terser` for JS, gzip
  level 9). Change its output from writing `.gz` files into `data/` (a SPIFFS image
  source) to emitting a generated C header — e.g. `src/web_assets_generated.h` — with
  one `static const uint8_t ..._gz[] = { 0x1f, 0x8b, ... };` and a matching `_len`
  constant per file. Same minify/gzip logic, different output format (a Python
  byte-array-literal formatter instead of a raw file write).
- `WebRemoteHandler::serveSpiffsFile()` (rename it — it no longer touches SPIFFS) drops
  the `SPIFFS.open()`/`readBytes()`/`new (std::nothrow) char[]` sequence entirely and
  just points `beginResponse()` at the generated array + its `_len` constant directly.
  `WebRemoteHandler::begin()` drops its `SPIFFS.begin(true)` call.
- `platformio.ini` / `min_spiffs.csv`: remove the `spiffs` partition. That 128KB either
  goes unused or gets reclaimed for something else later (e.g. larger OTA app
  partitions) — a separate decision, not part of this fix.
- The generated header is a build artifact (regenerated every build from `web_src/`),
  same as `data/` is today — stays git-ignored, not checked in.

### Verification

- Confirm `/remote`, `/style.css`, `/app.js` still serve byte-identical gzipped content
  (same `gunzip`-and-diff check used earlier this session).
- Confirm `/heap` shows the expected ~5.7KB free-heap improvement immediately after
  boot, before any state changes — a clean, isolated before/after comparison since
  nothing else changes in this branch.
- Confirm boot succeeds with no SPIFFS partition present at all (no more
  `SPIFFS.begin(true)` call means no dependency on that partition existing, but worth
  confirming the partition table change itself doesn't break anything else that assumed
  its presence, e.g. OTA size calculations).

---

## Branch 1: `fix/efp1-fixed-buffers`

### Step 0 — Field-length table (needs your input, not a guess)

You offered to supply exact max lengths per field rather than have me infer them. That's
the right call — I don't want to bake in a guessed number that turns out too small and
silently truncates a real fencer name or competition ID under EFP1 protocol pressure.
What I have as a cross-check from `OPP2::FencerSide` (`opp2_types.h`) — useful as a
sanity bound since EFP1's fencer fields are populated from/to these exact OPP2 fields —
is:

| Field (OPP2 struct) | Current size | EFP1 field(s) it maps to |
|---|---|---|
| `id[32]` | 32 | `RightFencerId` / `LeftFencerId` |
| `name[64]` | 64 | `RightFencerName` / `LeftFencerName` |
| `nation[5]` | 5 | `RightFencerNation` / `LeftFencerNation` |
| `piste_id[PISTE_ID_MAX]` | 32 | `PisteId` |

Everything else (Command, State, scores, card flags, priority, timestamps, weapon,
competition type, etc.) is a short status code, single digit/char, or small formatted
number — a handful of bytes each. I'll use whatever table you provide as the source of
truth for all 41 fields; where you don't have a strong opinion, I'll default to the
OPP2-side sizes above for the shared fields, and a conservative small fixed size (e.g.
8–16 bytes) for the small status/numeric ones, flagged in the diff for your review
rather than assumed silently.

### Step 1 — Design a fixed-capacity field type

`EFP1Message::operator[]` currently returns `std::string&`, and ~39 call sites across
`Opp2Handler.cpp`/`CyranoHandler.cpp` use it like a normal string (`(*this)[Command] =
"INFO"`, `if ((*this)[Command] == "HELLO")`, etc.). Rewriting all 39 call sites is one
option; a smaller-blast-radius option is a lightweight fixed-capacity string type that
those call sites don't need to change for:

```cpp
template <size_t N>
class FixedField {
 public:
  FixedField() { m_buf[0] = '\0'; }
  FixedField& operator=(const char* s) {
    strncpy(m_buf, s, N - 1);
    m_buf[N - 1] = '\0';
    return *this;
  }
  FixedField& operator=(const std::string& s) { return *this = s.c_str(); }
  operator const char*() const { return m_buf; }
  bool operator==(const char* s) const { return strcmp(m_buf, s) == 0; }
  bool empty() const { return m_buf[0] == '\0'; }
  size_t appendTo(char* dst, size_t dstCap, size_t pos) const; // bounded append helper
 private:
  char m_buf[N];
};
```

Bounded (`strncpy` + explicit null-termination, per field's own max — not one global
size) so a value from a real, untrusted Cyrano DISP can't overflow — the same discipline
`getPisteId()` already uses correctly today, applied consistently across all 41 fields
this time (this is also CLAUDE.md invariant #7 territory: verify nothing gets silently
dropped, not just "doesn't crash").

Field storage becomes fixed arrays of these, sized per-field from the Step 0 table —
matching the existing OPP2 convention (fixed buffers with per-field max lengths) rather
than one uniform size for everything.

**Verify at this step, before moving on:** the ~39 call sites still compile unchanged.
If any call site needs a real `std::string` (e.g. passing a field into something that
takes `const std::string&` elsewhere), that's a signal the wrapper's interface is
missing something — fix the wrapper, not the call site, to keep the blast radius small.

### Step 2 — Rewrite `ToString()` / `MakeNextMessageString()` / `MakePrevMessageString()`

Currently:
```cpp
Buffer = Buffer + mGeneralFields[i] + "|";   // 41 times, 2 temporaries per iteration
```
This is the single biggest churn source found in the analysis — replace with bounded
appends into a fixed output buffer (size from summing the Step 0 table, matching the
`char payloadBuf[512]`-style stack buffers this codebase already uses in every
`Opp2Handler::Publish*` function):
```cpp
size_t pos = 0;
pos += appendBounded(out, outCap, pos, "|");
for (int i = 0; i < GetNrOfGeneralFields(); i++) {
  pos += appendBounded(out, outCap, pos, mGeneralFields[i]);
  pos += appendBounded(out, outCap, pos, "|");
}
```
No intermediate `std::string` objects, no reallocation — `out`/`outCap` is a
caller-supplied buffer, so this function itself stays allocation-free regardless of who
calls it or from which task.

### Step 3 — `EFP1Message` construction/assignment

With fixed-array storage, the default constructor no longer needs `push_back()` loops
(no vector growth at all — remove the `reserve()` calls added this session, they become
moot) and `operator=` becomes a fixed number of bounded `strncpy`s instead of 41
`std::string` copy-assignments. Both become genuinely allocation-free.

### Step 4 — `CyranoHandler`'s cache members

`m_CachedCyranoString`, `m_CachedNextCyrano`, `m_CachedPrevCyrano` (currently
`std::string`) become fixed `char[N]` members, `N` sized from Step 0's totals. This
removes the last heap involvement in the whole Cyrano push path — the cache becomes
genuinely pre-built and zero-allocation at both build time (this fix) and send time
(already true from the earlier push-based-cache work).

### Step 5 — Concurrency (needs your explicit go-ahead, not bundled silently)

Confirmed this session: `PushCachedStatusToCyrano()` → `CyranoHandler::updateCachedStatus()`
is reachable from at least three independent FreeRTOS tasks (`StateMachineHandler` via
FSM `notify()`, `uiEventTask` via button presses, `async_udp` via DISP receipt) and
`CyranoHandler`'s cache members have **no mutex today**, with `std::string` or without.
Switching to fixed `char[N]` doesn't make this worse — corrupted `std::string` internals
from a lost race and corrupted `char[N]` contents from a lost race are both bad — but it
doesn't fix it either, and a `char[N]` race is silent data corruption in what gets sent
to a real CMS (wrong score/fencer data), not a crash you'd notice.

Since `CyranoHandler` is one of the classes CLAUDE.md protects from unrequested field
additions: **this plan proposes adding one `SemaphoreHandle_t` mutex to `CyranoHandler`**,
taken around the read-modify-write of the three cache buffers in
`updateCachedStatus()`/`RebuildCachedStrings()`, mirroring `Opp2Handler::m_StateMutex`'s
existing pattern exactly. Flagging it here explicitly rather than folding it into "just a
storage type change" — say if you'd rather handle this differently (e.g. you may already
know something about actual call timing that makes it a non-issue in practice).

### Step 6 — Verification protocol

- Rebuild `docs/HEAP_MEMORY_ANALYSIS.md`'s two stress patterns (60× rapid `toggle_timer`,
  20 rounds of concurrent mixed load) — expect zero crashes, matching this session's
  post-`reserve()` result. **Done** — 400 sequential `toggle_timer` cycles on real
  hardware, zero crashes, heap flat throughout (this branch's full implementation, not
  just `reserve()`).
- **New, stricter than this session's testing:** a soak test of several hundred to a
  few thousand state-change cycles (not just 20–60), sampling `/heap`'s `largest_block`
  every N cycles. This session's `reserve()` fix looked completely clean after 20 rounds
  and still crashed later under continued ordinary use — a short burst test gave false
  confidence once already; don't repeat that mistake for this fix. **Done** — see above.
- EFP1 roundtrip check (CLAUDE.md invariant #7): simulate or send a real DISP with all 41
  fields populated including edge-length values (a 63-char name, a name at exactly the
  boundary, an empty field), confirm every field reappears correctly in the resulting
  INFO message. **Done, but not the way originally planned** — a live-device UDP
  roundtrip turned out to be blocked by an unrelated, unresolved networking issue (see
  `docs/CYRANO_UDP_LISTENER_MYSTERY.md` — parked, not this branch's problem). Verified
  instead with a standalone host-side test (`test/host_efp1/`) that compiles
  `EFP1Message.cpp` directly and round-trips all 41 fields including boundary-length
  values, run under ASan/UBSan plus a 20,000-iteration random fuzz pass. **This is how
  the leading-`|`-skip bug below was actually found** — arguably better coverage than
  the originally-planned live test would have given, since a live test would have shown
  "the CMS handshake looks wrong somehow" rather than pointing at the exact parsing bug.
- If a Cyrano CMS (real or simulated) is available, run an actual DISP→INFO→ACK cycle
  under the soak test, not just a synthetic curl-based one. **Still open** — genuinely
  needs a live device once the parked networking issue is resolved; the standalone test
  can't exercise the real UDP/MQTT transport layer.

**Bug found and fixed via the standalone test, not anticipated when this plan was
written:** `EFP1Message(const char*)`'s parsing constructor was missing a leading-`|`
skip when transitioning from the general area into the right-fencer area, and again into
the left-fencer area (the wire format is `...|%|firstField|...` — the `|` right after
`%` belongs to the next area, not a separator to discard). Without it, every field in
both fencer areas silently held the *previous* field's value, with the first field
(`RightFencerId`) ending up empty. Fixed in `src/EFP1Message.cpp`; the standalone test
now checks for regressions on every run.

---

## Branch 2: `fix/opp2-json-heap`

### Step 0 — Pick a direction (recommendation below, not decided for you)

Searched for ESP32-suitable JSON alternatives beyond ArduinoJson before finalizing this
table (not done when the plan was first drafted). Findings: ArduinoJson v7's custom
`Allocator` support (Option B) is an officially documented feature, not a workaround —
confirmed via ArduinoJson's own site, which explicitly shows overriding
`allocate`/`deallocate`/`reallocate` for cases like external PSRAM; the same interface
adapts directly to a fixed static-buffer allocator. **RaftJson** (on-demand parsing,
genuinely zero heap) surfaced as a strong candidate too, but it parses — extracts values
from a JSON document without materializing a tree — rather than builds/serializes one, so
it's a candidate for `opp2_deserialize.h` (incoming DISP/CMS messages) alongside whatever
gets chosen below for `opp2_serialize.h`, not a replacement for it. No option found beats
hand-rolled `snprintf` for *building* small fixed-schema JSON with zero heap — which is
already Option C.

| Option | Where the work happens | Effort | Notes |
|---|---|---|---|
| **A. Pin ArduinoJson to v6.x** | This repo only — one line in `platformio.ini` | Smallest | OPP2 library only uses old-style `StaticJsonDocument<N>` API (confirmed, no v7-only features used anywhere in `opp2_serialize.h`/`opp2_deserialize.h`) — very likely a clean drop-in with **zero changes to opp2-library**. Downside: v6 is EOL upstream. |
| **B. Custom fixed-buffer `Allocator` for v7** | `opp2-library` (separate repo) | Medium | Needs `opp2_serialize.h`'s `serialize()` functions to accept/use a custom `Allocator*` instead of the default. Officially documented ArduinoJson v7 pattern (confirmed), just needs a static-buffer implementation instead of their PSRAM example. Cross-repo change. |
| **C. Hand-rolled `snprintf` serialization, drop ArduinoJson for these messages** | `opp2-library` (separate repo) | Largest (12 message types) | Zero heap, zero library dependency for this path. `WebRemoteHandler::handleState()` (this session) already proves the pattern works. Cross-repo change, biggest diff. |
| **D. RaftJson for the deserialize side only** | `opp2-library` (separate repo) | Small, additive | Zero-heap on-demand parsing, purpose-built for exactly this ("extract values from fixed-schema messages"). Doesn't touch `opp2_serialize.h` at all — a complement to A/B/C, not an alternative to them. Worth evaluating alongside whichever of A/B/C is chosen, since `opp2_deserialize.h` has the identical `StaticJsonDocument` heap issue on the incoming side (DISP/CMS message parsing) that this plan hasn't otherwise addressed. |

**My recommendation is A** — it's the only option that doesn't require coordinating a
change across two repositories, it's a one-line change to test, and it's trivially
reversible if it doesn't pan out. The EOL-library concern is real but modest for a
vendored, rarely-updated dependency doing a narrow, stable job (fixed JSON schemas that
aren't changing). B and C stay on the table if you'd rather not carry a pinned-EOL
dependency, or if `opp2-library` needs the memory discipline for other consumers besides
this device anyway (it's a shared library across the OpenPiste platform, so a proper fix
there benefits other devices too — a real argument for B or C that A doesn't address).
Worth deciding D independently of A/B/C, since it addresses a gap (deserialize-side heap
use) that this plan otherwise leaves open.

This is a real decision for you, not something I should default silently — happy to
proceed with A as soon as you confirm, or discuss B/C further first.

### Step 1 — Implementation (once direction is chosen) — **Done, Option A**

Chose A per Piet's explicit call (2026-08-13): pin now, revisit B/C later rather than
decide on them today.

- `platformio.ini`: `bblanchon/ArduinoJson @ ^7.0.0` → `^6.21.0`.
- **Not actually zero opp2-library changes, unlike this plan originally claimed.**
  Rebuilding surfaced one real compile break: `opp2_serialize.h`'s `VideoReview`
  serializer used `JsonArray::add<JsonObject>()`, a v7-only no-arg template form (the
  earlier grep for `StaticJsonDocument`/`JsonDocument`/`Allocator`/`shrinkToFit` didn't
  cover this — it's a different part of the v7 API surface). Confirmed `VideoReview` is
  never called anywhere in this project (matches `CLAUDE.md`'s own
  "❌ Not started: Medical and VideoReview publishing"), so this was a dead-code-only
  compile break, not a behavior risk. Fixed in `opp2-library` itself
  (`~/esp-idfProjects/opp2-library`, branch `fix/videoreview-v6-compat`, committed there
  — not merged) by swapping to `createNestedObject()`, confirmed present in **both** v6
  and v7 (deprecated-but-functional in v7, per ArduinoJson's own
  `compatibility.hpp`) — a genuinely version-agnostic fix, not a v6-only shim. This
  repo's `platformio.ini` temporarily points at that local branch via
  `symlink:///home/piet/esp-idfProjects/opp2-library` until the fix is merged/tagged
  upstream and a real released version can be pinned again.
- `TierAProvisioning.cpp`'s two real `JsonDocument` (v7-only name) uses, exactly as
  anticipated below: ported to `DynamicJsonDocument(N)` (heap, not
  `StaticJsonDocument<N>`/stack) — this path carries a CSR and, on response, two PEM
  certificates (a few KB), and runs inside the MQTT client's own event-handler task, so
  sizing that on the stack isn't a safe default regardless of the task's actual budget.
  Fires at most once per device lifetime (or on cert renewal) — a rare one-time
  allocation, unlike the OPP2/Cyrano publish paths this whole pin exists to fix, which
  fire on every state change.

### Step 2 — Verification protocol — **Done**

- Confirm OPP2 JSON payloads are byte-identical (or at least schema-identical — same
  fields, same types) before/after, for every message type in `opp2_serialize.h` —
  spot-check against `docs/level2.md`'s field definitions, since v6→v7 (or
  ArduinoJson→hand-rolled) could subtly change float formatting, escaping, or key
  ordering even when semantically equivalent. **Done** — captured a real boot +
  MQTT-boot-recovery + `Publish*()` cycle on hardware with logging temporarily enabled;
  JSON payloads (e.g. the `Match` publish) are byte-for-byte identical in schema to the
  v7 capture from earlier the same session.
- Same soak-test discipline as Branch 1: hundreds of cycles of state changes that
  trigger OPP2 publishes, sampling `/heap` periodically, not just a short burst.
  **Partially superseded** — see below; the actual verification done here is stronger
  than a heap-delta soak test for this specific question.
- Confirm `mqttClient.isConnected()` gating still short-circuits correctly (i.e. the fix
  doesn't accidentally publish when disconnected, or vice versa skip when connected).
  **Done** — unchanged code path, not touched by this fix; confirmed still present by
  reading `PublishClock()` etc., not re-derived.

**Additional verification beyond what this plan called for:** a standalone host-side
test (`test/host_opp2_json/`) that overrides global `operator new`/`delete` to count
allocations around real calls to `OPP2::Serializer::serialize()` for every message type
this project actually publishes. This directly proves the zero-heap claim rather than
inferring it from documentation or from a heap-delta soak test (which can be confounded
by concurrent activity) — **zero allocations across all 11 message types actually
used**, `VideoReview` excluded (unused, see above). Arguably stronger evidence for this
specific question than the live-hardware soak test the plan originally called for, in
the same way the EFP1 branch's standalone test caught something a live test would have
struggled to pin down.

---

## Integration testing (after all three branches pass individually)

Merge all three into a combined test branch (not `main` yet) and re-run the soak test
with everything firing together — real operation always has the OPP2 and Cyrano paths
active simultaneously (every state change triggers both an OPP2 publish and a Cyrano
cache push), so this is the first point where their combined heap behavior under
sustained load actually gets exercised. This is the test that would have caught
tonight's Crash 4 (repeat crash at an already-`reserve()`-fixed site, from cumulative
cross-path fragmentation) had it existed before that fix shipped — worth treating as the
real gate before merging to `main`, not a formality. Branch 0 doesn't need to be part of
this specific soak test's *logic* (it doesn't touch the publish paths), but merging it
first means the other two get soak-tested with realistic headroom from the start rather
than the artificially tighter margin that exists on `main` today.

## Rollback

`main` currently has this session's `reserve()` fix and the request-concurrency guard —
real, verified improvements, not a broken baseline. All three branches are additive risk
reduction on top of a working state, not a from-scratch rewrite of something fragile.
If any branch stalls or introduces a regression, `main` is a safe fallback with no loss
of tonight's progress.
