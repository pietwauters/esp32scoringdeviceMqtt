# Technical Notes

## ESP-IDF Timer Task Core Assignment

### The Problem

ESP-IDF 4.4 hardcodes the `esp_timer` task to a specific core (typically core 0). For this fencing scoring device, high-frequency sensor sampling requires the timer task to run on a dedicated core, isolated from WiFi and network processing which can introduce latency and jitter.

The standard ESP-IDF does not expose timer task core affinity via `sdkconfig`, making it impossible to change this behavior without modifying framework source code.

### The Solution

This project uses an **automated patching system** that modifies ESP-IDF's `esp_timer.c` at build time:

1. **Patch script**: [`patch_esp_timer.py`](patch_esp_timer.py) runs automatically before every build
2. **Build flag**: `-DESP_TIMER_TASK_CORE=1` in [`platformio.ini`](platformio.ini) controls which core the timer runs on
3. **Code coordination**: [`src/RTOSSettings.h`](src/RTOSSettings.h) centralizes all task core assignments

### How It Works

#### The Patch

The script modifies ESP-IDF's `esp_timer.c` to:

**Before (unpatched):**
```c
int ret = xTaskCreatePinnedToCore(&timer_task, "esp_timer",
        ESP_TASK_TIMER_STACK, NULL, ESP_TASK_TIMER_PRIO, &s_timer_task, 0);
```

**After (patched):**
```c
#ifndef ESP_TIMER_TASK_CORE
#error "ESP_TIMER_TASK_CORE is not defined. Add -DESP_TIMER_TASK_CORE=0 or 1 to build_flags in platformio.ini"
#endif
int ret = xTaskCreatePinnedToCore(&timer_task, "esp_timer",
        ESP_TASK_TIMER_STACK, NULL, ESP_TASK_TIMER_PRIO, &s_timer_task, ESP_TIMER_TASK_CORE);
```

Changes:
1. Replace hardcoded `0` or `1` with `ESP_TIMER_TASK_CORE` macro
2. Add compile-time check to ensure the macro is defined

#### Automatic Application

The patch is applied transparently via PlatformIO's `extra_scripts` mechanism:

```ini
extra_scripts = pre:patch_esp_timer.py   # Runs first - applies ESP-IDF patch
                pre:generate_cert.py      # Generates TLS certificates
                extra_script.py           # Post-build OTA packaging
```

The script is **idempotent** — it checks if the file is already patched and only applies the patch if needed. This means:
- ✓ Safe to run multiple times
- ✓ Works after PlatformIO package updates (will re-patch automatically)
- ✓ Works for all contributors after a fresh `git clone`
- ✓ No manual intervention required

### Core Assignment Strategy

Current configuration (see [`src/RTOSSettings.h`](src/RTOSSettings.h)):

```
Core 0 (PRO_CPU):   WiFi, lwIP, state machine, display, AutoRef
Core 1 (APP_CPU):   Sensor (ESP_TIMER_TASK only)
```

**Rationale:**
- Core 1 is dedicated exclusively to the high-frequency sensor task
- WiFi and network processing on Core 0 cannot introduce jitter into sensor sampling
- All application tasks run on Core 0 to keep Core 1 clean

**To change the sensor core:**
1. Edit `-DESP_TIMER_TASK_CORE=1` in [`platformio.ini`](platformio.ini) (change `1` to `0`)
2. Rebuild — the patch script will handle everything automatically

### For Contributors

**Normal workflow:** Just build — the patch happens automatically.

**If you see build errors** about `ESP_TIMER_TASK_CORE`:
1. Check the build output for messages from `patch_esp_timer.py`
2. The script will report if it cannot locate or patch `esp_timer.c`
3. If patching fails, the error message includes diagnostic information

**If ESP-IDF is updated** (e.g., PlatformIO upgrades framework packages):
- The patch script will automatically detect the unpatched new version
- It will re-apply the patch before the next build
- No action required

### Troubleshooting: Patch Fails to Apply

If you see "❌ Could not find xTaskCreatePinnedToCore call to patch":

The automatic patch script tries multiple patterns to handle different ESP-IDF versions, but if your version has significantly different code structure, you'll need to manually patch:

#### Step 1: Locate the file

**Windows:** `C:\Users\<YourUser>\.platformio\packages\framework-espidf\components\esp_timer\src\esp_timer.c`
**Linux/Mac:** `~/.platformio/packages/framework-espidf/components/esp_timer/src/esp_timer.c`

#### Step 2: Find the xTaskCreatePinnedToCore call

Search for the function that creates the timer task. Look for:
- `xTaskCreatePinnedToCore` AND
- `s_timer_task` (the task handle variable)

