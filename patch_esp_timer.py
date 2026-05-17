"""
Automatic ESP Timer Core Patch Script
======================================

This script patches ESP-IDF's esp_timer.c to allow configurable core assignment
for the ESP_TIMER_TASK via the ESP_TIMER_TASK_CORE build flag.

The patch is necessary because ESP-IDF 4.4 hardcodes the timer task to a specific
core, but this project needs the flexibility to assign it to either core for
optimal sensor performance.

The script:
1. Locates esp_timer.c in the PlatformIO packages directory
2. Checks if the file is already patched (idempotent - safe to run multiple times)
3. If unpatched, applies the patch to:
   - Add a compile-time check for ESP_TIMER_TASK_CORE
   - Replace the hardcoded core value with the ESP_TIMER_TASK_CORE macro

This runs automatically before every build, so developers don't need to manually
patch their ESP-IDF installation after cloning the repository.
"""

import os
import re
from SCons.Script import Import

Import("env")

def find_esp_timer_c():
    """Locate esp_timer.c in the PlatformIO packages directory."""
    platform = env.PioPlatform()
    framework_dir = platform.get_package_dir("framework-espidf")
    
    if not framework_dir:
        print("⚠️  Could not locate ESP-IDF framework directory")
        return None
    
    esp_timer_path = os.path.join(framework_dir, "components", "esp_timer", "src", "esp_timer.c")
    
    if not os.path.exists(esp_timer_path):
        print(f"⚠️  esp_timer.c not found at: {esp_timer_path}")
        return None
    
    return esp_timer_path


