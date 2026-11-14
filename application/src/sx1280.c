#include "sx1280.h"

#include <string.h>

#include "main.h"
#include "spi.h"
#include "radio_timer.h"

#define SX1280_SPI              (&hspi1)
#define SX1280_SPI_TIMEOUT_MS   10u
#define SX1280_BUSY_TIMEOUT_US  2000u
#define SX1280_BOOT_TIMEOUT_US  100000u

/* Largest transfer: WriteBuffer opcode + offset + 255 bytes (ReadBuffer adds one NOP). */
static uint8_t s_tx[260];
static uint8_t s_rx[260];
static uint32_t s_busy_timeouts;
static uint16_t s_fw_version;

static inline void cs_low(void)  { HAL_GPIO_WritePin(CS_RADIO_GPIO_Port, CS_RADIO_Pin, GPIO_PIN_RESET); }
static inline void cs_high(void) { HAL_GPIO_WritePin(CS_RADIO_GPIO_Port, CS_RADIO_Pin, GPIO_PIN_SET); }

static bool wait_busy(uint32_t timeout_us)
{
  uint32_t start = rtimer_now();
  while (HAL_GPIO_ReadPin(RADIO_BUSY_GPIO_Port, RADIO_BUSY_Pin) == GPIO_PIN_SET)
  {
    if ((rtimer_now() - start) > timeout_us)
    {
      s_busy_timeouts++;
      return false;
    }
  }
  return true;
}

static bool xfer(uint16_t len)
{
  if (!wait_busy(SX1280_BUSY_TIMEOUT_US))
  {
    return false;
  }
  cs_low();
  HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(SX1280_SPI, s_tx, s_rx, len, SX1280_SPI_TIMEOUT_MS);
  cs_high();
  return st == HAL_OK;
}

static void cmd_write(uint8_t opcode, const uint8_t *params, uint8_t n)
{
  s_tx[0] = opcode;
  if (n > 0u)
  {
    memcpy(&s_tx[1], params, n);
  }
  (void)xfer((uint16_t)(n + 1u));
}

static void cmd_write1(uint8_t opcode, uint8_t param)
{
  cmd_write(opcode, &param, 1);
}

/* Read commands return: [status] [data...] after the opcode byte. */
static void cmd_read(uint8_t opcode, uint8_t *out, uint8_t n)
{
  memset(s_tx, 0, (size_t)n + 2u);
  s_tx[0] = opcode;
  if (xfer((uint16_t)(n + 2u)))
  {
    memcpy(out, &s_rx[2], n);
  }
  else
  {
    memset(out, 0, n);
  }
}

static void reg_write(uint16_t addr, const uint8_t *data, uint8_t n)
{
  s_tx[0] = SX1280_CMD_WRITE_REGISTER;
  s_tx[1] = (uint8_t)(addr >> 8);
  s_tx[2] = (uint8_t)addr;
  memcpy(&s_tx[3], data, n);
  (void)xfer((uint16_t)(n + 3u));
}

static void reg_write1(uint16_t addr, uint8_t value)
{
  reg_write(addr, &value, 1);
}

static void reg_read(uint16_t addr, uint8_t *data, uint8_t n)
{
  memset(s_tx, 0, (size_t)n + 4u);
  s_tx[0] = SX1280_CMD_READ_REGISTER;
  s_tx[1] = (uint8_t)(addr >> 8);
  s_tx[2] = (uint8_t)addr;
  if (xfer((uint16_t)(n + 4u)))
  {
    memcpy(data, &s_rx[4], n);
  }
  else
  {
    memset(data, 0, n);
  }
}

static uint8_t reg_read1(uint16_t addr)
{
  uint8_t v;
  reg_read(addr, &v, 1);
  return v;
}

