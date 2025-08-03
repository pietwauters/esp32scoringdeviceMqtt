#pragma once

#include <limits.h>
#include <stdint.h>

class RunningAdcWindow {
public:
  RunningAdcWindow() { clear(); }

  // Add a new sample to the buffer
  inline void add(int value) {
    buffer[index] = value;
    index = (index + 1) % SIZE;
    if (count < SIZE)
      count++;
  }

  // Clear the buffer
  inline void clear() {
    count = 0;
    index = 0;
  }

  // Get the average, excluding min and max (returns 0 if not enough samples)
  inline int get_average() const {
    if (count < SIZE)
      return 0; // Not enough samples yet

    int min = INT_MAX, max = INT_MIN, sum = 0;
    for (int i = 0; i < SIZE; ++i) {
      int v = buffer[i];
      sum += v;
      if (v < min)
        min = v;
      if (v > max)
        max = v;
    }
    sum -= min;
    sum -= max;
    // Divide by 8 (right shift by 3) for fast average
    return sum >> 3;
  }

private:
  static constexpr int SIZE = 10;
  int buffer[SIZE];
  int index = 0;
  int count = 0;
};