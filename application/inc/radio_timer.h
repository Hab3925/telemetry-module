/*
 * 1 MHz free-running 32-bit timebase on TIM2 with one compare event (CC1).
 * Used for frame timing of the radio link and for short busy-wait delays.
 * TIM2 is set up in CubeMX (1 MHz, free-running, CH1 output compare, interrupt enabled). Its interrupt
 * handler is here: keep "Generate IRQ handler" for TIM2 switched off in the .ioc (NVIC, Code generation).
 */
#ifndef RADIO_TIMER_H
#define RADIO_TIMER_H

#include <stdint.h>
#include "stm32h5xx.h"

void rtimer_init(void);

static inline uint32_t rtimer_now(void)
{
  return TIM2->CNT;
}

/* Fire rtimer_callback() from the TIM2 ISR at absolute time t_us (fires immediately if already passed). */
void rtimer_schedule(uint32_t t_us);
void rtimer_cancel(void);
void rtimer_delay_us(uint32_t us);

/* Implemented by the user (radio link). Runs in interrupt context. */
void rtimer_callback(void);

#endif /* RADIO_TIMER_H */
