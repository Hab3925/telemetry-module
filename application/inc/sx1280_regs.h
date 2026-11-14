/*
 * SX1280 command opcodes, register addresses and parameter values.
 * Source: Semtech SX1280/SX1281 datasheet rev 3.x.
 */
#ifndef SX1280_REGS_H
#define SX1280_REGS_H

/* ---- Commands -------------------------------------------------------------------------------- */
#define SX1280_CMD_GET_STATUS             0xC0
#define SX1280_CMD_WRITE_REGISTER         0x18
#define SX1280_CMD_READ_REGISTER          0x19
#define SX1280_CMD_WRITE_BUFFER           0x1A
#define SX1280_CMD_READ_BUFFER            0x1B
#define SX1280_CMD_SET_SLEEP              0x84
#define SX1280_CMD_SET_STANDBY            0x80
#define SX1280_CMD_SET_FS                 0xC1
#define SX1280_CMD_SET_TX                 0x83
#define SX1280_CMD_SET_RX                 0x82
#define SX1280_CMD_SET_PACKET_TYPE        0x8A
#define SX1280_CMD_SET_RF_FREQUENCY       0x86
#define SX1280_CMD_SET_TX_PARAMS          0x8E
#define SX1280_CMD_SET_BUFFER_BASE_ADDR   0x8F
#define SX1280_CMD_SET_MODULATION_PARAMS  0x8B
#define SX1280_CMD_SET_PACKET_PARAMS      0x8C
#define SX1280_CMD_GET_RX_BUFFER_STATUS   0x17
#define SX1280_CMD_GET_PACKET_STATUS      0x1D
#define SX1280_CMD_GET_RSSI_INST          0x1F
#define SX1280_CMD_SET_DIO_IRQ_PARAMS     0x8D
#define SX1280_CMD_GET_IRQ_STATUS         0x15
#define SX1280_CMD_CLR_IRQ_STATUS         0x97
#define SX1280_CMD_SET_REGULATOR_MODE     0x96
#define SX1280_CMD_SET_AUTO_FS            0x9E
#define SX1280_CMD_SET_TX_CW              0xD1

/* ---- Registers ------------------------------------------------------------------------------- */
#define SX1280_REG_FIRMWARE_VERSION       0x0153 /* 2 bytes */
#define SX1280_REG_RX_GAIN                0x0891 /* bits 7:6 = 0b11 -> high sensitivity mode */
#define SX1280_REG_LORA_SF_CONFIG         0x0925 /* must be set according to SF after SetModulationParams */
#define SX1280_REG_LORA_FREQ_ERR_COMP     0x093C /* must be 0x01 in LoRa mode */
#define SX1280_REG_LORA_SYNC_WORD         0x0944

/* ---- Parameters ------------------------------------------------------------------------------ */
#define SX1280_STANDBY_RC                 0x00
#define SX1280_STANDBY_XOSC               0x01

#define SX1280_REGULATOR_LDO              0x00
#define SX1280_REGULATOR_DCDC             0x01

#define SX1280_PACKET_TYPE_LORA           0x01

#define SX1280_TICK_15_625US              0x00
#define SX1280_TICK_62_5US                0x01
#define SX1280_TICK_1MS                   0x02
#define SX1280_TICK_4MS                   0x03

#define SX1280_RAMP_04_US                 0x20

/* LoRa spreading factors */
#define SX1280_LORA_SF5                   0x50
#define SX1280_LORA_SF6                   0x60
#define SX1280_LORA_SF7                   0x70
#define SX1280_LORA_SF8                   0x80
#define SX1280_LORA_SF9                   0x90
#define SX1280_LORA_SF10                  0xA0
#define SX1280_LORA_SF11                  0xB0
#define SX1280_LORA_SF12                  0xC0

/* LoRa bandwidths (actual: 203.125 / 406.25 / 812.5 / 1625 kHz) */
#define SX1280_LORA_BW_200                0x34
#define SX1280_LORA_BW_400                0x26
#define SX1280_LORA_BW_800                0x18
#define SX1280_LORA_BW_1600               0x0A

/* LoRa coding rates (long-interleaving variants intentionally not supported here) */
#define SX1280_LORA_CR_4_5                0x01
#define SX1280_LORA_CR_4_6                0x02
#define SX1280_LORA_CR_4_7                0x03
#define SX1280_LORA_CR_4_8                0x04

#define SX1280_LORA_HEADER_EXPLICIT       0x00
#define SX1280_LORA_HEADER_IMPLICIT       0x80
#define SX1280_LORA_CRC_ON                0x20
#define SX1280_LORA_CRC_OFF               0x00
#define SX1280_LORA_IQ_NORMAL             0x40
#define SX1280_LORA_IQ_INVERTED           0x00

/* IRQ bits */
#define SX1280_IRQ_TX_DONE                0x0001
#define SX1280_IRQ_RX_DONE                0x0002
#define SX1280_IRQ_SYNCWORD_VALID         0x0004
#define SX1280_IRQ_SYNCWORD_ERROR         0x0008
#define SX1280_IRQ_HEADER_VALID           0x0010
#define SX1280_IRQ_HEADER_ERROR           0x0020
#define SX1280_IRQ_CRC_ERROR              0x0040
#define SX1280_IRQ_RX_TX_TIMEOUT          0x4000
#define SX1280_IRQ_PREAMBLE_DETECTED      0x8000
#define SX1280_IRQ_ALL                    0xFFFF

/* Crystal 52 MHz, PLL step = 52e6 / 2^18 */
#define SX1280_XTAL_HZ                    52000000ULL

#endif /* SX1280_REGS_H */