def is_already_patched(file_path):
    """Check if esp_timer.c is already patched."""
    with open(file_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Look for the patch signature - the #ifndef ESP_TIMER_TASK_CORE check
    if '#ifndef ESP_TIMER_TASK_CORE' in content:
        return True
    
    # Also check if ESP_TIMER_TASK_CORE is used as the core parameter
    if re.search(r'xTaskCreatePinnedToCore\([^)]*ESP_TIMER_TASK_CORE\)', content, re.DOTALL):
        return True
    
    return False


def apply_patch(file_path):
    """Apply the ESP_TIMER_TASK_CORE patch to esp_timer.c."""
    with open(file_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Try multiple patterns to handle different ESP-IDF versions
    patterns = [
        # Pattern 1: Multi-line format with explicit variable assignment
        # int ret = xTaskCreatePinnedToCore(&timer_task, "esp_timer",
        #         ESP_TASK_TIMER_STACK, NULL, ESP_TASK_TIMER_PRIO, &s_timer_task, 0);
        (r'([ \t]*(?:int|BaseType_t)\s+\w+\s*=\s*xTaskCreatePinnedToCore\s*\(\s*&timer_task\s*,\s*"esp_timer"\s*,\s*\n?\s*ESP_TASK_TIMER_STACK\s*,\s*NULL\s*,\s*ESP_TASK_TIMER_PRIO\s*,\s*&s_timer_task\s*,\s*)([01]|tskNO_AFFINITY|CONFIG_\w+)(\s*\)\s*;)',
         'multiline_explicit'),
        
        # Pattern 2: Single line format
        # xTaskCreatePinnedToCore(&timer_task, "esp_timer", ESP_TASK_TIMER_STACK, NULL, ESP_TASK_TIMER_PRIO, &s_timer_task, 0);
        (r'(xTaskCreatePinnedToCore\s*\(\s*&timer_task\s*,\s*"esp_timer"\s*,\s*ESP_TASK_TIMER_STACK\s*,\s*NULL\s*,\s*ESP_TASK_TIMER_PRIO\s*,\s*&s_timer_task\s*,\s*)([01]|tskNO_AFFINITY|CONFIG_\w+)(\s*\)\s*;)',
         'singleline'),
        
        # Pattern 3: More flexible - just look for s_timer_task with a core parameter
        # Matches any xTaskCreatePinnedToCore with s_timer_task and a simple core value
        (r'(xTaskCreatePinnedToCore\s*\([^)]*&s_timer_task\s*,\s*)([01]|tskNO_AFFINITY|CONFIG_\w+)(\s*\)\s*;)',
         'flexible'),
        
        # Pattern 4: Even more flexible - capture everything between function name and last parameter
        (r'(xTaskCreatePinnedToCore\s*\([^,]+,\s*[^,]+,\s*[^,]+,\s*[^,]+,\s*[^,]+,\s*&s_timer_task\s*,\s*)([01]|tskNO_AFFINITY|CONFIG_\w+)(\s*\))',
         'very_flexible'),
    ]
    
    match = None
    pattern_name = None
    
    # Try each pattern
    for pattern, name in patterns:
        match = re.search(pattern, content, re.MULTILINE)
        if match:
            pattern_name = name
            print(f"   Found match using pattern: {name}")
            break
    
    if not match:
        print("❌ Could not find xTaskCreatePinnedToCore call to patch")
        print("   The ESP-IDF version may have changed. Manual patching required.")
        print("")
        print("   Diagnostic information:")
        # Show if xTaskCreatePinnedToCore exists at all
        if 'xTaskCreatePinnedToCore' in content:
            print("   ✓ xTaskCreatePinnedToCore found in file")
        else:
            print("   ✗ xTaskCreatePinnedToCore NOT found in file")
        
        if 's_timer_task' in content:
            print("   ✓ s_timer_task variable found in file")
        else:
            print("   ✗ s_timer_task variable NOT found in file")
        
        # Try to extract the actual line for user inspection
        task_create_pattern = re.search(r'xTaskCreatePinnedToCore[^\;]{0,200};', content, re.DOTALL)
        if task_create_pattern:
            print("\n   Found xTaskCreatePinnedToCore call:")
            print("   " + task_create_pattern.group(0)[:150].replace('\n', '\n   '))
        
        return False
    
    # Extract matched groups
    before_core = match.group(1)
    old_core_value = match.group(2)
    after_core = match.group(3)
    
    # Find appropriate indentation
    line_start = content.rfind('\n', 0, match.start()) + 1
    indent = content[line_start:match.start()]
    if not indent.strip():  # It's all whitespace
        pass
    else:
        indent = '    '  # Default to 4 spaces
    
    # Add the #ifndef check before the xTaskCreatePinnedToCore call
    patch_check = f'''{indent}#ifndef ESP_TIMER_TASK_CORE
{indent}#error "ESP_TIMER_TASK_CORE is not defined. Add -DESP_TIMER_TASK_CORE=0 or 1 to build_flags in platformio.ini"
{indent}#endif
{indent}'''
    
    # Replace the hardcoded core value with ESP_TIMER_TASK_CORE
    patched_call = f"{patch_check}{before_core}ESP_TIMER_TASK_CORE{after_core}"
    
    # Apply the patch
    original_call = match.group(0)
    content_patched = content.replace(original_call, patched_call)
    
    # Verify the patch worked
    if content_patched == content:
        print("❌ Patch application failed - content unchanged")
        return False
    
    # Write the patched file
    try:
        with open(file_path, 'w', encoding='utf-8') as f:
            f.write(content_patched)
        print(f"✅ Patched esp_timer.c: {old_core_value} → ESP_TIMER_TASK_CORE")
        return True
    except Exception as e:
        print(f"❌ Failed to write patched file: {e}")
        return False


def patch_esp_timer_core():
    """Main entry point - locate and patch esp_timer.c if needed."""
    print("=" * 70)
    print("ESP Timer Core Patch")
    print("=" * 70)
    
    # Locate esp_timer.c
    esp_timer_path = find_esp_timer_c()
    if not esp_timer_path:
        print("❌ Cannot proceed without esp_timer.c")
        env.Exit(1)
        return
    
    print(f"📄 Found: {esp_timer_path}")
    
    # Check if already patched
    if is_already_patched(esp_timer_path):
        print("✓  Already patched - no action needed")
        print("=" * 70)
        return
    
    print("⚙️  Applying patch...")
    
    # Apply the patch
    success = apply_patch(esp_timer_path)
    
    if success:
        print("✅ Patch applied successfully")
        print("   ESP_TIMER_TASK core is now configurable via -DESP_TIMER_TASK_CORE")
    else:
        print("❌ Patch failed - build may not succeed")
        print("   See RTOSSettings.h for manual patching instructions")
        env.Exit(1)
    
    print("=" * 70)


# Run the patch check/application
patch_esp_timer_core()
