#pragma once
#include "driver/adc.h"
#include "soc/sens_reg.h"
#include "soc/sens_struct.h"
#ifdef __cplusplus
extern "C" {
#endif
void adc1_fast_begin(void);
int fast_adc1_get_raw(adc1_channel_t channel);
void adc1_fast_register_channel(adc1_channel_t channel);
void adc1_fast_begin_unsafe(void);
uint16_t fast_adc1_get_raw_unsafe(adc1_channel_t channel);

static inline int fast_adc1_get_raw_inline(adc1_channel_t channel) {
  SENS.sar_meas_start1.sar1_en_pad = 1 << channel;
  SENS.sar_meas_start1.meas1_start_sar = 1;
  while (SENS.sar_meas_start1.meas1_done_sar == 0) {
  }
  SENS.sar_meas_start1.meas1_start_sar = 0;
  return SENS.sar_meas_start1.meas1_data_sar;
}

#ifdef __cplusplus
}
#endif
