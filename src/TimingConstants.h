// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
/************************************************************************************************/
/* Timing Constants for ESP32 implementation */
/************************************************************************************************/
// Below values are in microseconds

// Times in ms, spec in ms
constexpr int FOIL_LOCK_TIME = 300;  // 300 +/- 25 ms
constexpr int EPEE_LOCK_TIME = 45;   // 40-50 ms or 45 +/- 5 ms
constexpr int SABRE_LOCK_TIME = 170; // 170 +/- 10 ms

constexpr int FoilContactTime_us = 13500;
constexpr int Foil_DosSantosCorrection_us = 150;
constexpr int Foil_LameLeak_us = 2000;

constexpr int EpeeContactTime_us = 6000;
constexpr int Epee_DosSantosCorrection_us = 150;

constexpr int SabreContactTime_us = 120;
constexpr int Sabre_DosSantosCorrection_us = 2 * SabreContactTime_us / 3;