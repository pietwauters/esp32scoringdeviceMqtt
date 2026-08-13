# OPP2/ArduinoJson host-side heap test

Standalone, no-device test proving `OPP2::Serializer::serialize()` (in the `opp2-library`
dependency, `opp2_serialize.h`) genuinely does not allocate on the heap under the
ArduinoJson v6 pin (`docs/HEAP_FIX_IMPLEMENTATION_PLAN.md`, Branch 2 / Option A) — not
assumed from documentation, proven directly by overriding global `operator new`/`delete`
to count allocations around real calls to every message type this project actually
publishes.

Added 2026-08-13 alongside the v6 pin. Result: **zero allocations across all 11 message
types** (`Lights`, `Clock`, `Score`, `Connection`, `ApparatusStateMsg`, `Fencers`,
`Match`, `UW2F`, `BladeContact`, `Medical`, `Control`). `VideoReview` deliberately
excluded — not called anywhere in this project (matches `CLAUDE.md`'s own
"❌ Not started" note), though the v6-compat fix that makes it *compile* at all
(`opp2_serialize.h`'s `createNestedObject()` vs the v7-only `add<JsonObject>()`) is
exercised just by this file linking successfully, since it's in the same translation
unit.

Confirmed separately on real hardware: full boot + MQTT boot-recovery + `Publish*()`
cycle with logging temporarily enabled, zero crashes, JSON payload schema byte-for-byte
identical to what ArduinoJson v7 produced (no protocol-visible behavior change).

## Run it

```sh
g++ -std=c++14 -g -O0 \
  -I <path-to-opp2-library>/src \
  -I <path-to-project>/.pio/libdeps/esp32dev/ArduinoJson@6.21.6/src \
  test/host_opp2_json/test_opp2_json_heap.cpp -o /tmp/test_opp2_json_heap
/tmp/test_opp2_json_heap
```

Adjust the `opp2-library` path to wherever it's checked out locally (this repo depends
on it via a `symlink://` `lib_deps` entry while its own v6-compat fix is unmerged --
see the comment above that line in `platformio.ini`), and the ArduinoJson path to match
whatever version is actually pinned there if it changes.
