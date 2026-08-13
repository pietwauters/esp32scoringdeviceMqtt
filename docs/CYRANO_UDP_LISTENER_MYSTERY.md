# Cyrano UDP Listener — Parked Investigation (2026-08-13)

**Status:** Unresolved. Parked, not abandoned — this document exists so the next
session (or the next person) doesn't have to re-derive everything below from scratch.
Branch: `fix/cyrano-udp-listener-retry` (off `main`, not `fix/efp1-fixed-buffers` —
deliberately separate, see `docs/HEAP_FIX_IMPLEMENTATION_PLAN.md`'s branch-isolation
rationale).

## What triggered this

While trying to run a live EFP1 roundtrip test (send a real Cyrano `HELLO`/`DISP` over
UDP, verify the device's `INFO` response), no `HELLO` ever got a reply — not even an
initial "software is now known" acknowledgment. Investigating *why* turned into this
whole thread.

## What's confirmed true, in order of discovery

1. **UDP port 50101 (`CyranoPort`) is unreachable from outside the device.** Verified
   directly at the socket level, repeatedly, across multiple fresh boots: a "connected"
   UDP socket (`socket.connect()`) surfaces `ECONNREFUSED` on send/recv, which only
   happens on a real ICMP port-unreachable response — i.e. nothing is bound to that port
   from the network's point of view. This is not a guess or a timeout; it's a definitive
   signal from the device's own IP stack.

2. **A real, separate bug exists in `CheckConnection()`** (`CyranoHandler.cpp`) and has
   been fixed on this branch: `budpCyranoConnected` was set `true` unconditionally after
   attempting `CyranoHandlerudpRcv.listen(CyranoPort)`, regardless of whether `listen()`
   actually returned `true`. Since the retry guard is `if (!budpCyranoConnected)`, a
   single failed attempt (e.g. a timing race right after boot) would permanently block
   all future retries for the rest of that boot session — matching the code's own
   comment ("Somehow we should call this only once... keep on trying for ever") admitting
   the intended behavior wasn't what was implemented. **Fixed:** `budpCyranoConnected`
   (and `bCyranoConnected`) are now only set `true` inside the success branch of the
   `if (CyranoHandlerudpRcv.listen(CyranoPort))` check.

3. **The fix did not resolve the symptom.** Added a rate-limited diagnostic
   (`ESP_LOGW` every 3s logging `bWifiConnected`/`bCyranoConnected`/
   `budpCyranoConnected`/`bmqttCyranoConnected`/`WiFi.status()`) and confirmed on real
   hardware: `budp=1` — `listen()` genuinely returns `true`. The port is still externally
   unreachable regardless.

4. **Logging was compiled out project-wide by default**
   (`CORE_DEBUG_LEVEL=0`/`LOG_LOCAL_LEVEL=0` in `platformio.ini`), which is *why* none of
   tonight's earlier `ESP_LOGI`/`ESP_LOGW` diagnostics (including this investigation's
   first two attempts) ever showed up on serial, no matter how the capture tooling was
   set up. Only the ESP-IDF panic handler's direct-to-UART crash output and the ROM
   bootloader banner bypass the app's log level, which is why *those* were visible all
   night while routine logs weren't. This was temporarily bumped to level 3 to get the
   diagnostic in point 3, then reverted back to 0/0 before parking this — production
   builds should keep logging off by design; don't ship this branch with it enabled.

