// ---- CyranoConverter.h ----
#pragma once
#include "cJSON.h"
#include <string>
extern cJSON* cyrano_to_json(const std::string& cyrano_str);
extern std::string json_to_cyrano(cJSON* json);
extern std::string convert_cyrano_to_json_string(const std::string& cyrano_str);
extern std::string convert_json_to_cyrano_string(const std::string& json_str);
