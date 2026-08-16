// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
#include "TimingConstants.h"
#include "ExperimentalMode.h"
#include <Preferences.h>
#include <cstring>

volatile int FOIL_LOCK_TIME = FIE_FOIL_LOCK_TIME;
volatile int EPEE_LOCK_TIME = FIE_EPEE_LOCK_TIME;
volatile int SABRE_LOCK_TIME = FIE_SABRE_LOCK_TIME;

// Its own namespace -- distinct from every other setting -- so it's
// obviously "the experimental stash" and never confused with the FIE
// defaults, which are never written to NVS at all.
static const char *kExperimentsNamespace = "experiments";

bool SetExperimentalBlockingTimeMs(const char *weaponCode, int ms) {
  const char *key;
  if (strcmp(weaponCode, "F") == 0) {
    key = "ExpFoilMs";
  } else if (strcmp(weaponCode, "E") == 0) {
    key = "ExpEpeeMs";
  } else if (strcmp(weaponCode, "S") == 0) {
    key = "ExpSabreMs";
  } else {
    return false;
  }

  Preferences prefs;
  prefs.begin(kExperimentsNamespace, false);
  prefs.putInt(key, ms);
  prefs.end();

  // Persisted regardless -- only takes effect on the live variable while
  // Experimental mode is on. See ExperimentalMode.h.
  if (IsExperimentalModeEnabled()) {
    if (strcmp(weaponCode, "F") == 0) {
      FOIL_LOCK_TIME = ms;
    } else if (strcmp(weaponCode, "E") == 0) {
      EPEE_LOCK_TIME = ms;
    } else {
      SABRE_LOCK_TIME = ms;
    }
  }
  return true;
}

void ApplyExperimentalBlockingTimes(bool enabled) {
  if (!enabled) {
    FOIL_LOCK_TIME = FIE_FOIL_LOCK_TIME;
    EPEE_LOCK_TIME = FIE_EPEE_LOCK_TIME;
    SABRE_LOCK_TIME = FIE_SABRE_LOCK_TIME;
    return;
  }
  Preferences prefs;
  prefs.begin(kExperimentsNamespace, true); // read-only
  FOIL_LOCK_TIME = prefs.getInt("ExpFoilMs", FIE_FOIL_LOCK_TIME);
  EPEE_LOCK_TIME = prefs.getInt("ExpEpeeMs", FIE_EPEE_LOCK_TIME);
  SABRE_LOCK_TIME = prefs.getInt("ExpSabreMs", FIE_SABRE_LOCK_TIME);
  prefs.end();
}
