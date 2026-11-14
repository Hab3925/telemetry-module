/*
 * FHSS LoRa link between two boards (one master, one slave) with LBT before every transmission.
 *
 * Frame (one hop, period computed from the modem settings):
 *
 *   |<----------------------------------- frame_us ------------------------------------>|
 *   | hop+CCA | master TX (<= max) | turnaround+CCA | slave TX (<= max) | guard | lead |
 *   ^ frame start (master timer)                                                   ^ slave hops
 *
 * The master always transmits at the start of a hop (an empty frame if it has no data) so the slave
 * can stay synchronised. The slave only transmits as a reply to a master frame received in the
 * same hop. Every frame carries the hop index, so a slave can lock on from any channel.
 */
#ifndef RADIO_LINK_H
#define RADIO_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "dgram_queue.h"

/* Modem settings that can be changed at runtime (coordinated by the master, see radio_link_request_modem). */
typedef struct
{
  uint8_t sf;             /* SX1280_LORA_SF5 .. SX1280_LORA_SF12 */
  uint8_t bw;             /* SX1280_LORA_BW_200/400/800/1600 */
  uint8_t cr;             /* SX1280_LORA_CR_4_5 .. SX1280_LORA_CR_4_8 */
  uint8_t master_payload; /* max frame size sent by the master, bytes */
  uint8_t slave_payload;  /* max frame size sent by the slave, bytes */
} radio_modem_cfg_t;

typedef enum
{
  RADIO_SWITCH_IDLE,
  RADIO_SWITCH_ANNOUNCE,  /* master announcing / slave waiting for the switch frame */
  RADIO_SWITCH_PROBATION, /* switched, waiting for the first frame from the peer */
} radio_switch_state_t;

typedef enum
{
  RADIO_SWITCH_NONE,
  RADIO_SWITCH_PENDING,
  RADIO_SWITCH_OK,        /* link works on the new settings */
  RADIO_SWITCH_NO_ACK,    /* slave did not confirm: nothing changed */
  RADIO_SWITCH_REVERTED,  /* no link on the new settings within 5 s: back to the previous ones */
  RADIO_SWITCH_DEFAULTS,  /* long outage: back to the compile-time defaults */
  RADIO_SWITCH_REJECTED,  /* link was lost or a switch was running when the request was handled */
} radio_switch_result_t;

typedef enum
{
  RADIO_MODEM_OK,
  RADIO_MODEM_ERR_INVALID,    /* bad parameter, or a frame would be >= 60 ms on air */
  RADIO_MODEM_ERR_BUSY,       /* switch or carrier test in progress */
  RADIO_MODEM_ERR_NO_LINK,    /* the slave must be connected to confirm the switch */
  RADIO_MODEM_ERR_NOT_MASTER, /* send modem commands to the master */
} radio_modem_error_t;

typedef struct
{
  uint32_t frames_tx;
  uint32_t frames_rx;      /* valid frames from the peer */
  uint32_t frames_lost;    /* gaps in the peer's frame counter while connected */
  uint32_t rx_crc_errors;
  uint32_t rx_foreign;     /* frames with wrong link ID / same role */
  uint32_t lbt_busy;       /* transmissions skipped because the channel was busy */
  uint32_t lbt_invalid;    /* transmissions skipped because no valid RSSI could be measured */
  uint32_t tx_timeouts;
  uint32_t reply_skipped;  /* slave: master frame arrived too late in the hop to answer */
  uint32_t sync_lost;      /* slave: number of times sync was lost */
  uint32_t dgram_tx;
  uint32_t dgram_rx;
  uint32_t dgram_oversize; /* datagrams too large to fragment, dropped */
  uint32_t reassembly_errors;
  uint32_t dgram_echoed;   /* echo requests received and queued to be sent back */
  uint32_t echo_drops;     /* echo requests dropped because the echo queue was full */
  bool gen_active;         /* master: generator requested; slave: generating */
  uint32_t gen_tx_bytes;   /* generator filler sent (slave) */
  uint32_t gen_rx_bytes;   /* generator filler received (master) */
  uint32_t gen_rx_segments;
  uint32_t gen_lost;       /* gaps in the filler numbering = lost generator frames */
  int32_t rssi_sum;        /* sum of RSSI / SNR over all valid frames: average = delta sum / delta frames_rx */
  int32_t snr_sum;
  int16_t last_rssi;
  int8_t last_snr;
  int16_t last_cca_rssi;
  int16_t lbt_threshold;
  uint8_t hop;
  bool connected;
  bool radio_ok;
  uint32_t frame_us;
  uint32_t bytes_tx;       /* datagram payload bytes sent over the radio */
  uint32_t bytes_rx;       /* datagram payload bytes received and delivered */
  uint32_t capacity_bps;   /* max payload rate this node can send (bit/s) */
  bool test_cw;            /* continuous-carrier test mode active (link stopped) */
  uint32_t test_freq_khz;
  int8_t test_dbm;
  int8_t tx_dbm;           /* SX1280 output actually used: requested, limited for the TX antenna */
  int8_t tx_req_dbm;       /* requested ("power" command) */
  int8_t tx_max_dbm;       /* EIRP limit for the TX antenna */
  uint8_t ant_tx;          /* antenna port for transmitting (1..3 = J3..J5) */
  uint8_t ant_rx;          /* antenna port for receiving */
  radio_modem_cfg_t modem; /* active modem settings */
  radio_switch_state_t switch_state;
  radio_switch_result_t switch_result;
  bool rx_high_sens;       /* SX1280 LNA high sensitivity mode */
} radio_link_stats_t;

