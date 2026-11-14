#include "board.h"

#include "main.h"
#include "app_config.h"
#include "util.h"

static volatile uint32_t s_faults;

void board_early_init(void)
{
  /* Pin start levels come from CubeMX (radio deselected and held in reset, front end off, LEDs off).
   * Only the antenna switch depends on the application config. */
  board_antenna_select(APP_ANT_RX_PORT);
}

void board_eth_phy_reset(void)
{
  HAL_GPIO_WritePin(ETH_nRST_GPIO_Port, ETH_nRST_Pin, GPIO_PIN_RESET);
  HAL_Delay(2);  /* LAN8742A: reset pulse >= 100 us */
  HAL_GPIO_WritePin(ETH_nRST_GPIO_Port, ETH_nRST_Pin, GPIO_PIN_SET);
  HAL_Delay(20); /* crystal start-up and strap latching before REF_CLK is stable */
}

void board_get_mac_address(uint8_t mac[6])
{
  /* Read the UID as 32-bit words through the HAL and hash a RAM copy: byte access to the
   * UID area hard faults on the STM32H5. */
  const uint32_t uid[3] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
  const uint32_t h = crc32((const uint8_t *)uid, sizeof(uid));
  mac[0] = 0x02; /* locally administered, unicast */
  mac[1] = 0x50;
  mac[2] = (uint8_t)(h >> 24);
  mac[3] = (uint8_t)(h >> 16);
  mac[4] = (uint8_t)(h >> 8);
  mac[5] = (uint8_t)h;
}

/* RFX2401C: TXEN=1 -> TX (RXEN don't care), TXEN=0/RXEN=1 -> RX, both 0 -> shutdown */
void board_fe_tx(void)
{
  HAL_GPIO_WritePin(RADIO_RX_EN_GPIO_Port, RADIO_RX_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(RADIO_TX_EN_GPIO_Port, RADIO_TX_EN_Pin, GPIO_PIN_SET);
}

void board_fe_rx(void)
{
  HAL_GPIO_WritePin(RADIO_TX_EN_GPIO_Port, RADIO_TX_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(RADIO_RX_EN_GPIO_Port, RADIO_RX_EN_Pin, GPIO_PIN_SET);
}

void board_fe_off(void)
{
  HAL_GPIO_WritePin(RADIO_TX_EN_GPIO_Port, RADIO_TX_EN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(RADIO_RX_EN_GPIO_Port, RADIO_RX_EN_Pin, GPIO_PIN_RESET);
}

/* SKY13414-485LF truth table (V1 V2 V3): 000 RF1, 001 RF2, 010 RF3, 011 RF4 */
void board_antenna_select(uint8_t port)
{
  const uint8_t code = (port >= 1u && port <= 4u) ? (uint8_t)(port - 1u) : 0u;
  HAL_GPIO_WritePin(ANT_V1_GPIO_Port, ANT_V1_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(ANT_V2_GPIO_Port, ANT_V2_Pin, (code & 0x2u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(ANT_V3_GPIO_Port, ANT_V3_Pin, (code & 0x1u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void board_led_user(bool on)
{
  HAL_GPIO_WritePin(USR_LED_GPIO_Port, USR_LED_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void board_led_error(bool on)
{
  HAL_GPIO_WritePin(ERROR_LED_GPIO_Port, ERROR_LED_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void board_set_fault(uint32_t fault, bool active)
{
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  s_faults = active ? (s_faults | fault) : (s_faults & ~fault);
  __set_PRIMASK(primask);
}

uint32_t board_get_faults(void)
{
  return s_faults;
}
