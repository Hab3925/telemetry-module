#include "temperature.h"

#include <math.h>
#include <stddef.h>

#include "adc.h"
#include "app_config.h"

#define ADC_FULL_SCALE  4095u
#define ADC_SAMPLES     16u
#define ADC_TIMEOUT_MS  2u
#define KELVIN_0C       273.15f

void temperature_init(void)
{
  /* Channel and sampling time (247.5 cycles) come from CubeMX; the calibration is not generated. */
  (void)HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
}

bool temperature_read_decideg(int32_t *decideg, uint16_t *raw_out)
{
  uint32_t sum = 0;
  for (uint32_t i = 0; i < ADC_SAMPLES; i++)
  {
    if ((HAL_ADC_Start(&hadc1) != HAL_OK) || (HAL_ADC_PollForConversion(&hadc1, ADC_TIMEOUT_MS) != HAL_OK))
    {
      (void)HAL_ADC_Stop(&hadc1);
      return false;
    }
    sum += HAL_ADC_GetValue(&hadc1);
  }
  (void)HAL_ADC_Stop(&hadc1);

  const float code = (float)sum / ADC_SAMPLES;
  if (raw_out != NULL)
  {
    *raw_out = (uint16_t)(code + 0.5f);
  }
  if ((code < 10.0f) || (code > (float)ADC_FULL_SCALE - 10.0f))
  {
    return false; /* NTC shorted or open */
  }

  /* Divider: code / full = R_ntc / (R_fixed + R_ntc) */
  const float r_ntc = (float)APP_NTC_FIXED_OHM * code / ((float)ADC_FULL_SCALE - code);

  /* Beta equation: 1/T = 1/T25 + ln(R/R25)/B */
  const float inv_t = 1.0f / (25.0f + KELVIN_0C) + logf(r_ntc / (float)APP_NTC_R25_OHM) / (float)APP_NTC_BETA;
  const float celsius = 1.0f / inv_t - KELVIN_0C;
  *decideg = (int32_t)lroundf(celsius * 10.0f);
  return true;
}
