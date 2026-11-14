#include "radio_link.h"

#include <string.h>

#include "tx_api.h"
#include "main.h"
#include "app_config.h"
#include "board.h"
#include "fhss.h"
#include "radio_timer.h"
#include "sx1280.h"
#include "util.h"

/* ---- Over-the-air format ---------------------------------------------------------------------
 * Frame:   [link_id:16][flags:8][hop:8][seq:8][byte5:8] [control block]? [segment]...
 *          flags: bit0 = sent by master, bit1 = control block follows, bit2 = modem switch ack,
 *                 bit3 = master asks the slave to run its traffic generator, bits 7..4 = protocol version
 *          byte5: master = tx_offset, time from frame start to TX start in 8 us units (slave timing)
 *                 slave  = id of the acknowledged modem switch (with the ack flag)
 * Control: [countdown:8][id:8][sf:8][bw:8][cr:8][master_payload:8][slave_payload:8]
 *          master announces a modem switch; both sides switch at the start of the frame
 *          countdown + 1 frames after this one.
 * Segment: [dgram_id:8][frag_index:4 | rsvd:2 | echo:1 | last:1][len:8][data:len]
 *          echo: the receiver sends the datagram straight back instead of delivering it
 *          gen (bit 2): generator filler [gen_seq:32][dummy bytes], counted by the receiver and dropped
 * A datagram is split into up to 16 fragments; several datagrams may share one frame.
 * LoRa explicit header + hardware CRC16 protect each frame. */
#define LINK_VERSION        2u
#define LINK_HDR_LEN        6u
#define LINK_CTRL_LEN       7u
#define LINK_HDR_MAX        (LINK_HDR_LEN + LINK_CTRL_LEN)
#define LINK_FLAG_MASTER    0x01u
#define LINK_FLAG_CTRL      0x02u
#define LINK_FLAG_ACK       0x04u
#define LINK_FLAG_GEN       0x08u
#define SEG_HDR_LEN         3u
#define SEG_FLAG_LAST       0x01u
#define SEG_FLAG_ECHO       0x02u
#define SEG_FLAG_GEN        0x04u
#define GEN_MIN_SEG         8u    /* only add filler if at least this much room is left */
#define MAX_FRAGMENTS       16u
#define MIN_FRAGMENT        16u /* don't start a datagram in a frame with less room than this */
#define MIN_PAYLOAD         (LINK_HDR_MAX + SEG_HDR_LEN + MIN_FRAGMENT)

/* ---- Timing (us) ----------------------------------------------------------------------------- */
#define RX_SWITCH_US        60u   /* SX1280 switch into RX */
#define RSSI_MARGIN_US      30u
#define RSSI_INVALID_DBM    (-127) /* GetRssiInst reads 0xFE/0xFF until a measurement is available */
#define TX_LOAD_US          100u  /* CCA end -> TX start (buffer write, SetTx, PLL) */
#define HOP_US              200u  /* standby, SetRfFrequency, SetRx over SPI */
#define TURNAROUND_EXTRA_US 300u  /* master TX end -> slave CCA start (IRQ, thread wake, readout) */
#define GUARD_US            1000u
#define SLAVE_HOP_LEAD_US   500u  /* slave changes channel this much before the master's frame start */
#define RX_DONE_LATENCY_US  50u   /* end of TX at master -> RX_DONE edge at slave (approximate) */
#define MAX_COT_US          60000u /* EN 300 328 LBT: channel occupancy time < 60 ms */
#define MAX_MISSED_FRAMES   25u
#define LINK_LOST_MIN_US    2000000u  /* ...and at least this long: short fades at fast settings ride through */
#define TX_TIMEOUT_MARGIN_MS 5u

/* ---- Modem switching ------------------------------------------------------------------------- */
#define SWITCH_ANNOUNCE_US  1000000u  /* announce a switch for about this long (at least 8 frames) */
#define PROBATION_US        5000000u  /* new settings must give a link within this time, else revert */
#define DEFAULT_REVERT_US   30000000u /* master: no link for this long -> back to the compile-time defaults */
#define SEARCH_GOOD_ONLY_US 35000000u /* slave: search only on the last working settings this long before also
                                       * trying the defaults (longer than DEFAULT_REVERT_US, so it still meets a
                                       * master that went back to the defaults) */

_Static_assert(APP_MASTER_MAX_PAYLOAD <= 255u && APP_SLAVE_MAX_PAYLOAD <= 255u, "LoRa payload is max 255 bytes");
_Static_assert(APP_MASTER_MAX_PAYLOAD >= MIN_PAYLOAD, "payload too small");
_Static_assert(APP_SLAVE_MAX_PAYLOAD >= MIN_PAYLOAD, "payload too small");
#define PA_INPUT_MAX_DBM    5     /* RFX2401C absolute max input */
#define CW_MIN_HZ           2401000000u /* keep the carrier well inside 2400-2483.5 MHz */
#define CW_MAX_HZ           2482000000u
#define CW_MIN_DBM          (-18)
#define CW_MAX_TIMEOUT_S    600u

_Static_assert(APP_SX1280_TX_POWER_DBM <= PA_INPUT_MAX_DBM, "RFX2401C PA input must not exceed +5 dBm");
_Static_assert(APP_LBT_CCA_US >= 120u, "CCA must be >= 0.2 % of the 60 ms maximum COT");

#define EV_DIO1             0x1u
#define EV_TIMER            0x2u
#define EV_CMD              0x4u
#define EV_POWER            0x8u
#define EV_RXGAIN           0x10u
#define EV_MODEM            0x20u
#define EV_ANT              0x40u

#define RADIO_THREAD_PRIO   3u
#define RADIO_STACK_SIZE    2048u

#define DIO1_IRQS (SX1280_IRQ_TX_DONE | SX1280_IRQ_RX_DONE | SX1280_IRQ_CRC_ERROR | \
                   SX1280_IRQ_HEADER_ERROR | SX1280_IRQ_RX_TX_TIMEOUT)

typedef enum
{
  RS_IDLE,
  RS_RX,
  RS_TX,
} radio_state_t;

/* Timing derived from a modem configuration */
typedef struct
{
  uint32_t frame_us;
  uint32_t toa_master_us;
  uint32_t toa_slave_us;
  uint32_t rssi_valid_us;
  uint32_t reply_prep_us;
  int16_t lbt_base;      /* LBT threshold at the SX1280 for a 0 dBi antenna */
} modem_timing_t;

static TX_THREAD s_thread;
static ULONG s_stack[RADIO_STACK_SIZE / sizeof(ULONG)];
static TX_EVENT_FLAGS_GROUP s_events;
static volatile bool s_ready;
static volatile uint32_t s_dio1_time;

static dgram_queue_t *s_q_tx;
static dgram_queue_t *s_q_rx;
static void (*s_rx_notify)(void);

