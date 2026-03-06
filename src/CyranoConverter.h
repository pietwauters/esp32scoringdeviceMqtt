// ---- CyranoConverter.h ----
#pragma once
#include "cJSON.h"
#include <cstdint>
#include <string>
extern cJSON *cyrano_to_json(const std::string &cyrano_str);
extern std::string json_to_cyrano(cJSON *json);
extern std::string convert_cyrano_to_json_string(const std::string &cyrano_str);
extern std::string convert_json_to_cyrano_string(const std::string &json_str);

// Create a UW2F_Timer JSON object. `time_ms` is timer duration in ms,
// `timestamp_ms` is epoch milliseconds for the event.
extern cJSON *make_uw2f_timer(const std::string &piste, int time_ms,
                              long long timestamp_ms);
extern std::string make_uw2f_timer_string(const std::string &piste, int time_ms,
                                          long long timestamp_ms);

// Create a UW2F_Timer JSON object from an encoded event word.
// `eventtype` is the full event word (contains the 24-bit time in the low
// bytes). `timestamp_ms` is epoch milliseconds for the event.
extern cJSON *make_uw2f_timer_from_event(uint32_t eventtype,
                                         const std::string &piste,
                                         long long timestamp_ms);
extern std::string make_uw2f_timer_from_event_string(uint32_t eventtype,
                                                     const std::string &piste,
                                                     long long timestamp_ms);
