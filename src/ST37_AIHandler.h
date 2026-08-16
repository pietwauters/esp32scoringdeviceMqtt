// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// Vendor extension: x_ST37_AI. Issues commands the apparatus is expected to
// act on -- e.g. blocking one side's signal. Not part of the core OPP2
// publisher vocabulary (apparatus/software/remote) -- carried under its own
// topic segment, same openpiste/{piste_id}/{publisher}/{message_type} shape
// (docs/level2.md Section 5 permits new {publisher} values without a
// version bump). Payload mirrors the existing control message shape
// (Section 12: command/side/duration).
//
// Plumbing only for now: subscribes, routes, parses, and logs. Does NOT yet
// act on any command -- what commands exist and how the apparatus should
// react (e.g. "block signals from one side") is still being defined; see
// ProcessMessage() in the .cpp.
#ifndef ST37_AI_HANDLER_H
#define ST37_AI_HANDLER_H
#include "Singleton.h"
#include <cstddef>

class ST37_AIHandler : public SingletonMixin<ST37_AIHandler> {
public:
  // Called by Opp2Handler::OnMqttMessageStatic for messages under
  // openpiste/{piste_id}/x_ST37_AI/#.
  void ProcessMessage(const char *topic, const char *payload, size_t length);

private:
  friend class SingletonMixin<ST37_AIHandler>;
  ST37_AIHandler() = default;
};

#endif // ST37_AI_HANDLER_H