static const radio_modem_cfg_t s_cfg_default = {
    .sf = APP_LORA_SF,
    .bw = APP_LORA_BW,
    .cr = APP_LORA_CR,
    .master_payload = APP_MASTER_MAX_PAYLOAD,
    .slave_payload = APP_SLAVE_MAX_PAYLOAD,
};
static radio_modem_cfg_t s_cfg;      /* active */
static radio_modem_cfg_t s_cfg_good; /* last configuration that gave a link: fallback */
static sx1280_lora_cfg_t s_lora = {.preamble_len = APP_LORA_PREAMBLE};

static uint16_t s_link_id;
static uint32_t s_frame_us;
static uint32_t s_toa_own_max;
static int16_t s_lbt_threshold;
static int16_t s_lbt_base;        /* threshold for a 0 dBi antenna; + gain of the TX antenna = s_lbt_threshold */
static uint32_t s_rssi_valid_us;  /* RX start -> first valid instantaneous RSSI (~1 LoRa symbol) */
static uint32_t s_reply_prep_us;  /* slave: RX done -> TX start */

static radio_state_t s_state;
static uint8_t s_hop;
static uint8_t s_tx_seq;
static uint32_t s_frame_start;    /* start of the current frame (master time base) */
static uint32_t s_timer_deadline;
static uint32_t s_missed;         /* consecutive frames without a valid frame from the peer */
#if !APP_ROLE_MASTER
static uint32_t s_search_start;   /* slave: when the current search began */
#endif

/* Frames without a valid frame from the peer before the link counts as lost: 25 hops, but at least 2 s. */
static uint32_t max_missed(void)
{
  const uint32_t frames = LINK_LOST_MIN_US / s_frame_us;
  return (frames > MAX_MISSED_FRAMES) ? frames : MAX_MISSED_FRAMES;
}
static bool s_rx_this_frame;
#if APP_ROLE_MASTER
static uint32_t s_last_link_time; /* master: last time a slave frame was received */
#else
static bool s_synced;             /* slave: following the master's hop clock */
#endif
static bool s_have_peer_seq;
static uint8_t s_peer_seq;

/* Modem switch in progress */
static struct
{
  radio_switch_state_t state;
  radio_modem_cfg_t cfg;   /* announced configuration */
  uint8_t id;
  uint8_t countdown;       /* frames until the switch */
  bool acked;              /* master: slave confirmed the announcement */
  uint32_t probation_start;
} s_sw;
#if APP_ROLE_MASTER
static uint8_t s_next_switch_id = 1;
static volatile radio_modem_cfg_t s_modem_req; /* from the net thread, published by EV_MODEM */
#endif

static uint8_t s_txbuf[256];
static uint8_t s_rxbuf[256];

/* Datagrams to send back (echo test); produced and consumed by the radio thread only */
static dgram_queue_t s_q_echo;

static struct
{
  dgram_t *d;
  dgram_queue_t *q; /* queue the datagram came from */
  uint16_t off;
  uint8_t id;
  uint8_t frag;
} s_txd;
static uint8_t s_next_dgram_id;

static struct
{
  bool active;
  dgram_t *d; /* NULL: receive queue was full, datagram is discarded */
  dgram_queue_t *q; /* s_q_rx, or s_q_echo for datagrams to send back */
  uint8_t id;
  uint8_t next_frag;
} s_rxd;

static radio_link_stats_t s_stats;

/* Master: per channel (frequency order) frames sent, replies received and transmissions skipped by LBT.
 * Reply success = both directions worked on that channel. */
static uint32_t s_chan_tx[FHSS_CHANNEL_COUNT];
static uint32_t s_chan_rx[FHSS_CHANNEL_COUNT];
static uint32_t s_chan_busy[FHSS_CHANNEL_COUNT];

/* Test command handed over from the net thread (published by the EV_CMD event flag) */
static struct
{
  bool cw;
  uint32_t freq_hz;
  int8_t dbm;
  uint32_t timeout_s;
} s_cmd;
static bool s_test_mode;
#if APP_ROLE_MASTER
static volatile bool s_gen_request;  /* ask the slave to generate (set by the "gen" command) */
#else
static bool s_gen_active;            /* generating because the master asks for it */
static uint32_t s_gen_tx_seq;
#endif
static uint32_t s_gen_rx_seq;
static bool s_gen_have_seq;
static volatile bool s_rx_high_sens = false; /* see sx1280_init(): high sensitivity breaks RSSI/LBT */
static volatile int8_t s_link_dbm = APP_SX1280_TX_POWER_DBM; /* requested TX power ("power" command) */

/* ---- Antenna ports ---------------------------------------------------------------------------- */
typedef struct
{
  const char *name;
  int16_t gain_x10; /* peak gain, 0.1 dBi; APP_ANT_UNUSED = nothing connected */
} antenna_t;

static const antenna_t s_antennas[3] = {
    {APP_ANT1_NAME, APP_ANT1_GAIN_X10},
    {APP_ANT2_NAME, APP_ANT2_GAIN_X10},
    {APP_ANT3_NAME, APP_ANT3_GAIN_X10},
};
static uint8_t s_ant_tx = APP_ANT_TX_PORT;
static uint8_t s_ant_rx = APP_ANT_RX_PORT;
static volatile uint8_t s_ant_req_tx;
static volatile uint8_t s_ant_req_rx;

static int32_t floor_div10(int32_t v)
{
  return (v >= 0) ? (v / 10) : -((-v + 9) / 10);
}

static bool ant_connected(uint8_t port)
{
  return (port >= 1u) && (port <= 3u) && (s_antennas[port - 1u].gain_x10 > APP_ANT_UNUSED);
}

/* Highest SX1280 setting on this port that keeps the EIRP within the limit (may be below -18: not usable). */
static int32_t ant_power_limit(uint8_t port)
{
  const int32_t x10 = APP_EIRP_LIMIT_DBM * 10 - APP_EIRP_MARGIN_DB_X10 - APP_TX_CHAIN_GAIN_DB_X10 -
                      s_antennas[port - 1u].gain_x10;
  const int32_t db = floor_div10(x10);
  return (db > PA_INPUT_MAX_DBM) ? PA_INPUT_MAX_DBM : db;
}

static bool ant_tx_allowed(uint8_t port)
{
  return ant_connected(port) && (ant_power_limit(port) >= CW_MIN_DBM);
}

_Static_assert((APP_ANT_TX_PORT >= 1u) && (APP_ANT_TX_PORT <= 3u) && (APP_ANT_RX_PORT >= 1u) &&
               (APP_ANT_RX_PORT <= 3u), "antenna ports are 1..3");

/* Apply the requested TX power, limited for the TX antenna (radio thread). */
static void apply_tx_power(void)
{
  const int32_t limit = ant_power_limit(s_ant_tx);
  const int8_t eff = (int8_t)((s_link_dbm < limit) ? s_link_dbm : limit);
  sx1280_set_tx_power(eff);
  s_stats.tx_dbm = eff;
  s_stats.tx_req_dbm = s_link_dbm;
  s_stats.tx_max_dbm = (int8_t)limit;
}

/* LBT threshold for the TX antenna: the standard allows correcting for its gain (rounded down). */
static void update_lbt_threshold(void)
{
  s_lbt_threshold = (int16_t)(s_lbt_base + floor_div10(s_antennas[s_ant_tx - 1u].gain_x10));
  s_stats.lbt_threshold = s_lbt_threshold;
}

