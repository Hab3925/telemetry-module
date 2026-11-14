/*
 * Application configuration for the 2026 Telemetry Module (Ethernet <-> SX1280 LoRa bridge).
 *
 * Two boards form one link:
 *   MASTER  (APP_ROLE_MASTER = 1): owns the hop clock, transmits one frame at the start of every hop.
 *   SLAVE   (APP_ROLE_MASTER = 0): synchronises to the master and answers in the same hop.
 * Select the role at configure time:  cmake --preset Debug -DTELEMETRY_ROLE=MASTER|SLAVE
 * Both boards must be built with the same link settings (bind phrase, modem, payload sizes).
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "sx1280_regs.h"

#ifndef APP_ROLE_MASTER
#define APP_ROLE_MASTER 1
#endif

/* ============================== Ethernet / UDP ============================================== */

/* Each board needs its own IP address (overrides the CubeMX default 192.168.1.50). */
#if APP_ROLE_MASTER
#define APP_IP_ADDRESS        IP_ADDRESS(192, 168, 1, 50)
#else
#define APP_IP_ADDRESS        IP_ADDRESS(192, 168, 1, 51)
#endif
#define APP_NET_MASK          IP_ADDRESS(255, 255, 255, 0)

/* Every UDP datagram received on this port is sent over the radio. */
#define APP_UDP_LISTEN_PORT   5000u
/* Every datagram received over the radio is sent to this address/port (from APP_UDP_LISTEN_PORT).
 * 255.255.255.255 = broadcast on the local segment; set a unicast address once the peer is known. */
#define APP_UDP_DEST_IP       IP_ADDRESS(255, 255, 255, 255)
#define APP_UDP_DEST_PORT     5001u
/* Datagrams sent to this port go over the radio and the far board sends them straight back (they come
 * out on APP_UDP_DEST_PORT here). Used by tools/throughput_test.py with only one board on Ethernet. */
#define APP_UDP_ECHO_PORT     5003u
/* Send any datagram to this port to get a one-line text status report back. */
#define APP_UDP_STATUS_PORT   5002u

#define APP_DGRAM_MAX         1472u /* largest UDP payload in a single 1500 byte Ethernet frame */
#define APP_DGRAM_QUEUE_DEPTH 8u    /* datagrams buffered in each direction */

/* ============================== Radio link ================================================== */

/* Seeds the hop sequence and the link ID. Both ends must match; pick something unique per system. */
#define APP_BIND_PHRASE       "propulse-fossekall-2026"

/* Default modem settings, used at power-up and as the fallback: if the link is down for 30 s on
 * settings changed at runtime (control panel / "modem" command), both boards return to these.
 * They are therefore chosen for maximum range within the 60 ms per-transmission limit:
 * SF9 / 812 kHz / CR4/5 is ~5 dB more sensitive than SF7 / 812 kHz (about 1.8x the range).
 * Small master frames (commands, ~1.8 kbit/s up) and larger slave frames (telemetry, ~4.1 kbit/s
 * down), ~104 ms hop. Switch to faster settings at runtime once the link is up. */
#define APP_LORA_SF           SX1280_LORA_SF9
#define APP_LORA_BW           SX1280_LORA_BW_800
#define APP_LORA_CR           SX1280_LORA_CR_4_5
#define APP_LORA_PREAMBLE     12u

/* Maximum radio frame size (bytes, 32..255) sent by each side. UDP datagrams larger than one frame
 * are fragmented (max 16 fragments), small datagrams are packed several per frame.
 * Each frame must stay under 60 ms on air: at SF9 / 812 kHz that is at most 62 bytes. */
#define APP_MASTER_MAX_PAYLOAD 32u
#define APP_SLAVE_MAX_PAYLOAD  62u

/* SX1280 output power requested at power-up, dBm (-18..+5; the RFX2401C PA input must stay <= +5 dBm).
 * Can be changed at runtime ("power" command / control panel). Whatever is requested, the output is
 * limited per antenna port so the EIRP stays within the limit (see the antenna table below). */
