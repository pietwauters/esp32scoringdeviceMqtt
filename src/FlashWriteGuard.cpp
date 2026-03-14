// Copyright (c) Piet Wauters 2025 <piet.wauters@gmail.com>
#include "FlashWriteGuard.h"

// ---------------------------------------------------------------------------
// Static member storage
// ---------------------------------------------------------------------------
uint32_t FlashWriteGuard::s_savedReg = 0;
bool FlashWriteGuard::s_disable = true;

// ---------------------------------------------------------------------------
// Static interface
// ---------------------------------------------------------------------------

void FlashWriteGuard::init(bool disable) {
  s_disable = disable;
  s_savedReg = READ_PERI_REG(RTC_CNTL_BROWN_OUT_REG);
  if (s_disable) {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  }
}

void FlashWriteGuard::setDisable(bool disable) {
  if (disable == s_disable)
    return;
  s_disable = disable;
  if (s_disable) {
    // switching to "disable globally" — turn detection off now
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  } else {
    // switching to "always on" — restore detection now
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, s_savedReg);
  }
}

// ---------------------------------------------------------------------------
// RAII guard
// ---------------------------------------------------------------------------

FlashWriteGuard::FlashWriteGuard() {
  if (s_disable) {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, s_savedReg);
  }
}

FlashWriteGuard::~FlashWriteGuard() {
  if (s_disable) {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  }
}