/* ---- ISR hooks ------------------------------------------------------------------------------ */

void rtimer_callback(void)
{
  if (s_ready)
  {
    tx_event_flags_set(&s_events, EV_TIMER, TX_OR);
  }
}

void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == RADIO_DIO1_Pin)
  {
    s_dio1_time = rtimer_now();
    if (s_ready)
    {
      tx_event_flags_set(&s_events, EV_DIO1, TX_OR);
    }
  }
}

/* ---- Modem configuration -------------------------------------------------------------------- */

static bool cfg_equal(const radio_modem_cfg_t *a, const radio_modem_cfg_t *b)
{
  return (a->sf == b->sf) && (a->bw == b->bw) && (a->cr == b->cr) &&
         (a->master_payload == b->master_payload) && (a->slave_payload == b->slave_payload);
}

static uint8_t own_payload(void)
{
  return APP_ROLE_MASTER ? s_cfg.master_payload : s_cfg.slave_payload;
}

/* Validate a configuration and derive its timing. Pure function: safe from any thread. */
static bool modem_timing(const radio_modem_cfg_t *cfg, modem_timing_t *t)
{
  const bool sf_ok = (cfg->sf >= SX1280_LORA_SF5) && (cfg->sf <= SX1280_LORA_SF12) && ((cfg->sf & 0x0Fu) == 0u);
  const bool bw_ok = (cfg->bw == SX1280_LORA_BW_200) || (cfg->bw == SX1280_LORA_BW_400) ||
                     (cfg->bw == SX1280_LORA_BW_800) || (cfg->bw == SX1280_LORA_BW_1600);
  const bool cr_ok = (cfg->cr >= SX1280_LORA_CR_4_5) && (cfg->cr <= SX1280_LORA_CR_4_8);
  const bool pl_ok = (cfg->master_payload >= MIN_PAYLOAD) && (cfg->slave_payload >= MIN_PAYLOAD);
  if (!sf_ok || !bw_ok || !cr_ok || !pl_ok)
  {
    return false;
  }

  const sx1280_lora_cfg_t lora = {.sf = cfg->sf, .bw = cfg->bw, .cr = cfg->cr, .preamble_len = APP_LORA_PREAMBLE};
  t->toa_master_us = sx1280_lora_time_on_air_us(&lora, cfg->master_payload, true, true);
  t->toa_slave_us = sx1280_lora_time_on_air_us(&lora, cfg->slave_payload, true, true);
  if ((t->toa_master_us >= MAX_COT_US) || (t->toa_slave_us >= MAX_COT_US))
  {
    return false; /* a single frame would exceed the 60 ms channel occupancy time */
  }

  /* The instantaneous RSSI is only valid about one symbol after entering RX. */
  const uint32_t symbol_us = (uint32_t)(((1ull << (cfg->sf >> 4)) * 1000000ull) / sx1280_lora_bandwidth_hz(cfg->bw));
  t->rssi_valid_us = RX_SWITCH_US + symbol_us + RSSI_MARGIN_US;
#if APP_LBT_ENABLED
  const uint32_t cca_us = t->rssi_valid_us + APP_LBT_CCA_US;
#else
  const uint32_t cca_us = 0;
#endif
  t->reply_prep_us = cca_us + TX_LOAD_US + 100u;
  t->frame_us = (HOP_US + cca_us + TX_LOAD_US) + t->toa_master_us +        /* master slot */
                (TURNAROUND_EXTRA_US + t->reply_prep_us) + t->toa_slave_us + /* slave slot */
                GUARD_US + SLAVE_HOP_LEAD_US;
  t->frame_us = (t->frame_us + 99u) / 100u * 100u;

  /* -70 dBm/MHz integrated over the LoRa bandwidth, referred to the SX1280 input. */
  int16_t bw_db;
  switch (cfg->bw)
  {
    case SX1280_LORA_BW_200:  bw_db = -7; break;
    case SX1280_LORA_BW_400:  bw_db = -4; break;
    case SX1280_LORA_BW_1600: bw_db = 2;  break;
    default:                  bw_db = -1; break;
  }
  t->lbt_base = (int16_t)(APP_LBT_THRESHOLD_DBM_PER_MHZ + bw_db + APP_LBT_RX_PATH_GAIN_DB - APP_LBT_MARGIN_DB);
  return true;
}

/* Make cfg the active configuration (radio thread only). cfg must have passed modem_timing(). */
static void apply_modem(const radio_modem_cfg_t *cfg)
{
  modem_timing_t t;
  (void)modem_timing(cfg, &t);

  s_cfg = *cfg;
  s_lora.sf = cfg->sf;
  s_lora.bw = cfg->bw;
  s_lora.cr = cfg->cr;
  s_frame_us = t.frame_us;
  s_toa_own_max = APP_ROLE_MASTER ? t.toa_master_us : t.toa_slave_us;
  s_rssi_valid_us = t.rssi_valid_us;
  s_reply_prep_us = t.reply_prep_us;
  s_lbt_base = t.lbt_base;
  update_lbt_threshold();

  sx1280_set_standby(SX1280_STANDBY_XOSC);
  sx1280_set_lora_modulation(&s_lora); /* packet parameters are set by listen()/transmit() */

  s_stats.modem = *cfg;
  s_stats.frame_us = s_frame_us;
  /* One full frame of payload per hop (one segment header, large datagrams) */
  s_stats.capacity_bps = (uint32_t)(((uint64_t)(own_payload() - LINK_HDR_LEN - SEG_HDR_LEN) * 8u * 1000000u) /
                                    s_frame_us);
}

static void set_switch_state(radio_switch_state_t state)
{
  s_sw.state = state;
  s_stats.switch_state = state;
}

static void switch_done(radio_switch_result_t result)
{
  s_stats.switch_result = result;
  set_switch_state(RADIO_SWITCH_IDLE);
}

/* ---- Helpers -------------------------------------------------------------------------------- */

static void schedule(uint32_t t)
{
  s_timer_deadline = t;
  rtimer_schedule(t);
}

static uint32_t toa_us(uint8_t len)
{
  return sx1280_lora_time_on_air_us(&s_lora, len, true, true);
}

static void listen(uint8_t hop)
{
  board_antenna_select(s_ant_rx);
  sx1280_set_standby(SX1280_STANDBY_XOSC);
  sx1280_set_frequency_hz(fhss_freq_hz(hop));
  sx1280_set_lora_packet(&s_lora, 255, true, true);
  board_fe_rx();
  sx1280_set_rx(SX1280_TICK_15_625US, 0xFFFF); /* continuous */
  s_hop = hop;
  s_state = RS_RX;
}

