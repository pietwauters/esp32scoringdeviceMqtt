// Copyright (c) Piet Wauters 2025 <piet.wauters@gmail.com>
#pragma once

#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include <cstdint>

/**
 * @brief RAII guard that temporarily re-enables brownout detection while a
 *        flash-write block is executing, then disables it again on scope exit.
 *
 * All state is encapsulated as private static members — there are no loose
 * globals.  Call @ref FlashWriteGuard::init() once from @c setup() before
 * any flash writes occur.  Use @ref FlashWriteGuard::setDisable() at runtime
 * to switch strategy without a reboot.
 *
 * Behaviour depends on the @c disable flag passed to @ref init():
 *  - @b true  (default): brownout detection is kept OFF globally.  Each guard
 *                         instance re-enables it at the captured level-0 value
 *                         and turns it off again on destruction.
 *  - @b false           : brownout detection is always ON.  The guard becomes
 *                         a no-op (register is never touched).
 *
 * Usage:
 * @code
 *   // In setup():
 *   FlashWriteGuard::init();           // captures register, disables detection
 *
 *   // Around every flash write:
 *   {
 *       FlashWriteGuard guard;         // detection ON for this scope
 *       prefs.begin("ns", false);
 *       prefs.putInt("key", value);
 *       prefs.end();
 *   }                                  // detection OFF again
 *
 *   // To keep detection always on at runtime:
 *   FlashWriteGuard::setDisable(false);
 * @endcode
 *
 * The class is non-copyable and non-movable (like std::lock_guard).
 */
class FlashWriteGuard {
public:
  /**
   * @brief Capture the current brownout register value and apply the initial
   *        state.  Must be called once from @c setup() before any flash writes.
   *
   * @param disable  @c true  (default) → disable detection globally; guarded
   *                           writes re-enable it temporarily.
   *                 @c false → leave detection on; guards become no-ops.
   */
  static void init(bool disable = true);

  /**
   * @brief Change the detection strategy at runtime without a reboot.
   *
   * Switching from @c true to @c false immediately re-enables detection.
   * Switching from @c false to @c true immediately disables detection.
   *
   * @param disable  New value for the disable flag (see @ref init()).
   */
  static void setDisable(bool disable);

  /// @return Current value of the disable flag.
  static bool getDisable() { return s_disable; }

  /// Re-enable brownout detection for this scope (no-op when disable == false).
  FlashWriteGuard();

  /// Disable brownout detection again (no-op when disable == false).
  ~FlashWriteGuard();

  FlashWriteGuard(const FlashWriteGuard &) = delete;
  FlashWriteGuard &operator=(const FlashWriteGuard &) = delete;
  FlashWriteGuard(FlashWriteGuard &&) = delete;
  FlashWriteGuard &operator=(FlashWriteGuard &&) = delete;

private:
  /// Brownout register value captured by init() (level-0, ~2.43 V threshold).
  static uint32_t s_savedReg;

  /// Whether brownout detection is globally disabled outside of guarded scopes.
  static bool s_disable;
};
