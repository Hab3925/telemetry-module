#include "radio_timer.h"

#include "main.h"
#include "tim.h"

void rtimer_init(void)
{
  /* TIM2 is configured by CubeMX (MX_TIM2_Init): prescaler 249, period 0xFFFFFFFF, CH1 output compare
   * without output, interrupt enabled. The prescaler assumes the 250 MHz APB1 timer clock: if the clock
   * tree changes, the radio timing would silently be wrong, so stop here instead. */
  uint32_t tim_clk = HAL_RCC_GetPCLK1Freq();
  if ((RCC->CFGR2 & RCC_CFGR2_PPRE1_2) != 0u)
  {
    tim_clk *= 2u; /* APB1 divided: the timer runs at twice PCLK1 */
  }
  if (tim_clk / (htim2.Init.Prescaler + 1u) != 1000000u)
  {
    Error_Handler(); /* TIM2 must count at 1 MHz: fix the prescaler in the .ioc */
  }
  TIM2->SR = 0;
  (void)HAL_TIM_Base_Start(&htim2);
}

void rtimer_schedule(uint32_t t_us)
{
  TIM2->DIER &= ~TIM_DIER_CC1IE;
  TIM2->CCR1 = t_us;
  TIM2->SR = ~TIM_SR_CC1IF;
  TIM2->DIER |= TIM_DIER_CC1IE;

  /* If the deadline already passed the compare will not match until the counter wraps: force it. */
  if ((int32_t)(t_us - TIM2->CNT) <= 0)
  {
    TIM2->EGR = TIM_EGR_CC1G;
  }
}

void rtimer_cancel(void)
{
  TIM2->DIER &= ~TIM_DIER_CC1IE;
  TIM2->SR = ~TIM_SR_CC1IF;
}

void rtimer_delay_us(uint32_t us)
{
  uint32_t start = TIM2->CNT;
  while ((TIM2->CNT - start) < us)
  {
  }
}

void TIM2_IRQHandler(void)
{
  if (((TIM2->SR & TIM_SR_CC1IF) != 0u) && ((TIM2->DIER & TIM_DIER_CC1IE) != 0u))
  {
    TIM2->SR = ~TIM_SR_CC1IF;
    TIM2->DIER &= ~TIM_DIER_CC1IE; /* one-shot */
    rtimer_callback();
  }
}