/* Energy-detect CCA on the current channel. Radio must be in RX. */
static bool channel_clear(void)
{
#if APP_LBT_ENABLED
  rtimer_delay_us(s_rssi_valid_us);
  int16_t peak = -128;
  uint32_t valid = 0;
  const uint32_t start = rtimer_now();
  do
  {
    const int16_t rssi = sx1280_get_rssi_inst();
    if (rssi > RSSI_INVALID_DBM)
    {
      valid++;
      if (rssi > peak)
      {
        peak = rssi;
      }
    }
  } while ((rtimer_now() - start) < APP_LBT_CCA_US);

  s_stats.last_cca_rssi = (valid > 0u) ? peak : RSSI_INVALID_DBM;
  if (valid == 0u)
  {
    s_stats.lbt_invalid++; /* no measurement: treat as busy, never transmit unchecked */
    return false;
  }
  if (peak >= s_lbt_threshold)
  {
    s_stats.lbt_busy++;
    return false;
  }
#endif
  return true;
}

static void transmit(uint8_t len)
{
  sx1280_set_standby(SX1280_STANDBY_XOSC);
  sx1280_write_buffer(0, s_txbuf, len);
  sx1280_set_lora_packet(&s_lora, len, true, true);
  board_antenna_select(s_ant_tx);
  board_fe_tx();
  sx1280_set_tx(SX1280_TICK_1MS, (uint16_t)(toa_us(len) / 1000u + TX_TIMEOUT_MARGIN_MS));
  s_state = RS_TX;
  s_stats.frames_tx++;
}

/* LBT before a transmission, measured on the antenna that will transmit. Radio must be in RX. */
static bool clear_to_send(void)
{
  if (s_ant_tx != s_ant_rx)
  {
    board_antenna_select(s_ant_tx);
  }
  const bool clear = channel_clear();
  if (!clear && (s_ant_tx != s_ant_rx))
  {
    board_antenna_select(s_ant_rx); /* not transmitting: keep listening on the RX antenna */
  }
  return clear;
}

uint32_t radio_link_max_datagram(void)
{
  /* Worst case: every frame carries a control block */
  const uint32_t max = (MAX_FRAGMENTS - 1u) * (own_payload() - LINK_HDR_MAX - SEG_HDR_LEN) + MIN_FRAGMENT;
  return (max < APP_DGRAM_MAX) ? max : APP_DGRAM_MAX;
}

/* Fill s_txbuf with header (+ control block) + as much queued data as fits.
 * Only call when the frame will be sent. */
static uint8_t build_frame(uint8_t max_len)
{
  uint8_t flags = (uint8_t)(LINK_VERSION << 4);
  uint32_t pos = LINK_HDR_LEN;
  s_txbuf[5] = 0;
#if APP_ROLE_MASTER
  flags |= LINK_FLAG_MASTER;
  if (s_gen_request)
  {
    flags |= LINK_FLAG_GEN;
  }
  if (s_sw.state == RADIO_SWITCH_ANNOUNCE)
  {
    flags |= LINK_FLAG_CTRL;
    s_txbuf[pos++] = s_sw.countdown;
    s_txbuf[pos++] = s_sw.id;
    s_txbuf[pos++] = s_sw.cfg.sf;
    s_txbuf[pos++] = s_sw.cfg.bw;
    s_txbuf[pos++] = s_sw.cfg.cr;
    s_txbuf[pos++] = s_sw.cfg.master_payload;
    s_txbuf[pos++] = s_sw.cfg.slave_payload;
  }
#else
  if (s_sw.state == RADIO_SWITCH_ANNOUNCE)
  {
    flags |= LINK_FLAG_ACK;
    s_txbuf[5] = s_sw.id;
  }
#endif
  s_txbuf[0] = (uint8_t)(s_link_id >> 8);
  s_txbuf[1] = (uint8_t)s_link_id;
  s_txbuf[2] = flags;
  s_txbuf[3] = s_hop;
  s_txbuf[4] = s_tx_seq++;

  for (;;)
  {
    if (s_txd.d == NULL)
    {
      dgram_queue_t *q = &s_q_echo; /* echo replies first */
      dgram_t *d = dq_peek(q);
      if (d == NULL)
      {
        q = s_q_tx;
        d = dq_peek(q);
      }
      if (d == NULL)
      {
        break;
      }
      if (d->len > radio_link_max_datagram())
      {
        dq_release(q);
        s_stats.dgram_oversize++;
        continue;
      }
      s_txd.d = d;
      s_txd.q = q;
      s_txd.off = 0;
      s_txd.id = s_next_dgram_id++;
      s_txd.frag = 0;
    }

    const uint32_t space = max_len - pos;
    if (space < SEG_HDR_LEN + 1u)
    {
      break;
    }
    const uint32_t remaining = (uint32_t)s_txd.d->len - s_txd.off;
    const uint32_t chunk = (remaining < space - SEG_HDR_LEN) ? remaining : space - SEG_HDR_LEN;
    const bool last = (chunk == remaining);
    if (!last && (chunk < MIN_FRAGMENT))
    {
      break; /* start it in the next frame instead */
    }
    if (!last && (s_txd.frag >= MAX_FRAGMENTS - 1u))
    {
      /* Only possible when the frame size shrank mid-datagram (modem switch): drop it. */
      dq_release(s_txd.q);
      s_txd.d = NULL;
      s_stats.dgram_oversize++;
      continue;
    }

    s_txbuf[pos++] = s_txd.id;
    s_txbuf[pos++] = (uint8_t)((s_txd.frag << 4) | (last ? SEG_FLAG_LAST : 0u) |
                               (((s_txd.d->flags & DGRAM_FLAG_ECHO) != 0u) ? SEG_FLAG_ECHO : 0u));
    s_txbuf[pos++] = (uint8_t)chunk;
    memcpy(&s_txbuf[pos], &s_txd.d->data[s_txd.off], chunk);
    pos += chunk;
    s_stats.bytes_tx += chunk;
    s_txd.off = (uint16_t)(s_txd.off + chunk);
    s_txd.frag++;

    if (last)
    {
      dq_release(s_txd.q);
      s_txd.d = NULL;
      s_stats.dgram_tx++;
    }
  }
#if !APP_ROLE_MASTER
  /* Traffic generator: fill the rest of the frame with numbered dummy data (real data goes first). */
  if (s_gen_active && (max_len - pos >= SEG_HDR_LEN + GEN_MIN_SEG))
  {
    const uint32_t n = max_len - pos - SEG_HDR_LEN;
    s_txbuf[pos++] = 0;
    s_txbuf[pos++] = (uint8_t)(SEG_FLAG_GEN | SEG_FLAG_LAST);
    s_txbuf[pos++] = (uint8_t)n;
    memcpy(&s_txbuf[pos], &s_gen_tx_seq, 4);
    memset(&s_txbuf[pos + 4u], 0x55, n - 4u);
    pos += n;
    s_gen_tx_seq++;
    s_stats.gen_tx_bytes += n;
  }
#endif
  return (uint8_t)pos;
}

