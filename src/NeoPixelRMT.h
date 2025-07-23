#pragma once
#include "driver/rmt.h"
#include <vector>
#include <algorithm>

class NeoPixelRMT {
public:
    NeoPixelRMT(uint16_t numPixels, gpio_num_t pin);
    ~NeoPixelRMT();

    void begin();

    void setPixelColor(uint16_t idx, uint32_t color);
    void fill(uint32_t color, int startIndex, int count);
    void clear();
    void show();

    static uint32_t Color(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 255);

private:
    void encodePixels();
    void setRMTConfig();

    static constexpr int BITS_PER_PIXEL = 24;

    uint16_t numPixels;
    gpio_num_t pin;
    rmt_channel_t channel;

    std::vector<uint32_t> pixels;      // Pixels in GRB order packed
    std::vector<rmt_item32_t> items;   // RMT waveform data

    // Timing parameters (clock ticks at clk_div=2, 40MHz tick = 25ns)
    static constexpr int T0H = 14; // 350ns
    static constexpr int T0L = 38; // 950ns
    static constexpr int T1H = 28; // 700ns
    static constexpr int T1L = 24; // 600ns
    static constexpr int RESET_US = 80; // Reset pulse length
};
