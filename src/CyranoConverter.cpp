// ---- CyranoConverter.cpp ----
#include "CyranoConverter.h"
#include "CyranoFields.h"
#include <sstream>
#include <algorithm>
#include "cJSON.h"

// Split function preserving empty fields
std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);

    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

// Join function preserving empty fields
std::string join(const std::vector<std::string>& elements, char delimiter) {
    std::ostringstream oss;
    for (size_t i = 0; i < elements.size(); ++i) {
        if (i > 0) oss << delimiter;
        oss << elements[i];
    }
    return oss.str();
}

cJSON* cyrano_to_json(const std::string& cyrano_str) {
    std::vector<std::string> groups;
    size_t start = 0;

    // Split groups using "%|" delimiter
    while (true) {
        size_t end = cyrano_str.find("%|", start);
        if (end == std::string::npos) {
            groups.push_back(cyrano_str.substr(start));
            break;
        }
        groups.push_back(cyrano_str.substr(start, end - start));
        start = end + 2;
    }

    // Remove empty groups
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                 [](const std::string& s) { return s.empty(); }), groups.end());

    // Validate group structure
    if (groups.size() != 1 && groups.size() != 3) return nullptr;

    cJSON* root = cJSON_CreateObject();
    if (!root) return nullptr;

    // Process header group
    std::vector<std::string> header = split(groups[0], '|');
    if (!header.empty() && header.front().empty()) header.erase(header.begin());
    if (!header.empty() && header.back().empty()) header.pop_back();
    header.resize(17, "");

    for (int i = 0; i < 17; ++i) {
        if(header[i] != ""){
            cJSON_AddStringToObject(root, header_fields[i], header[i].c_str());
        }
    }

    // Process fencer groups
    if (groups.size() == 3) {
        // Right fencer
        std::vector<std::string> right = split(groups[1], '|');
        //if (!right.empty() && right.front().empty()) right.erase(right.begin());
        if (!right.empty() && right.back().empty()) right.pop_back();
        right.resize(12, "");
        for (int i = 0; i < 12; ++i) {
            if(right[i] != ""){
                cJSON_AddStringToObject(root, right_fencer_fields[i], right[i].c_str());
            }
        }

        // Left fencer
        std::vector<std::string> left = split(groups[2], '|');
        //if (!left.empty() && left.front().empty()) left.erase(left.begin());
        if (!left.empty() && left.back().empty()) left.pop_back();
        left.resize(12, "");
        for (int i = 0; i < 12; ++i) {
            if(left[i] != ""){
                cJSON_AddStringToObject(root, left_fencer_fields[i], left[i].c_str());
            }
        }
    }

    return root;
}

std::string json_to_cyrano(cJSON* json) {
    if (!json) return "";

    std::ostringstream oss;

    // Header group (17 fields)
    std::vector<std::string> header;
    for (int i = 0; i < 17; ++i) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(json, header_fields[i]);
        header.push_back(item && cJSON_IsString(item) ? item->valuestring : "");
    }
    // Trim trailing empty fields in reverse order
    while (!header.empty() && header.back().empty()) {
    header.pop_back();
    }


    oss << "|" << join(header, '|') << "|%";

    // Fencer groups (both or none)
    // bool has_right = cJSON_HasObjectItem(json, "RightId");
    // bool has_left = cJSON_HasObjectItem(json, "LeftId");

    //if (has_right && has_left) {
    if (true) {
        // Right fencer
        std::vector<std::string> right;
        for (int i = 0; i < 12; ++i) {
            cJSON* item = cJSON_GetObjectItemCaseSensitive(json, right_fencer_fields[i]);
            right.push_back(item && cJSON_IsString(item) ? item->valuestring : "");
        }
        // Trim trailing empty fields in reverse order
        while (!right.empty() && right.back().empty()) {
            right.pop_back();
        }
        if(right.empty())
            return  oss.str();;///// we're done, there are no fencers parts
        oss << "|" << join(right, '|') << "|%";

        // Left fencer
        std::vector<std::string> left;
        for (int i = 0; i < 12; ++i) {
            cJSON* item = cJSON_GetObjectItemCaseSensitive(json, left_fencer_fields[i]);
            left.push_back(item && cJSON_IsString(item) ? item->valuestring : "");
        }
        while (!left.empty() && left.back().empty()) {
            left.pop_back();
        }
        oss << "|" << join(left, '|') << "|%";
    }
    oss << "|" ;

    return oss.str();
}

std::string cjson_to_string(const cJSON* json) {
  if (!json) return ""; // Handle null input

  // Generate an unformatted string (no newlines/whitespace)
  char* json_str = cJSON_PrintUnformatted(json);

  if (!json_str) return ""; // Handle print failure

  // Convert to std::string and free allocated memory
  std::string result(json_str);
  cJSON_free(json_str); // Use cJSON's free function

  return result;
}

std::string convert_cyrano_to_json_string(const std::string& cyrano_str) {
    cJSON* json = cyrano_to_json(cyrano_str);
    if (!json) return "{}";  // Return empty JSON object on failure

    char* json_str = cJSON_PrintUnformatted(json);
    std::string result(json_str ? json_str : "{}");

    cJSON_Delete(json);
    if (json_str) cJSON_free(json_str);

    return result;
}

std::string convert_json_to_cyrano_string(const std::string& json_str) {
    cJSON* json = cJSON_Parse(json_str.c_str());
    if (!json) return "";  // Return empty string on failure

    std::string cyrano = json_to_cyrano(json);
    cJSON_Delete(json);

    return cyrano;
}