static void process_segments(uint32_t pos, uint8_t len)
{
  while (pos + SEG_HDR_LEN <= len)
  {
    const uint8_t id = s_rxbuf[pos];
    const uint8_t frag = (uint8_t)(s_rxbuf[pos + 1u] >> 4);
    const bool last = (s_rxbuf[pos + 1u] & SEG_FLAG_LAST) != 0u;
    const bool echo = (s_rxbuf[pos + 1u] & SEG_FLAG_ECHO) != 0u;
    const bool gen = (s_rxbuf[pos + 1u] & SEG_FLAG_GEN) != 0u;
    const uint8_t seg_len = s_rxbuf[pos + 2u];
    pos += SEG_HDR_LEN;
    if (pos + seg_len > len)
    {
      s_stats.reassembly_errors++;
      return;
    }
    const uint8_t *seg = &s_rxbuf[pos];
    pos += seg_len;

    if (gen)
    {
      /* generator filler: count it, check the numbering, drop it */
      if (seg_len >= 4u)
      {
        uint32_t gseq;
        memcpy(&gseq, seg, 4);
        if (s_gen_have_seq)
        {
          s_stats.gen_lost += gseq - s_gen_rx_seq - 1u;
        }
        s_gen_rx_seq = gseq;
        s_gen_have_seq = true;
      }
      s_stats.gen_rx_bytes += seg_len;
      s_stats.gen_rx_segments++;
      continue;
    }

    if (frag == 0u)
    {
      if (s_rxd.active)
      {
        s_stats.reassembly_errors++; /* previous datagram never completed */
      }
      s_rxd.active = true;
      s_rxd.id = id;
      s_rxd.next_frag = 0;
      s_rxd.q = echo ? &s_q_echo : s_q_rx;
      s_rxd.d = dq_alloc(s_rxd.q);
      if (s_rxd.d != NULL)
      {
        s_rxd.d->len = 0;
        s_rxd.d->flags = 0; /* an echoed datagram goes back as normal data */
      }
    }

    if (!s_rxd.active || (id != s_rxd.id) || (frag != s_rxd.next_frag))
    {
      s_stats.reassembly_errors++;
      s_rxd.active = false;
      continue;
    }

    if (s_rxd.d != NULL)
    {
      if ((uint32_t)s_rxd.d->len + seg_len > APP_DGRAM_MAX)
      {
        s_stats.reassembly_errors++;
        s_rxd.active = false;
        continue;
      }
      memcpy(&s_rxd.d->data[s_rxd.d->len], seg, seg_len);
      s_rxd.d->len = (uint16_t)(s_rxd.d->len + seg_len);
    }
    s_rxd.next_frag++;

    if (last)
    {
      s_rxd.active = false;
      if ((s_rxd.d != NULL) && (s_rxd.q == &s_q_echo))
      {
        dq_commit(&s_q_echo);
        s_stats.dgram_echoed++;
      }
      else if (s_rxd.d != NULL)
      {
        s_stats.bytes_rx += s_rxd.d->len;
        dq_commit(s_q_rx);
        s_stats.dgram_rx++;
        if (s_rx_notify != NULL)
        {
          s_rx_notify();
        }
      }
    }
  }
}

/* Read and validate a received frame. Returns false if it is not a frame from our peer.
 * *flags_out receives the header flags; segments are processed here. */
static bool receive_frame(uint8_t *len_out, uint8_t *flags_out)
{
  uint8_t len;
  uint8_t offset;
  sx1280_get_rx_buffer_status(&len, &offset);
  if (len < LINK_HDR_LEN)
  {
    s_stats.rx_foreign++;
    return false;
  }
  sx1280_read_buffer(offset, s_rxbuf, len);

  const uint16_t link_id = (uint16_t)((s_rxbuf[0] << 8) | s_rxbuf[1]);
  const uint8_t flags = s_rxbuf[2];
  const bool from_master = (flags & LINK_FLAG_MASTER) != 0u;
  const uint32_t hdr_len = LINK_HDR_LEN + (((flags & LINK_FLAG_CTRL) != 0u) ? LINK_CTRL_LEN : 0u);
  if ((link_id != s_link_id) || ((flags >> 4) != LINK_VERSION) || (from_master == (APP_ROLE_MASTER != 0)) ||
      (len < hdr_len))
  {
    s_stats.rx_foreign++;
    return false;
  }

  sx1280_get_lora_packet_status(&s_stats.last_rssi, &s_stats.last_snr);
  s_stats.rssi_sum += s_stats.last_rssi;
  s_stats.snr_sum += s_stats.last_snr;

  const uint8_t seq = s_rxbuf[4];
  if (s_have_peer_seq)
  {
    s_stats.frames_lost += (uint8_t)(seq - s_peer_seq - 1u);
  }
  s_peer_seq = seq;
  s_have_peer_seq = true;
  s_stats.frames_rx++;
  s_rx_this_frame = true;

  process_segments(hdr_len, len);
  *len_out = len;
  *flags_out = flags;
  return true;
}

static void set_connected(bool connected)
{
  if (!connected && s_stats.connected)
  {
    s_have_peer_seq = false;
    s_rxd.active = false;
  }
  s_stats.connected = connected;
}

/* ---- Master --------------------------------------------------------------------------------- */

#if APP_ROLE_MASTER

static void master_start(void)
{
  s_hop = FHSS_CHANNEL_COUNT - 1u; /* first frame uses hop 0 */
  s_frame_start = rtimer_now() + 1000u - s_frame_us;
  s_last_link_time = rtimer_now();
  schedule(s_frame_start + s_frame_us);
}

/* Modem switch / fallback decisions, at the start of a frame before it is scheduled. */
static void master_switch_step(uint32_t now)
{
  switch (s_sw.state)
  {
    case RADIO_SWITCH_ANNOUNCE:
      if (s_sw.countdown > 0u)
      {
        s_sw.countdown--;
      }
      else if (s_sw.acked)
      {
        apply_modem(&s_sw.cfg); /* this frame is the first one with the new settings */
        s_sw.probation_start = now;
        set_switch_state(RADIO_SWITCH_PROBATION);
      }
      else
      {
        switch_done(RADIO_SWITCH_NO_ACK); /* slave never confirmed: keep the current settings */
      }
      break;

    case RADIO_SWITCH_PROBATION:
      if ((now - s_sw.probation_start) > PROBATION_US)
      {
        apply_modem(&s_cfg_good); /* no link on the new settings: go back */
        switch_done(RADIO_SWITCH_REVERTED);
        s_last_link_time = now;
      }
      break;

    default:
      if (!cfg_equal(&s_cfg, &s_cfg_default) && ((now - s_last_link_time) > DEFAULT_REVERT_US))
      {
        /* Long outage on custom settings (e.g. the slave restarted on its defaults) */
        apply_modem(&s_cfg_default);
        s_cfg_good = s_cfg_default;
        s_stats.switch_result = RADIO_SWITCH_DEFAULTS;
        s_last_link_time = now;
      }
      break;
  }
}