Example (the exact format varies by version):
```c
int ret = xTaskCreatePinnedToCore(&timer_task, "esp_timer",
        ESP_TASK_TIMER_STACK, NULL, ESP_TASK_TIMER_PRIO, &s_timer_task, 0);
```

#### Step 3: Apply the manual patch

1. **Add the compile guard** (near the top of the file, after includes):
```c
#ifndef ESP_TIMER_TASK_CORE
#error "ESP_TIMER_TASK_CORE is not defined. Add -DESP_TIMER_TASK_CORE=0 or 1 to build_flags in platformio.ini"
#endif
```

2. **Replace the last parameter** (the core number) with `ESP_TIMER_TASK_CORE`:

Before:
```c
xTaskCreatePinnedToCore(&timer_task, "esp_timer",
        ESP_TASK_TIMER_STACK, NULL, ESP_TASK_TIMER_PRIO, &s_timer_task, 0);
                                                                        ^^^
```

After:
```c
xTaskCreatePinnedToCore(&timer_task, "esp_timer",
        ESP_TASK_TIMER_STACK, NULL, ESP_TASK_TIMER_PRIO, &s_timer_task, ESP_TIMER_TASK_CORE);
                                                                        ^^^^^^^^^^^^^^^^^^^
```

#### Step 4: Report the issue

If you needed to manually patch, please:
1. Note your ESP-IDF version (`pio pkg list` or check the package directory name)
2. Copy the **original** xTaskCreatePinnedToCore line you found
3. Open an issue or submit a PR to add a new pattern to `patch_esp_timer.py`

This helps improve the auto-patcher for future users!

### Why Not a Framework Fork?

Alternative approaches considered:

| Approach | Pros | Cons |
|----------|------|------|
| **Automated patch** (current) | ✓ Works with standard PlatformIO<br>✓ Transparent to users<br>✓ Survives framework updates | – Patches framework code at build time |
| Fork ESP-IDF | ✓ Clean permanent solution | – Requires custom platform packages<br>– More complex setup<br>– Framework updates require manual merging |
| Manual patching | ✓ Simple | – Every contributor must patch manually<br>– Easy to forget<br>– Breaks after framework updates |
| Document only | ✓ No automation | – High barrier to entry for contributors<br>– Error-prone |

**Decision:** Automated patching provides the best balance of transparency, ease of use, and maintainability.

---

## Tier A (mTLS) provisioning: TLS hostname/CN check disabled

*Decided 2026-07-14 — see `src/TierAProvisioning.h`/`.cpp`, `src/AtlasAsyncMqttClient.cpp`.*

### The Problem

This device connects to the MQTT broker using a **resolved IP address**
(`CyranoHandler::Begin()` → `MDNSResolver::resolveHostname()` returns an `IPAddress`,
stored as `AtlasAsyncMqttClient::m_host`), never a hostname string. The broker's TLS
certificate (Atlas's `scripts/generate-tls-cert.sh`) only lists `openpiste.local`,
`localhost`, and `127.0.0.1` in its Subject Alternative Name — never an arbitrary LAN
IP. This mismatch was invisible until Tier A (`docs/level2.md` §30.5) added the first
certificate-verified TLS connection this device has ever made — the pre-existing
anonymous connection is plain TCP, no certificate check at all.

Confirmed on real hardware: without a fix, every mTLS connection attempt fails the
handshake unconditionally with `Failed to verify peer certificate!` (mbedtls `-0x2700`,
`MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`) — regardless of whether the certificate is
otherwise entirely valid and correctly signed.

### The Solution

`mqtt_cfg.skip_cert_common_name_check = true` in `AtlasAsyncMqttClient::begin()`
(only when `m_tlsEnabled`). This disables **only** the hostname/CN-vs-SAN match — the
certificate **chain** is still fully verified against `m_ca_cert` (Atlas's own
locally-generated CA; nothing else is trusted by this device). A certificate not
signed by that CA still cannot pass the handshake.

### Why not fix the root cause instead?

The textbook-correct fix is connecting via the `openpiste.local` hostname string
instead of a pre-resolved IP, so the certificate's SAN would actually match what's
being verified. Not done here because that touches `CyranoHandler::Begin()`'s shared
connection-setup/address-resolution logic, which the existing anonymous/legacy Cyrano
path also depends on — a bigger-blast-radius change than this narrower one, for a
project whose stated trust model (`docs/level2.md` §30.1) is already
"physically-secured local network... not bank-grade," not defense against an
on-path attacker who has already compromised the venue LAN.

**Revisit if:** the connection-setup code is ever refactored to support hostname-based
connections generally, or if a deployment's threat model changes such that this
tradeoff is no longer acceptable.

---

*Last updated: May 17, 2026*
