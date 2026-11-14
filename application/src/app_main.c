#include "app_main.h"

#include "board.h"
#include "dgram_queue.h"
#include "net_bridge.h"
#include "radio_link.h"
#include "radio_timer.h"
#include "temperature.h"

#define LED_TICK_MS 100u

static dgram_queue_t s_eth_to_radio;
static dgram_queue_t s_radio_to_eth;
static TX_TIMER s_led_timer;

/* Every 100 ms:
 *   USR LED:   1 Hz blink = running, no radio link; 5 Hz blink = radio link connected
 *   ERROR LED: solid = radio fault; 1 Hz blink = Ethernet link down */
static VOID led_timer_entry(ULONG arg)
{
  (void)arg;
  static uint32_t tick;
  tick++;

  radio_link_stats_t st;
  radio_link_get_stats(&st);
  const bool slow_phase = ((tick / 5u) % 2u) == 0u;
  board_led_user(st.connected ? ((tick % 2u) == 0u) : slow_phase);

  const uint32_t faults = board_get_faults();
  if ((faults & BOARD_FAULT_RADIO) != 0u)
  {
    board_led_error(true);
  }
  else
  {
    board_led_error(((faults & BOARD_FAULT_ETH_LINK) != 0u) && slow_phase);
  }
}

void app_early_init(void)
{
  board_early_init();
  rtimer_init();
  temperature_init();
}

void app_start(NX_IP *ip, NX_PACKET_POOL *pool)
{
  net_bridge_start(ip, pool, &s_eth_to_radio, &s_radio_to_eth);
  radio_link_start(&s_eth_to_radio, &s_radio_to_eth, net_bridge_notify);

  const ULONG period = (LED_TICK_MS * TX_TIMER_TICKS_PER_SECOND) / 1000u;
  tx_timer_create(&s_led_timer, "status leds", led_timer_entry, 0, period, period, TX_AUTO_ACTIVATE);
}
