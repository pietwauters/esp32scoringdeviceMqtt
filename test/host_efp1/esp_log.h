// Host-compilation stub for ESP-IDF's esp_log.h -- EFP1Message.cpp's only
// ESP-IDF dependency (used in print()). Not part of the real project; lives
// only in this test's include path.
#pragma once
#include <cstdio>
#define ESP_LOGI(tag, fmt, ...) printf("I(%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W(%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("E(%s) " fmt "\n", tag, ##__VA_ARGS__)