static void master_on_timer(void)
{
  const uint32_t now = rtimer_now();
  s_frame_start += s_frame_us;
  if ((int32_t)(now - s_frame_start) > (int32_t)(s_frame_us / 2u))
  {
    s_frame_start = now; /* fell far behind (debugger halt etc.): restart the time base */
  }
  master_switch_step(now); /* may change s_frame_us from this frame on */
  schedule(s_frame_start + s_frame_us);

  if (s_state == RS_TX)
  {
    s_stats.tx_timeouts++;
  }

  s_missed = s_rx_this_frame ? 0u : s_missed + 1u;
  s_rx_this_frame = false;
  set_connected(s_missed < max_missed());

  listen((uint8_t)((s_hop + 1u) % FHSS_CHANNEL_COUNT));
  if (clear_to_send())
  {
    const uint8_t len = build_frame(s_cfg.master_payload);
    const uint32_t offset = (rtimer_now() - s_frame_start + TX_LOAD_US) / 8u;
    s_txbuf[5] = (uint8_t)((offset > 255u) ? 255u : offset);
    transmit(len);
    s_chan_tx[fhss_channel(s_hop)]++;
  }
  else
  {
    s_chan_busy[fhss_channel(s_hop)]++;
  }
}

static void master_on_rx(uint32_t t_irq)
{
  (void)t_irq;
  uint8_t len;
  uint8_t flags;
  if (!receive_frame(&len, &flags)) /* slave reply; radio stays in RX until the next hop */
  {
    return;
  }
  s_last_link_time = rtimer_now();
  s_chan_rx[fhss_channel(s_hop)]++;
  if ((s_sw.state == RADIO_SWITCH_ANNOUNCE) && ((flags & LINK_FLAG_ACK) != 0u) && (s_rxbuf[5] == s_sw.id))
  {
    s_sw.acked = true;
  }
  else if (s_sw.state == RADIO_SWITCH_PROBATION)
  {
    s_cfg_good = s_cfg; /* the slave answered on the new settings */
    switch_done(RADIO_SWITCH_OK);
  }
}

/* Start announcing a modem switch (radio thread). */
static void master_begin_switch(const radio_modem_cfg_t *cfg)
{
  if (s_test_mode || (s_sw.state != RADIO_SWITCH_IDLE) || !s_stats.connected)
  {
    s_stats.switch_result = RADIO_SWITCH_REJECTED;
    return;
  }
  s_sw.cfg = *cfg;
  s_sw.id = s_next_switch_id++;
  if (s_next_switch_id == 0u)
  {
    s_next_switch_id = 1u;
  }
  uint32_t frames = SWITCH_ANNOUNCE_US / s_frame_us;
  frames = (frames < 8u) ? 8u : ((frames > 60u) ? 60u : frames);
  s_sw.countdown = (uint8_t)frames;
  s_sw.acked = false;
  s_stats.switch_result = RADIO_SWITCH_PENDING;
  set_switch_state(RADIO_SWITCH_ANNOUNCE);
}

#else /* ---- Slave ------------------------------------------------------------------------------ */

static void slave_enter_search(void)
{
  s_synced = false;
  s_gen_active = false;
  s_stats.gen_active = false;
  set_connected(false);
  if (s_sw.state == RADIO_SWITCH_ANNOUNCE)
  {
    switch_done(RADIO_SWITCH_NO_ACK); /* lost the master before the switch */
  }
  s_search_start = rtimer_now();
  listen(s_hop);
  /* The master visits every channel once per cycle: wait one full cycle per search channel. */
  schedule(rtimer_now() + (FHSS_CHANNEL_COUNT + 1u) * s_frame_us);
}

static void slave_on_timer(void)
{
  if (!s_synced)
  {
    /* Searching: first only on the last working settings (a fade or an obstruction: the master is still on
     * them). After SEARCH_GOOD_ONLY_US also alternate with the defaults, which the master returns to after
     * DEFAULT_REVERT_US without link or uses after a restart. Searching the defaults earlier would deafen the
     * slave for a whole default-settings search cycle (~4 s) after every short loss. */
    if (!cfg_equal(&s_cfg_good, &s_cfg_default) && ((rtimer_now() - s_search_start) > SEARCH_GOOD_ONLY_US))
    {
      apply_modem(cfg_equal(&s_cfg, &s_cfg_good) ? &s_cfg_default : &s_cfg_good);
    }
    listen((uint8_t)((s_hop + 1u) % FHSS_CHANNEL_COUNT));
    schedule(rtimer_now() + (FHSS_CHANNEL_COUNT + 1u) * s_frame_us);
    return;
  }

  if (s_state == RS_TX)
  {
    s_stats.tx_timeouts++;
  }

  s_missed = s_rx_this_frame ? 0u : s_missed + 1u;
  s_rx_this_frame = false;

  if ((s_sw.state == RADIO_SWITCH_PROBATION) && ((rtimer_now() - s_sw.probation_start) > PROBATION_US))
  {
    apply_modem(&s_cfg_good); /* never heard the master on the new settings: go back and search */
    switch_done(RADIO_SWITCH_REVERTED);
    s_stats.sync_lost++;
    slave_enter_search();
    return;
  }
  if (s_missed >= max_missed())
  {
    s_stats.sync_lost++;
    slave_enter_search();
    return;
  }

  /* Free-run on the predicted hop clock until the next master frame re-anchors it. */
  s_frame_start += s_frame_us;
  if (s_sw.state == RADIO_SWITCH_ANNOUNCE)
  {
    if (s_sw.countdown > 0u)
    {
      s_sw.countdown--;
    }
    else
    {
      apply_modem(&s_sw.cfg); /* the frame starting now uses the new settings */
      s_sw.probation_start = rtimer_now();
      set_switch_state(RADIO_SWITCH_PROBATION);
    }
  }
  schedule(s_frame_start + s_frame_us - SLAVE_HOP_LEAD_US);
  listen((uint8_t)((s_hop + 1u) % FHSS_CHANNEL_COUNT));
}

/* A master frame with a control block: take over the announced switch. */
static void slave_handle_control(void)
{
  const uint8_t *c = &s_rxbuf[LINK_HDR_LEN];
  const radio_modem_cfg_t cfg = {.sf = c[2], .bw = c[3], .cr = c[4], .master_payload = c[5], .slave_payload = c[6]};
  modem_timing_t t;
  if (!modem_timing(&cfg, &t))
  {
    return; /* invalid: no ack, the master will cancel */
  }
  if ((s_sw.state == RADIO_SWITCH_IDLE) || ((s_sw.state == RADIO_SWITCH_ANNOUNCE) && (s_sw.id == c[1])))
  {
    s_sw.cfg = cfg;
    s_sw.id = c[1];
    s_sw.countdown = c[0];
    if (s_sw.state != RADIO_SWITCH_ANNOUNCE)
    {
      s_stats.switch_result = RADIO_SWITCH_PENDING;
      set_switch_state(RADIO_SWITCH_ANNOUNCE);
    }
  }
}

