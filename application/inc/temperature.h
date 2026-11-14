/*
 * Board temperature from the NTC (R25) next to the RF front end, read on ADC1 channel 5 (PB1).
 * Divider: 3V3 -- R24 (fixed) -- TEMP -- R25 (NTC) -- GND. The ADC reference is the same 3V3,
 * so the reading is ratiometric and does not depend on the exact supply voltage.
 */
#ifndef TEMPERATURE_H
#define TEMPERATURE_H

#include <stdbool.h>
#include <stdint.h>

/* Calibrate the ADC (configured by CubeMX). Call before the scheduler starts. */
void temperature_init(void);

/* Read the board temperature in 0.1 °C. Returns false if the ADC fails or the NTC looks open/shorted.
 * raw_out (optional) receives the averaged 12-bit ADC code. Only call from one thread. */
bool temperature_read_decideg(int32_t *decideg, uint16_t *raw_out);

#endif /* TEMPERATURE_H */
