#pragma once
#include "driver/adc.h"
#include "soc/sens_reg.h"
#include "soc/sens_struct.h"
// If you have different pins, change below defines
/*#define cl_analog 36
#define bl_analog 39
#define piste_analog 34
#define cr_analog 35
#define br_analog 32*/
// Below table uses AD channels and not pin numbers
#define cl_analog 0
#define bl_analog 3
#define piste_analog 6
#define cr_analog 7
#define br_analog 4

#ifdef FIRST_PROTO
#define al_driver 22
#define bl_driver 21
#define cl_driver 23
#define ar_driver 05
#define br_driver 04
#define cr_driver 18
#define piste_driver 19
#endif

#ifdef SECOND_PROTO
#define al_driver 33
#define bl_driver 21
#define cl_driver 23
#define ar_driver 25
#define br_driver 05
#define cr_driver 18
#define piste_driver 19
#endif

#ifdef __cplusplus
extern "C" {
#endif
void adc1_fast_begin(void);
int fast_adc1_get_raw(adc1_channel_t channel);
void adc1_fast_register_channel(adc1_channel_t channel);
void adc1_fast_begin_unsafe(void);

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
