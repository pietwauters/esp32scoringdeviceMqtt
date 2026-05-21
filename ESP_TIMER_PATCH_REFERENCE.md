# ESP Timer Patch Reference

This document shows the exact change that `patch_esp_timer.py` makes to ESP-IDF's `esp_timer.c` file.

## File Location

**Linux/Mac:** `~/.platformio/packages/framework-espidf/components/esp_timer/src/esp_timer.c`  
**Windows:** `C:\Users\<YourUser>\.platformio\packages\framework-espidf\components\esp_timer\src\esp_timer.c`

## The Change

### BEFORE (Unpatched)

The original ESP-IDF code hardcodes the core number (0 or 1) in the `xTaskCreatePinnedToCore` call:

```c
static void esp_timer_impl_init_generic(intr_handler_t alarm_handler)
{
    // ... other initialization code ...
    
    int ret = xTaskCreatePinnedToCore(&timer_task, "esp_timer",
            ESP_TASK_TIMER_STACK, NULL, ESP_TASK_TIMER_PRIO, &s_timer_task, 0);
                                                                            ^^^
                                                                    HARDCODED CORE
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create timer task");
        abort();
    }
}
```

The `0` at the end means the timer task always runs on Core 0. Different ESP-IDF versions may use:
- `0` or `1` (explicit core number)
- `tskNO_AFFINITY` (no affinity)
- `CONFIG_ESP_TIMER_TASK_AFFINITY` (config macro)

### AFTER (Patched)

The patched version adds a compile-time check and uses a configurable macro:

```c
static void esp_timer_impl_init_generic(intr_handler_t alarm_handler)
{
    // ... other initialization code ...
    
    #ifndef ESP_TIMER_TASK_CORE
    #error "ESP_TIMER_TASK_CORE is not defined. Add -DESP_TIMER_TASK_CORE=0 or 1 to build_flags in platformio.ini"
    #endif
    int ret = xTaskCreatePinnedToCore(&timer_task, "esp_timer",
            ESP_TASK_TIMER_STACK, NULL, ESP_TASK_TIMER_PRIO, &s_timer_task, ESP_TIMER_TASK_CORE);
                                                                            ^^^^^^^^^^^^^^^^^^^
                                                                        CONFIGURABLE VIA BUILD FLAG
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create timer task");
        abort();
    }
}
```

## Summary of Changes

1. **Added compile guard** (`#ifndef ESP_TIMER_TASK_CORE ... #endif`)
   - Ensures the macro is defined
   - Provides clear error message if missing
   
2. **Replaced hardcoded core** with `ESP_TIMER_TASK_CORE` macro
   - Value comes from `-DESP_TIMER_TASK_CORE=1` in `platformio.ini`
   - Can be changed without modifying framework code

## To View the Actual File

```bash
# Linux/Mac
cat ~/.platformio/packages/framework-espidf/components/esp_timer/src/esp_timer.c | grep -A 5 -B 5 "xTaskCreatePinnedToCore.*s_timer_task"

# Or open in your editor
code ~/.platformio/packages/framework-espidf/components/esp_timer/src/esp_timer.c
```

```powershell
# Windows PowerShell
code "$env:USERPROFILE\.platformio\packages\framework-espidf\components\esp_timer\src\esp_timer.c"
```

The file is around 500-600 lines. Search for `xTaskCreatePinnedToCore` and look for the one that has `&s_timer_task` as a parameter.