5. **The AP/STA dual-interface theory was raised and then ruled out.** The device runs
   `WiFi.mode(WIFI_MODE_APSTA)` (SoftAP + Station simultaneously), and the hypothesis was
   that `AsyncUDP::listen(port)` might bind in a way that only serves one interface,
   while the debugging session's UDP test only exercises the other. This does not hold
   up:
   - The SoftAP has no static IP configured, so it uses ESP32's default `192.168.4.1` —
     a completely different subnet from the STA IP (`10.154.1.103`) used throughout
     testing. The test machine has no route to `192.168.4.x` at all, so it was only ever
     capable of testing the STA side.
   - The captured "Cyrano Listening on IP: ..." log line's IP fragment (partially
     truncated by a capture-timing race, but ending in `...1.103`) matches the STA IP
     exactly — confirming `WiFi.localIP()` (STA-specific in the Arduino-ESP32 API,
     distinct from `WiFi.softAPIP()`) is what gets logged, and that this **is** the
     interface being tested.
   - Read the actual vendored library source
     (`framework-arduinoespressif32/libraries/AsyncUDP/src/AsyncUDP.cpp`):
     `AsyncUDP::listen(uint16_t port)` calls `listen(IP_ANY_TYPE, port)`, which binds via
     lwIP's interface-agnostic "any" address type. A PCB bound this way is not scoped to
     a single interface at the lwIP level — it should receive packets arriving on
     *either* interface. This directly contradicts the "wrong interface" theory.

## What remains unexplained

`AsyncUDP::listen()`'s only failure path is `_udp_bind(_pcb, addr, port) != ERR_OK`
returning `false`. `budp=1` on real hardware means this call returned `ERR_OK` — a
genuinely successful, interface-agnostic lwIP UDP PCB bind, by the library's own
accounting. And yet packets aimed at that exact port from a real network peer are met
with ICMP port-unreachable, which is *lwIP itself* saying no PCB is registered for that
port. These two facts are in direct contradiction and neither obviously explains the
other from anything read or tested so far.

Live remote-control interaction (physical NEXT/PREV button presses, monitored live over
serial) confirmed the *outbound* half of the Cyrano path works — `SendInfoMessage()`,
`EVENT_CYRANO_SEND_NEXT`/`PREV` all fire correctly in response to real button presses.
This says nothing about the inbound listener, since none of it requires an external
packet to actually reach the device.

## What would be needed to actually resolve this

Not attempted, since each is a bigger step than tonight's scope:

- Instrument *inside* the vendored `AsyncUDP.cpp`/lwIP itself (not just the application
  layer) to see the actual PCB list state and confirm/deny whether the `_pcb` from
  `listen()` is really the one lwIP's UDP demux code is checking against when a real
  packet arrives — e.g. a temporary log in lwIP's `udp_input()` on port-unreachable
  generation, or dumping `udp_pcbs` (lwIP's global PCB list) after a confirmed-successful
  `listen()`.
- Check for a **second, separate UDP listener elsewhere in the codebase** that might be
  contending for the same port or interfering with PCB registration in a way that leaves
  the *first* PCB orphaned/unreachable despite still reporting as bound — worth a
  project-wide grep for other `AsyncUDP`/`listen(` usages, which wasn't done in this
  session's investigation.
- Test from a genuinely different network path (e.g. a phone on the same WiFi, not this
  dev machine) to rule out anything specific to the test machine's network stack/driver,
  even though the ICMP-refusal evidence points squarely at the device, not the path to
  it.
- Consider whether `CONFIG_LWIP_MAX_UDP_PCBS` (16, per `sdkconfig.esp32dev`) or some
  other lwIP resource limit is silently exhausted from the device's long uptime and many
  MQTT/AsyncTCP connections tonight, causing a bind that reports success but doesn't
  actually register — untested hypothesis, not ruled in or out.

## What's on this branch right now

- `src/CyranoHandler.cpp`: the `budpCyranoConnected`-only-set-on-success fix (real,
  keep) plus a permanent (non-spammy, failure-only) `ESP_LOGW` when `listen()` fails —
  both good regardless of whether they explain tonight's specific symptom.
- `platformio.ini`: log level reverted back to 0/0 (production default) — do not ship
  with it bumped; if picking this investigation back up, re-bump `CORE_DEBUG_LEVEL`/
  `LOG_LOCAL_LEVEL` to 3 temporarily and revert again before merging.
- No diagnostic scaffolding left in the tree beyond the one permanent failure-log line
  above — the rate-limited flag-state dump used to gather the evidence in this document
  was removed once its findings were captured here.