static void slave_on_rx(uint32_t t_irq)
{
  uint8_t len;
  uint8_t flags;
  if (!receive_frame(&len, &flags))
  {
    return;
  }

  /* Re-anchor the hop clock on the master's frame start. */
  const uint32_t tx_offset = (uint32_t)s_rxbuf[5] * 8u;
  s_frame_start = t_irq - RX_DONE_LATENCY_US - toa_us(len) - tx_offset;
  s_hop = s_rxbuf[3] % FHSS_CHANNEL_COUNT;
  s_synced = true;
  s_missed = 0;
  set_connected(true);
  schedule(s_frame_start + s_frame_us - SLAVE_HOP_LEAD_US);

  if (s_sw.state == RADIO_SWITCH_PROBATION)
  {
    s_cfg_good = s_cfg; /* heard the master on the new settings */
    switch_done(RADIO_SWITCH_OK);
  }
  else if (!cfg_equal(&s_cfg, &s_cfg_good) && (s_sw.state == RADIO_SWITCH_IDLE))
  {
    s_cfg_good = s_cfg; /* found the master while searching on the defaults */
  }
  if ((flags & LINK_FLAG_CTRL) != 0u)
  {
    slave_handle_control();
  }
  s_gen_active = (flags & LINK_FLAG_GEN) != 0u;
  s_stats.gen_active = s_gen_active;

  /* Reply in the same hop if there is still time for a full-size frame. */
  const uint32_t deadline = s_frame_start + s_frame_us - SLAVE_HOP_LEAD_US - GUARD_US / 2u;
  if ((int32_t)(deadline - (rtimer_now() + s_reply_prep_us + s_toa_own_max)) < 0)
  {
    s_stats.reply_skipped++;
    return;
  }
  if (clear_to_send())
  {
    transmit(build_frame(s_cfg.slave_payload));
  }
}

#endif /* APP_ROLE_MASTER */

/* ---- Event handling ------------------------------------------------------------------------- */

static void on_rx_done(uint32_t t_irq)
{
#if APP_ROLE_MASTER
  master_on_rx(t_irq);
#else
  slave_on_rx(t_irq);
#endif
}

static void on_timer(void)
{
#if APP_ROLE_MASTER
  master_on_timer();
#else
  slave_on_timer();
#endif
}

static void handle_dio1(void)
{
  for (uint32_t i = 0; i < 4u; i++)
  {
    const uint32_t t_irq = s_dio1_time;
    const uint16_t irq = sx1280_get_irq_status();
    if (irq == 0u)
    {
      break;
    }
    sx1280_clear_irq(irq);

    if ((irq & SX1280_IRQ_TX_DONE) != 0u)
    {
      listen(s_hop); /* stay on this channel: master waits for the reply, slave until its hop */
    }
    if ((irq & SX1280_IRQ_RX_DONE) != 0u)
    {
      if ((irq & (SX1280_IRQ_CRC_ERROR | SX1280_IRQ_HEADER_ERROR)) != 0u)
      {
        s_stats.rx_crc_errors++;
      }
      else
      {
        on_rx_done(t_irq);
      }
    }
    else if ((irq & (SX1280_IRQ_CRC_ERROR | SX1280_IRQ_HEADER_ERROR)) != 0u)
    {
      s_stats.rx_crc_errors++;
    }
    if ((irq & SX1280_IRQ_RX_TX_TIMEOUT) != 0u)
    {
      s_stats.tx_timeouts++;
      listen(s_hop);
    }

    /* DIO1 is edge triggered: only leave once the line is low again. */
    if (HAL_GPIO_ReadPin(RADIO_DIO1_GPIO_Port, RADIO_DIO1_Pin) == GPIO_PIN_RESET)
    {
      break;
    }
  }
}

static bool link_setup(void)
{
  modem_timing_t t;
  if (!modem_timing(&s_cfg_default, &t))
  {
    return false; /* invalid compile-time settings, e.g. a frame longer than 60 ms on air */
  }
  if (!ant_tx_allowed(APP_ANT_TX_PORT) || !ant_connected(APP_ANT_RX_PORT))
  {
    return false; /* power-up antenna ports must have an antenna (and a usable power limit) */
  }
  apply_modem(&s_cfg_default);
  s_cfg_good = s_cfg_default;
  apply_tx_power();
  sx1280_set_dio_irq(DIO1_IRQS, DIO1_IRQS);
  sx1280_clear_irq(SX1280_IRQ_ALL);

  const uint32_t phrase_crc = crc32((const uint8_t *)APP_BIND_PHRASE, sizeof(APP_BIND_PHRASE) - 1u);
  s_link_id = (uint16_t)(phrase_crc ^ (phrase_crc >> 16));
  fhss_init(phrase_crc);

  s_stats.ant_tx = s_ant_tx;
  s_stats.ant_rx = s_ant_rx;
  s_stats.rx_high_sens = s_rx_high_sens;
  return true;
}

/* ---- Test mode: continuous carrier for conducted power measurements --------------------------- */

static void link_resume(void)
{
#if APP_ROLE_MASTER
  master_start();
#else
  s_hop = 0;
  slave_enter_search();
#endif
}

static void cw_start(void)
{
  rtimer_cancel();
  set_connected(false);
  if (s_sw.state != RADIO_SWITCH_IDLE)
  {
    apply_modem(&s_cfg_good);
    switch_done(RADIO_SWITCH_REVERTED);
  }
  sx1280_set_standby(SX1280_STANDBY_XOSC);
  sx1280_clear_irq(SX1280_IRQ_ALL);
  sx1280_set_frequency_hz(s_cmd.freq_hz);
  sx1280_set_tx_power(s_cmd.dbm); /* conducted test into an attenuator: not EIRP limited */
  board_antenna_select(s_ant_tx);
  board_fe_tx();
  sx1280_set_tx_cw();

  s_test_mode = true;
  s_state = RS_TX;
  s_stats.test_cw = true;
  s_stats.test_freq_khz = s_cmd.freq_hz / 1000u;
  s_stats.test_dbm = s_cmd.dbm;
  schedule(rtimer_now() + s_cmd.timeout_s * 1000000u); /* auto stop */
}

static void cw_stop(void)
{
  sx1280_set_standby(SX1280_STANDBY_XOSC);
  board_fe_off();
  apply_tx_power();
  sx1280_clear_irq(SX1280_IRQ_ALL);
  s_test_mode = false;
  s_stats.test_cw = false;
  link_resume();
}

bool radio_link_request_cw(uint32_t freq_hz, int8_t dbm, uint32_t timeout_s)
{
  if (!s_ready || (freq_hz < CW_MIN_HZ) || (freq_hz > CW_MAX_HZ) || (dbm < CW_MIN_DBM) ||
      (dbm > PA_INPUT_MAX_DBM) || (timeout_s == 0u) || (timeout_s > CW_MAX_TIMEOUT_S))
  {
    return false;
  }
  s_cmd.freq_hz = freq_hz;
  s_cmd.dbm = dbm;
  s_cmd.timeout_s = timeout_s;
  s_cmd.cw = true;
  tx_event_flags_set(&s_events, EV_CMD, TX_OR);
  return true;
}

bool radio_link_set_tx_power(int8_t dbm)
{
  if (!s_ready || (dbm < CW_MIN_DBM) || (dbm > PA_INPUT_MAX_DBM))
  {
    return false; /* the output is additionally limited per antenna port (apply_tx_power) */
  }
  s_link_dbm = dbm;
  tx_event_flags_set(&s_events, EV_POWER, TX_OR);
  return true;
}

bool radio_link_set_antennas(uint8_t tx_port, uint8_t rx_port)
{
  if (!s_ready || !ant_tx_allowed(tx_port) || !ant_connected(rx_port))
  {
    return false;
  }
  s_ant_req_tx = tx_port;
  s_ant_req_rx = rx_port;
  tx_event_flags_set(&s_events, EV_ANT, TX_OR);
  return true;
}