bool sx1280_init(void)
{
  cs_high();

  HAL_GPIO_WritePin(RADIO_nRST_GPIO_Port, RADIO_nRST_Pin, GPIO_PIN_RESET);
  rtimer_delay_us(2000);
  HAL_GPIO_WritePin(RADIO_nRST_GPIO_Port, RADIO_nRST_Pin, GPIO_PIN_SET);
  rtimer_delay_us(5000);
  if (!wait_busy(SX1280_BOOT_TIMEOUT_US))
  {
    return false;
  }

  uint8_t ver[2];
  reg_read(SX1280_REG_FIRMWARE_VERSION, ver, 2);
  s_fw_version = (uint16_t)((ver[0] << 8) | ver[1]);
  if ((s_fw_version == 0x0000u) || (s_fw_version == 0xFFFFu))
  {
    return false; /* no SPI communication */
  }

  sx1280_set_standby(SX1280_STANDBY_RC);
  cmd_write1(SX1280_CMD_SET_REGULATOR_MODE, SX1280_REGULATOR_DCDC); /* L1 is fitted */
  cmd_write1(SX1280_CMD_SET_PACKET_TYPE, SX1280_PACKET_TYPE_LORA);
  cmd_write1(SX1280_CMD_SET_AUTO_FS, 0x01);                          /* go to FS after TX/RX: faster turnaround */
  /* Normal LNA mode: in high sensitivity mode the SX1280 under-reports RSSI (packet and instantaneous)
   * by ~55 dB for moderate signals (measured), which breaks the LBT energy detection. The external
   * RFX2401C LNA sets the noise figure anyway, so the sensitivity cost is small. */
  sx1280_set_high_sensitivity(false);

  const uint8_t base[2] = {0x00, 0x00}; /* TX and RX share the full 256 byte buffer */
  cmd_write(SX1280_CMD_SET_BUFFER_BASE_ADDR, base, 2);
  sx1280_clear_irq(SX1280_IRQ_ALL);

  return true;
}

void sx1280_set_high_sensitivity(bool enable)
{
  const uint8_t reg = reg_read1(SX1280_REG_RX_GAIN);
  reg_write1(SX1280_REG_RX_GAIN, enable ? (uint8_t)(reg | 0xC0u) : (uint8_t)(reg & ~0xC0u));
}

uint16_t sx1280_firmware_version(void)
{
  return s_fw_version;
}

uint32_t sx1280_busy_timeouts(void)
{
  return s_busy_timeouts;
}

void sx1280_set_standby(uint8_t mode)
{
  cmd_write1(SX1280_CMD_SET_STANDBY, mode);
}

void sx1280_set_fs(void)
{
  cmd_write(SX1280_CMD_SET_FS, NULL, 0);
}