#define APP_SX1280_TX_POWER_DBM (-18)

/* ============================== EN 300 328 adaptivity (LBT) ================================= */

/* Adaptive FHSS using LBT (clause 4.3.1.7.2): a clear channel assessment (energy detect) is made
 * before every transmission; if the channel is busy we do not transmit in that hop. */
#define APP_LBT_ENABLED       1
/* CCA observation time. Must be >= 18 us and >= 0.2 % of the channel occupancy time (60 ms max). */
#define APP_LBT_CCA_US        150u
/* Detection threshold: -70 dBm/MHz at the receiver input for 20 dBm EIRP, referred to a 0 dBi antenna.
 * The SX1280 sees the signal after the antenna gain and the front end LNA, so the threshold at the
 * chip is raised by those gains. Margin makes the check more conservative (lower threshold). */
#define APP_LBT_THRESHOLD_DBM_PER_MHZ (-70)
#define APP_LBT_RX_PATH_GAIN_DB       9  /* ESTIMATE: RFX2401C LNA (+12 dB) minus switch, LPF and bandpass losses */
#define APP_LBT_MARGIN_DB             3

/* ============================== Temperature (NTC R25) ======================================= */

/* NTC divider values. R25 is a TDK/EPCOS B57332V5103F360: 10 kOhm +-1 % at 25 degC, B25/50 = 3380 K.
 * B25/50 is the best fit between 25 and 50 degC. Higher up the reading may drift slightly high
 * (around 1 degC at 85 degC if the part's B25/85 is ~1.5 % larger), which is fine for monitoring. */
#define APP_NTC_FIXED_OHM     10000u /* R24, from 3V3 */
#define APP_NTC_R25_OHM       10000u /* R25 at 25 degC */
#define APP_NTC_BETA          3380u

/* ============================== Antennas and EIRP limit ===================================== */

/* EN 300 328: max 20 dBm EIRP. For the antenna port that transmits, the firmware limits the SX1280 output to
 *   setting + TX chain gain + antenna gain <= EIRP limit - margin
 * regardless of the requested power. The LBT threshold uses the same antenna's gain.
 * TX chain gain: measured with the BB60D, SX1280 setting -> J3 = +17 dB (linear up to the -8 dBm setting;
 * compression can only lower the output, so this is an upper bound above that).
 * Margin 1 dB: with the Moxon this allows -3 dBm (18.2 dBm EIRP). The analyzer is +-2 dB, so a 2 dB margin
 * (-4 dBm on the Moxon) is the strictly safe choice. */
#define APP_EIRP_LIMIT_DBM        20
#define APP_EIRP_MARGIN_DB_X10    10   /* 1.0 dB */
#define APP_TX_CHAIN_GAIN_DB_X10  170  /* 17.0 dB */

/* What is connected to each antenna port: name (no spaces) and peak gain in 0.1 dBi.
 * Port 1 = J3, 2 = J4, 3 = J5. APP_ANT_UNUSED = nothing connected: the port is never used.
 * Each board uses this table, so describe the antennas of both boards conservatively (highest gain). */
#define APP_ANT_UNUSED            (-1000)
#define APP_ANT1_NAME             "Moxon"
#define APP_ANT1_GAIN_X10         42     /* Happymodel Moxon, 4.18 dBi */
#define APP_ANT2_NAME             "Yagi"
#define APP_ANT2_GAIN_X10         100    /* Yagi, 57 cm, 10 dBi per its manual: max -8 dBm SX1280 (19 dBm EIRP) */
#define APP_ANT3_NAME             "none"
#define APP_ANT3_GAIN_X10         APP_ANT_UNUSED

/* Ports used after power-up (can be changed at runtime: "antenna <tx> <rx>" / control panel). */
#define APP_ANT_TX_PORT           1u
#define APP_ANT_RX_PORT           1u

#endif /* APP_CONFIG_H */