bool radio_link_antenna_info(uint8_t port, const char **name, int16_t *gain_x10, int8_t *max_dbm, bool *tx_ok)
{
  if ((port < 1u) || (port > 3u))
  {
    return false;
  }
  *name = s_antennas[port - 1u].name;
  *gain_x10 = s_antennas[port - 1u].gain_x10;
  const int32_t limit = ant_connected(port) ? ant_power_limit(port) : CW_MIN_DBM;
  *max_dbm = (int8_t)((limit < -128) ? -128 : limit);
  *tx_ok = ant_tx_allowed(port);
  return true;
}

bool radio_link_set_rx_high_sensitivity(bool enable)
{
  if (!s_ready)
  {
    return false;
  }
  s_rx_high_sens = enable;
  tx_event_flags_set(&s_events, EV_RXGAIN, TX_OR);
  return true;
}

bool radio_link_set_generator(bool enable)
{
#if APP_ROLE_MASTER
  s_gen_request = enable;
  s_gen_have_seq = false; /* restart loss counting */
  s_stats.gen_active = enable;
  return true;
#else
  (void)enable;
  return false;
#endif
}

radio_modem_error_t radio_link_request_modem(const radio_modem_cfg_t *cfg)
{
#if APP_ROLE_MASTER
  modem_timing_t t;
  if (!s_ready)
  {
    return RADIO_MODEM_ERR_BUSY;
  }
  if (!modem_timing(cfg, &t))
  {
    return RADIO_MODEM_ERR_INVALID;
  }
  if (s_test_mode || (s_sw.state != RADIO_SWITCH_IDLE))
  {
    return RADIO_MODEM_ERR_BUSY;
  }
  if (!s_stats.connected)
  {
    return RADIO_MODEM_ERR_NO_LINK;
  }
  s_modem_req = *cfg;
  tx_event_flags_set(&s_events, EV_MODEM, TX_OR);
  return RADIO_MODEM_OK;
#else
  (void)cfg;
  return RADIO_MODEM_ERR_NOT_MASTER;
#endif
}

bool radio_link_modem_preview(const radio_modem_cfg_t *cfg, uint32_t *frame_us, uint32_t *toa_master_us,
                              uint32_t *toa_slave_us)
{
  modem_timing_t t;
  if (!modem_timing(cfg, &t))
  {
    return false;
  }
  *frame_us = t.frame_us;
  *toa_master_us = t.toa_master_us;
  *toa_slave_us = t.toa_slave_us;
  return true;
}

void radio_link_request_stop(void)
{
  if (s_ready)
  {
    s_cmd.cw = false;
    tx_event_flags_set(&s_events, EV_CMD, TX_OR);
  }
}

static VOID radio_thread_entry(ULONG arg)
{
  (void)arg;

  while (!sx1280_init())
  {
    board_set_fault(BOARD_FAULT_RADIO, true);
    tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
  }
  if (!link_setup())
  {
    board_set_fault(BOARD_FAULT_RADIO, true);
    sx1280_set_standby(SX1280_STANDBY_RC);
    tx_thread_suspend(tx_thread_identify());
  }
  board_set_fault(BOARD_FAULT_RADIO, false);
  s_stats.radio_ok = true;
  s_ready = true;

  link_resume();

  for (;;)
  {
    ULONG flags;
    tx_event_flags_get(&s_events, EV_DIO1 | EV_TIMER | EV_CMD | EV_POWER | EV_RXGAIN | EV_MODEM | EV_ANT,
                       TX_OR_CLEAR, &flags, TX_WAIT_FOREVER);

    if ((flags & EV_RXGAIN) != 0u)
    {
      sx1280_set_high_sensitivity(s_rx_high_sens); /* takes effect from the next RX start */
      s_stats.rx_high_sens = s_rx_high_sens;
    }

    if ((flags & EV_ANT) != 0u)
    {
      /* Power and LBT threshold follow the new TX antenna before anything is sent on it. The switch
       * itself is set at the next listen()/transmit(), so a frame on air is not cut. */
      s_ant_tx = s_ant_req_tx;
      s_ant_rx = s_ant_req_rx;
      s_stats.ant_tx = s_ant_tx;
      s_stats.ant_rx = s_ant_rx;
      update_lbt_threshold();
      if (!s_test_mode)
      {
        apply_tx_power();
      }
    }

    if ((flags & EV_POWER) != 0u)
    {
      if (!s_test_mode)
      {
        apply_tx_power(); /* applies from the next transmission */
      }
      s_stats.tx_req_dbm = s_link_dbm;
    }

#if APP_ROLE_MASTER
    if ((flags & EV_MODEM) != 0u)
    {
      const radio_modem_cfg_t req = s_modem_req;
      master_begin_switch(&req);
    }
#endif

    if ((flags & EV_CMD) != 0u)
    {
      if (s_cmd.cw)
      {
        cw_start(); /* also used to change frequency/power or extend the timeout while running */
      }
      else if (s_test_mode)
      {
        cw_stop();
      }
      continue;
    }
    if (s_test_mode)
    {
      if (((flags & EV_TIMER) != 0u) && ((int32_t)(rtimer_now() - s_timer_deadline) >= -50))
      {
        cw_stop(); /* timeout */
      }
      continue;
    }

    /* Recover a missed DIO1 edge (line already high when the IRQ was cleared). */
    if (((flags & EV_DIO1) != 0u) ||
        (HAL_GPIO_ReadPin(RADIO_DIO1_GPIO_Port, RADIO_DIO1_Pin) == GPIO_PIN_SET))
    {
      handle_dio1();
    }
    /* Ignore a timer event that belongs to a deadline replaced in the meantime. */
    if (((flags & EV_TIMER) != 0u) && ((int32_t)(rtimer_now() - s_timer_deadline) >= -50))
    {
      on_timer();
    }
    s_stats.hop = s_hop;
  }
}

void radio_link_start(dgram_queue_t *q_tx, dgram_queue_t *q_rx, void (*rx_notify)(void))
{
  s_q_tx = q_tx;
  s_q_rx = q_rx;
  s_rx_notify = rx_notify;

  tx_event_flags_create(&s_events, "radio events");
  tx_thread_create(&s_thread, "radio", radio_thread_entry, 0, s_stack, sizeof(s_stack),
                   RADIO_THREAD_PRIO, RADIO_THREAD_PRIO, TX_NO_TIME_SLICE, TX_AUTO_START);
}

void radio_link_get_channel_stats(uint32_t tx[], uint32_t rx[], uint32_t busy[])
{
  for (uint32_t i = 0; i < FHSS_CHANNEL_COUNT; i++)
  {
    tx[i] = s_chan_tx[i];
    rx[i] = s_chan_rx[i];
    busy[i] = s_chan_busy[i];
  }
}

void radio_link_get_stats(radio_link_stats_t *out)
{
  s_stats.echo_drops = s_q_echo.drops;
  *out = s_stats;
}
