/*
 * Board support: pins and peripherals of the 2026 Telemetry Module that are not covered by CubeMX.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#define BOARD_FAULT_RADIO    (1u << 0)
#define BOARD_FAULT_ETH_LINK (1u << 1)

/* Called from main() after the CubeMX peripheral init, before the scheduler starts: sets the antenna
 * switch for the configured port (the other pin start levels come from CubeMX). */
void board_early_init(void);

/* Called from MX_ETH_Init() before HAL_ETH_Init(): the LAN8742 supplies the 50 MHz RMII REF_CLK,
 * so it has to be out of reset before the ETH MAC/DMA can be reset. */
void board_eth_phy_reset(void);

/* Locally administered MAC address derived from the MCU unique ID (unique per board). */
void board_get_mac_address(uint8_t mac[6]);

/* RFX2401C front end */
void board_fe_tx(void);
void board_fe_rx(void);
void board_fe_off(void);

/* SKY13414 antenna switch, port 1..3 = RFAIO_1..3 (J3, J4, J5) */
void board_antenna_select(uint8_t port);

void board_led_user(bool on);
void board_led_error(bool on);

/* Fault flags are shown on the error LED by the status LED timer (app_main.c). */
void board_set_fault(uint32_t fault, bool active);
uint32_t board_get_faults(void);

#endif /* BOARD_H */
