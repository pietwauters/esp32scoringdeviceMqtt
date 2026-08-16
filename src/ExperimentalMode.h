// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// Generic "Experimental" runtime switch, exposed as a checkbox on the
// remote's /settings page (see AppSettings.cpp's handleExperimentalPost()
// -- deliberately its own non-restarting route, not part of the main
// Save-and-Restart form). In-memory only, never written to NVS, so it
// always starts OFF after a reboot regardless of what it was set to
// before.
//
// Reused for future experiments the same way blocking time uses it today:
// each experiment persists its own override distinctly in NVS (see
// TimingConstants.cpp's "experiments" namespace) and applies/reverts it
// from its own logic when this flag changes -- SetExperimentalMode() below
// is the single place that fans out to all of them.
#ifndef EXPERIMENTAL_MODE_H
#define EXPERIMENTAL_MODE_H

bool IsExperimentalModeEnabled();

// Updates the in-memory flag and immediately re-applies (enabled) or
// reverts (disabled) every currently-registered experimental override.
void SetExperimentalMode(bool enabled);

#endif // EXPERIMENTAL_MODE_H
