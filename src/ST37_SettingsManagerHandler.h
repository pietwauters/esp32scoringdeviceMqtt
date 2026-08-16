// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// Vendor extension: x_ST37_SettingsManager. Pushes updated setting values
// (e.g. blocking time) that the apparatus is expected to apply. Not part of
// the core OPP2 publisher vocabulary (apparatus/software/remote) -- carried
// under its own topic segment, same openpiste/{piste_id}/{publisher}/
// {message_type} shape (docs/level2.md Section 5 permits new {publisher}
// values without a version bump).
//
// Plumbing only for now: subscribes, routes, parses, and logs. Does NOT yet
// apply any received setting to device behavior -- there is no existing
// "blocking time" (or similar) concept anywhere in the weapon-sensing code
// today, so wiring a received value to real behavior needs its own design
// pass before it's implemented (see ProcessMessage() in the .cpp).
#ifndef ST37_SETTINGS_MANAGER_HANDLER_H
#define ST37_SETTINGS_MANAGER_HANDLER_H
#include "Singleton.h"
#include <cstddef>

class ST37_SettingsManagerHandler
    : public SingletonMixin<ST37_SettingsManagerHandler> {
public:
  // Called by Opp2Handler::OnMqttMessageStatic for messages under
  // openpiste/{piste_id}/x_ST37_SettingsManager/#.
  void ProcessMessage(const char *topic, const char *payload, size_t length);

private:
  friend class SingletonMixin<ST37_SettingsManagerHandler>;
  ST37_SettingsManagerHandler() = default;
};

#endif // ST37_SETTINGS_MANAGER_HANDLER_H
