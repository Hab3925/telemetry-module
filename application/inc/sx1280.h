/*
 * Minimal blocking SX1280 driver (LoRa only) for SPI1.
 * Only call from a single thread (the radio thread); it is not reentrant.
 */
#ifndef SX1280_H
#define SX1280_H

#include <stdbool.h>
#include <stdint.h>
#include "sx1280_regs.h"

typedef struct
{
  uint8_t sf;           /* SX1280_LORA_SFx */
  uint8_t bw;           /* SX1280_LORA_BW_x */
  uint8_t cr;           /* SX1280_LORA_CR_4_x */
  uint8_t preamble_len; /* symbols, 1..15 (encoded as mantissa, exponent 0) */
} sx1280_lora_cfg_t;

/* Reset the chip, verify SPI communication and apply the base configuration
 * (DC-DC, LoRa packet type, auto-FS, high sensitivity). Leaves the radio in STDBY_RC. */
bool sx1280_init(void);

uint16_t sx1280_firmware_version(void);

/* RX LNA mode (register 0x891 bits 7:6): true = high sensitivity, false = normal (low power). */
void sx1280_set_high_sensitivity(bool enable);
uint32_t sx1280_busy_timeouts(void);

void sx1280_set_standby(uint8_t mode);
void sx1280_set_fs(void);
void sx1280_set_frequency_hz(uint32_t hz);
void sx1280_set_lora_modulation(const sx1280_lora_cfg_t *cfg);
void sx1280_set_lora_packet(const sx1280_lora_cfg_t *cfg, uint8_t payload_len, bool explicit_header, bool crc);
void sx1280_set_tx_power(int8_t dbm); /* -18 .. +13 dBm at the chip output */
void sx1280_set_dio_irq(uint16_t irq_mask, uint16_t dio1_mask);
uint16_t sx1280_get_irq_status(void);
void sx1280_clear_irq(uint16_t mask);

void sx1280_set_tx(uint8_t period_base, uint16_t count);
void sx1280_set_rx(uint8_t period_base, uint16_t count); /* count 0xFFFF = continuous */
void sx1280_set_tx_cw(void); /* unmodulated carrier until the next mode change (test only) */

void sx1280_write_buffer(uint8_t offset, const uint8_t *data, uint8_t len);
void sx1280_read_buffer(uint8_t offset, uint8_t *data, uint8_t len);
void sx1280_get_rx_buffer_status(uint8_t *len, uint8_t *offset);
void sx1280_get_lora_packet_status(int16_t *rssi_dbm, int8_t *snr_db);
int16_t sx1280_get_rssi_inst(void);

/* LoRa time on air in microseconds (datasheet formula, standard interleaving). */
uint32_t sx1280_lora_time_on_air_us(const sx1280_lora_cfg_t *cfg, uint8_t payload_len,
                                    bool explicit_header, bool crc);
uint32_t sx1280_lora_bandwidth_hz(uint8_t bw);

#endif /* SX1280_H */
