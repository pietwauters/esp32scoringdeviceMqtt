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

*Last updated: May 17, 2026*
