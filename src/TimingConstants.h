// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
/************************************************************************************************/
/* Timing Constants for ESP32 implementation */
/************************************************************************************************/
// Below values are in microseconds

// FIE default blocking ("lock") times, in ms -- used only to initialize the
// live FOIL_LOCK_TIME/EPEE_LOCK_TIME/SABRE_LOCK_TIME variables below at
// boot. The rest of the code reads those variables, not these constants.
constexpr int FIE_FOIL_LOCK_TIME = 300;  // 300 +/- 25 ms
constexpr int FIE_EPEE_LOCK_TIME = 45;   // 40-50 ms or 45 +/- 5 ms
constexpr int FIE_SABRE_LOCK_TIME = 170; // 170 +/- 10 ms

// Live blocking times, in ms -- start at the FIE defaults above, may be
// changed at runtime via x_ST37_SettingsManager (see
// ST37_SettingsManagerHandler.cpp). volatile: written from Core 0 (MQTT
// task), read from Core 1 (weapon sensing, foil.cpp/epee.cpp/sabre.cpp).
// Each is a single aligned 32-bit int, so plain reads/writes are atomic on
// this platform and each lock event samples the value once at trigger
// time -- no mutex needed (same convention as WS2812BLedStrip.cpp's
// m_animationRunning hint).
extern volatile int FOIL_LOCK_TIME;
extern volatile int EPEE_LOCK_TIME;
extern volatile int SABRE_LOCK_TIME;

// Persists a single weapon's experimental blocking-time override to NVS
// (its own "experiments" namespace -- distinct storage from every other
// setting, and never conflated with the FIE_* defaults, which are never
// stored in NVS at all). If experimental mode is currently on (see
// ExperimentalMode.h), also applies it to the matching live variable above
// immediately. weaponCode is "F", "E", or "S"; returns false (no-op) for
// anything else. Called by ST37_SettingsManagerHandler when a
// blocking_time_ms value arrives.
bool SetExperimentalBlockingTimeMs(const char *weaponCode, int ms);

// Applies (enabled=true: reads each weapon's NVS override, falling back to
// its FIE default if none was ever stored) or reverts (enabled=false:
// resets straight to the FIE defaults, no NVS access needed) the three
// live variables above. Called by SetExperimentalMode() (ExperimentalMode.cpp)
// whenever the runtime "Experimental" flag changes.
void ApplyExperimentalBlockingTimes(bool enabled);

constexpr int FoilContactTime_us = 13500;
constexpr int Foil_DosSantosCorrection_us = 150;
constexpr int Foil_LameLeak_us = 2000;

constexpr int EpeeContactTime_us = 6000;
constexpr int Epee_DosSantosCorrection_us = 150;

constexpr int SabreContactTime_us = 120;
constexpr int Sabre_DosSantosCorrection_us = 2 * SabreContactTime_us / 3;
constexpr int SabreWhiteTime_us =
    2500; // Spec says: 3ms +/- 2ms The setting is a hard lower bound