/* Create the radio thread. q_tx: datagrams to send (consumer), q_rx: received datagrams (producer).
 * rx_notify is called from the radio thread after a datagram has been committed to q_rx. */
void radio_link_start(dgram_queue_t *q_tx, dgram_queue_t *q_rx, void (*rx_notify)(void));

/* Largest datagram this node can send (fragment limit). */
uint32_t radio_link_max_datagram(void);

void radio_link_get_stats(radio_link_stats_t *out);

/* Master: per channel 0..39 (FHSS_CHANNEL_COUNT, frequency order) frames sent, replies received and
 * transmissions skipped by LBT, since boot. Arrays must hold FHSS_CHANNEL_COUNT entries. */
void radio_link_get_channel_stats(uint32_t tx[], uint32_t rx[], uint32_t busy[]);

/* Test mode for conducted power measurements: stop the link and transmit an unmodulated carrier.
 * freq 2401..2482 MHz, SX1280 output -18..+5 dBm, stops by itself after timeout_s (1..600 s).
 * Only use with an attenuator or load on the antenna port: a continuous carrier does not do LBT.
 * Returns false if a parameter is out of range. Calling again while active updates the settings. */
bool radio_link_request_cw(uint32_t freq_hz, int8_t dbm, uint32_t timeout_s);
void radio_link_request_stop(void);

/* Request a link TX power (-18..+5 dBm SX1280 output) until reboot. The output used is additionally limited
 * for the TX antenna so the EIRP stays within the limit (stats: tx_dbm, tx_max_dbm). */
bool radio_link_set_tx_power(int8_t dbm);

/* Use tx_port for transmitting (and its LBT check) and rx_port for receiving (1..3 = J3..J5, see the antenna
 * table in app_config.h). The TX power is limited for the TX antenna's gain. Until reboot. */
bool radio_link_set_antennas(uint8_t tx_port, uint8_t rx_port);

/* Antenna table entry for status reports. max_dbm = highest SX1280 setting within the EIRP limit. */
bool radio_link_antenna_info(uint8_t port, const char **name, int16_t *gain_x10, int8_t *max_dbm, bool *tx_ok);

/* Switch the SX1280 LNA between normal (default) and high sensitivity mode until reboot (diagnostics only:
 * high sensitivity mode makes RSSI and the LBT measurement read ~55 dB too low for moderate signals). */
bool radio_link_set_rx_high_sensitivity(bool enable);

/* Master only: ask the slave to fill all its frames with numbered dummy data (slave -> master throughput
 * test). The master counts it (gen_rx_bytes, gen_lost) and drops it. Stops when disabled or the link drops. */
bool radio_link_set_generator(bool enable);

/* Master only: change the modem settings on both boards. The master announces the new settings over
 * the link for about 1 s, the slave confirms, and both switch at the same hop. If no link is
 * established on the new settings within 5 s, both go back to the previous ones. If the link is down
 * for 30 s, the master goes back to the compile-time defaults (the searching slave also tries them).
 * Settings last until reboot. Progress is reported in the stats (switch_state, switch_result). */
radio_modem_error_t radio_link_request_modem(const radio_modem_cfg_t *cfg);

/* Validate settings and report the resulting timing without applying anything. */
bool radio_link_modem_preview(const radio_modem_cfg_t *cfg, uint32_t *frame_us, uint32_t *toa_master_us,
                              uint32_t *toa_slave_us);

#endif /* RADIO_LINK_H */