void sx1280_set_frequency_hz(uint32_t hz)
{
  uint32_t reg = (uint32_t)(((uint64_t)hz << 18) / SX1280_XTAL_HZ);
  const uint8_t p[3] = {(uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg};
  cmd_write(SX1280_CMD_SET_RF_FREQUENCY, p, 3);
}

void sx1280_set_lora_modulation(const sx1280_lora_cfg_t *cfg)
{
  const uint8_t p[3] = {cfg->sf, cfg->bw, cfg->cr};
  cmd_write(SX1280_CMD_SET_MODULATION_PARAMS, p, 3);

  /* Datasheet: SF-dependent register setting after SetModulationParams */
  uint8_t sf_cfg;
  switch (cfg->sf)
  {
    case SX1280_LORA_SF5:
    case SX1280_LORA_SF6: sf_cfg = 0x1E; break;
    case SX1280_LORA_SF7:
    case SX1280_LORA_SF8: sf_cfg = 0x37; break;
    default:              sf_cfg = 0x32; break;
  }
  reg_write1(SX1280_REG_LORA_SF_CONFIG, sf_cfg);
  reg_write1(SX1280_REG_LORA_FREQ_ERR_COMP, 0x01);
}

void sx1280_set_lora_packet(const sx1280_lora_cfg_t *cfg, uint8_t payload_len, bool explicit_header, bool crc)
{
  const uint8_t p[7] = {
      (uint8_t)(cfg->preamble_len & 0x0Fu), /* mantissa, exponent 0 */
      explicit_header ? SX1280_LORA_HEADER_EXPLICIT : SX1280_LORA_HEADER_IMPLICIT,
      payload_len,
      crc ? SX1280_LORA_CRC_ON : SX1280_LORA_CRC_OFF,
      SX1280_LORA_IQ_NORMAL,
      0x00,
      0x00};
  cmd_write(SX1280_CMD_SET_PACKET_PARAMS, p, 7);
}

void sx1280_set_tx_power(int8_t dbm)
{
  if (dbm < -18)
  {
    dbm = -18;
  }
  if (dbm > 13)
  {
    dbm = 13;
  }
  const uint8_t p[2] = {(uint8_t)(dbm + 18), SX1280_RAMP_04_US};
  cmd_write(SX1280_CMD_SET_TX_PARAMS, p, 2);
}

void sx1280_set_dio_irq(uint16_t irq_mask, uint16_t dio1_mask)
{
  const uint8_t p[8] = {(uint8_t)(irq_mask >> 8), (uint8_t)irq_mask,
                        (uint8_t)(dio1_mask >> 8), (uint8_t)dio1_mask,
                        0, 0, 0, 0};
  cmd_write(SX1280_CMD_SET_DIO_IRQ_PARAMS, p, 8);
}

uint16_t sx1280_get_irq_status(void)
{
  uint8_t s[2];
  cmd_read(SX1280_CMD_GET_IRQ_STATUS, s, 2);
  return (uint16_t)((s[0] << 8) | s[1]);
}

void sx1280_clear_irq(uint16_t mask)
{
  const uint8_t p[2] = {(uint8_t)(mask >> 8), (uint8_t)mask};
  cmd_write(SX1280_CMD_CLR_IRQ_STATUS, p, 2);
}

void sx1280_set_tx(uint8_t period_base, uint16_t count)
{
  const uint8_t p[3] = {period_base, (uint8_t)(count >> 8), (uint8_t)count};
  cmd_write(SX1280_CMD_SET_TX, p, 3);
}

void sx1280_set_rx(uint8_t period_base, uint16_t count)
{
  const uint8_t p[3] = {period_base, (uint8_t)(count >> 8), (uint8_t)count};
  cmd_write(SX1280_CMD_SET_RX, p, 3);
}

void sx1280_set_tx_cw(void)
{
  cmd_write(SX1280_CMD_SET_TX_CW, NULL, 0);
}

void sx1280_write_buffer(uint8_t offset, const uint8_t *data, uint8_t len)
{
  s_tx[0] = SX1280_CMD_WRITE_BUFFER;
  s_tx[1] = offset;
  memcpy(&s_tx[2], data, len);
  (void)xfer((uint16_t)(len + 2u));
}

void sx1280_read_buffer(uint8_t offset, uint8_t *data, uint8_t len)
{
  memset(s_tx, 0, (size_t)len + 3u);
  s_tx[0] = SX1280_CMD_READ_BUFFER;
  s_tx[1] = offset;
  if (xfer((uint16_t)(len + 3u)))
  {
    memcpy(data, &s_rx[3], len);
  }
  else
  {
    memset(data, 0, len);
  }
}

void sx1280_get_rx_buffer_status(uint8_t *len, uint8_t *offset)
{
  uint8_t s[2];
  cmd_read(SX1280_CMD_GET_RX_BUFFER_STATUS, s, 2);
  *len = s[0];
  *offset = s[1];
}

void sx1280_get_lora_packet_status(int16_t *rssi_dbm, int8_t *snr_db)
{
  uint8_t s[5];
  cmd_read(SX1280_CMD_GET_PACKET_STATUS, s, 5);
  int8_t snr = (int8_t)((int8_t)s[1] / 4);
  int16_t rssi = (int16_t)(-(int16_t)s[0] / 2);
  if (snr < 0)
  {
    rssi = (int16_t)(rssi + snr); /* datasheet: correct RSSI when below the noise floor */
  }
  *rssi_dbm = rssi;
  *snr_db = snr;
}

int16_t sx1280_get_rssi_inst(void)
{
  uint8_t v;
  cmd_read(SX1280_CMD_GET_RSSI_INST, &v, 1);
  return (int16_t)(-(int16_t)v / 2);
}

uint32_t sx1280_lora_bandwidth_hz(uint8_t bw)
{
  switch (bw)
  {
    case SX1280_LORA_BW_200:  return 203125u;
    case SX1280_LORA_BW_400:  return 406250u;
    case SX1280_LORA_BW_800:  return 812500u;
    case SX1280_LORA_BW_1600: return 1625000u;
    default:                  return 812500u;
  }
}

uint32_t sx1280_lora_time_on_air_us(const sx1280_lora_cfg_t *cfg, uint8_t payload_len,
                                    bool explicit_header, bool crc)
{
  const int32_t sf = cfg->sf >> 4;
  const int32_t cr = cfg->cr; /* 1..4 for 4/5..4/8 */
  const int32_t nh = explicit_header ? 1 : 0;
  const int32_t ncrc = crc ? 16 : 0;

  /* Count in quarter symbols to keep the fractional preamble terms exact. */
  uint32_t quarters = (uint32_t)cfg->preamble_len * 4u + ((sf <= 6) ? 25u : 17u) + 32u; /* +6.25/4.25, +8 */

  int32_t num;
  int32_t den;
  if (sf <= 6)
  {
    num = 8 * payload_len + ncrc - 4 * sf + 20 * nh;
    den = 4 * sf;
  }
  else
  {
    num = 8 * payload_len + ncrc - 4 * sf + 8 + 20 * nh;
    den = (sf >= 11) ? 4 * (sf - 2) : 4 * sf;
  }
  if (num < 0)
  {
    num = 0;
  }
  quarters += (uint32_t)(((num + den - 1) / den) * (cr + 4)) * 4u;

  const uint64_t bw = sx1280_lora_bandwidth_hz(cfg->bw);
  return (uint32_t)(((uint64_t)quarters * (1ull << sf) * 1000000ull) / (4ull * bw));
}
