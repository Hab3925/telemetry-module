#include "net_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "board.h"
#include "fhss.h"
#include "radio_link.h"
#include "temperature.h"

#define EV_UDP_RX           0x1u
#define EV_RADIO_RX         0x2u

#define NET_THREAD_PRIO     9u
#define NET_STACK_SIZE      4096u
#define LINK_CHECK_TICKS    (TX_TIMER_TICKS_PER_SECOND)
#define UDP_QUEUE_MAX       8u

static TX_THREAD s_thread;
static ULONG s_stack[NET_STACK_SIZE / sizeof(ULONG)];
static TX_EVENT_FLAGS_GROUP s_events;

static NX_IP *s_ip;
static NX_PACKET_POOL *s_pool;
static dgram_queue_t *s_q_to_radio;
static dgram_queue_t *s_q_from_radio;

static NX_UDP_SOCKET s_data_socket;
static NX_UDP_SOCKET s_status_socket;
static NX_UDP_SOCKET s_echo_socket;

static uint32_t s_udp_rx;
static uint32_t s_udp_tx;
static uint32_t s_udp_tx_errors;
static uint32_t s_udp_oversize;
static bool s_phy_up_prev = true;

static VOID udp_rx_notify(NX_UDP_SOCKET *socket)
{
  (void)socket;
  tx_event_flags_set(&s_events, EV_UDP_RX, TX_OR);
}

void net_bridge_notify(void)
{
  tx_event_flags_set(&s_events, EV_RADIO_RX, TX_OR);
}

/* Ethernet -> radio. flags = DGRAM_FLAG_ECHO for the echo test port. */
static void drain_data_socket(NX_UDP_SOCKET *socket, uint8_t flags)
{
  NX_PACKET *packet;
  while (nx_udp_socket_receive(socket, &packet, NX_NO_WAIT) == NX_SUCCESS)
  {
    s_udp_rx++;
    const ULONG len = packet->nx_packet_length;
    if (len > radio_link_max_datagram())
    {
      s_udp_oversize++;
    }
    else
    {
      dgram_t *d = dq_alloc(s_q_to_radio); /* NULL: queue full, counted as drop */
      if (d != NULL)
      {
        ULONG copied = 0;
        nx_packet_data_retrieve(packet, d->data, &copied);
        d->len = (uint16_t)copied;
        d->flags = flags;
        dq_commit(s_q_to_radio);
      }
    }
    nx_packet_release(packet);
  }
}

/* Radio -> Ethernet */
static void drain_radio_queue(void)
{
  dgram_t *d;
  while ((d = dq_peek(s_q_from_radio)) != NULL)
  {
    NX_PACKET *packet;
    if (nx_packet_allocate(s_pool, &packet, NX_UDP_PACKET, NX_NO_WAIT) == NX_SUCCESS)
    {
      if ((nx_packet_data_append(packet, d->data, d->len, s_pool, NX_NO_WAIT) == NX_SUCCESS) &&
          (nx_udp_socket_send(&s_data_socket, packet, APP_UDP_DEST_IP, APP_UDP_DEST_PORT) == NX_SUCCESS))
      {
        s_udp_tx++;
      }
      else
      {
        nx_packet_release(packet);
        s_udp_tx_errors++;
      }
    }
    else
    {
      s_udp_tx_errors++;
    }
    dq_release(s_q_from_radio);
  }
}

static unsigned bw_khz(uint8_t bw)
{
  switch (bw)
  {
    case SX1280_LORA_BW_200:  return 200;
    case SX1280_LORA_BW_400:  return 400;
    case SX1280_LORA_BW_1600: return 1600;
    default:                  return 800;
  }
}

static const char *switch_state_name(radio_switch_state_t s)
{
  static const char *const names[] = {"idle", "announce", "probation"};
  return ((unsigned)s < 3u) ? names[s] : "?";
}

static const char *switch_result_name(radio_switch_result_t r)
{
  static const char *const names[] = {"none", "pending", "ok", "no_ack", "reverted", "defaults", "rejected"};
  return ((unsigned)r < 7u) ? names[r] : "?";
}

static int format_status(char *text, size_t size)
{
  radio_link_stats_t st;
  radio_link_get_stats(&st);

  /* Board temperature, printed as fixed point (printf without float support) */
  /* Antenna table: "<port>:<name>:<gain 0.1 dBi>:<max SX1280 dBm>:<tx allowed>;..." */
  char ports[160] = "";
  size_t pl = 0;
  for (uint8_t port = 1; port <= 3u; port++)
  {
    const char *name;
    int16_t gain;
    int8_t max_dbm;
    bool tx_ok;
    if (radio_link_antenna_info(port, &name, &gain, &max_dbm, &tx_ok) && (pl < sizeof(ports)))
    {
      pl += (size_t)snprintf(&ports[pl], sizeof(ports) - pl, "%s%u:%s:%d:%d:%u", (port > 1u) ? ";" : "", port, name,
                             gain, max_dbm, tx_ok);
    }
  }

  int32_t temp = 0;
  uint16_t ntc_raw = 0;
  char temp_text[16] = "error";
  if (temperature_read_decideg(&temp, &ntc_raw))
  {
    const int32_t mag = (temp < 0) ? -temp : temp;
    snprintf(temp_text, sizeof(temp_text), "%s%ld.%ld", (temp < 0) ? "-" : "", (long)(mag / 10), (long)(mag % 10));
  }

  return snprintf(text, size,
      "role=%s radio_ok=%u connected=%u hop=%u frame_us=%lu tx_dbm=%d tx_req_dbm=%d tx_max_dbm=%d rx_high_sens=%u "
      "test_cw=%u cw_khz=%lu cw_dbm=%d\n"
      "rssi=%d snr=%d cca_rssi=%d lbt_thr=%d rssi_sum=%ld snr_sum=%ld\n"
      "frames tx=%lu rx=%lu lost=%lu crc_err=%lu foreign=%lu lbt_busy=%lu lbt_invalid=%lu tx_timeout=%lu "
      "reply_skip=%lu sync_lost=%lu\n"
      "dgram radio_tx=%lu radio_rx=%lu oversize=%lu reasm_err=%lu q_to_radio_drop=%lu q_from_radio_drop=%lu "
      "echoed=%lu echo_drop=%lu\n"
      "udp rx=%lu tx=%lu tx_err=%lu oversize=%lu max_dgram=%lu\n"
      "bytes radio_tx=%lu radio_rx=%lu capacity_bps=%lu\n"
      "board temp_c=%s ntc_raw=%u\n"
      "antenna tx=%u rx=%u ports=%s\n"
      "modem sf=%u bw=%u cr=%u mp=%u sp=%u switch=%s result=%s\n"
      "gen active=%u tx_bytes=%lu rx_bytes=%lu rx_segments=%lu lost=%lu\n",
      APP_ROLE_MASTER ? "master" : "slave", st.radio_ok, st.connected, st.hop, (unsigned long)st.frame_us,
      st.tx_dbm, st.tx_req_dbm, st.tx_max_dbm, st.rx_high_sens, st.test_cw, (unsigned long)st.test_freq_khz, st.test_dbm,
      st.last_rssi, st.last_snr, st.last_cca_rssi, st.lbt_threshold, (long)st.rssi_sum, (long)st.snr_sum,
      (unsigned long)st.frames_tx, (unsigned long)st.frames_rx, (unsigned long)st.frames_lost,
      (unsigned long)st.rx_crc_errors, (unsigned long)st.rx_foreign, (unsigned long)st.lbt_busy,
      (unsigned long)st.lbt_invalid, (unsigned long)st.tx_timeouts, (unsigned long)st.reply_skipped,
      (unsigned long)st.sync_lost,
      (unsigned long)st.dgram_tx, (unsigned long)st.dgram_rx, (unsigned long)st.dgram_oversize,
      (unsigned long)st.reassembly_errors, (unsigned long)s_q_to_radio->drops,
      (unsigned long)s_q_from_radio->drops, (unsigned long)st.dgram_echoed, (unsigned long)st.echo_drops,
      (unsigned long)s_udp_rx, (unsigned long)s_udp_tx, (unsigned long)s_udp_tx_errors,
      (unsigned long)s_udp_oversize, (unsigned long)radio_link_max_datagram(),
      (unsigned long)st.bytes_tx, (unsigned long)st.bytes_rx, (unsigned long)st.capacity_bps,
      temp_text, ntc_raw, st.ant_tx, st.ant_rx, ports,
      (unsigned)(st.modem.sf >> 4), bw_khz(st.modem.bw), (unsigned)(st.modem.cr + 4u),
      (unsigned)st.modem.master_payload, (unsigned)st.modem.slave_payload,
      switch_state_name(st.switch_state), switch_result_name(st.switch_result),
      st.gen_active, (unsigned long)st.gen_tx_bytes, (unsigned long)st.gen_rx_bytes,
      (unsigned long)st.gen_rx_segments, (unsigned long)st.gen_lost);
}

/* "chan": per channel counters, "chan base_khz=.. step_khz=.. tx=a,b,.. rx=.. busy=.." */
static int format_channels(char *text, size_t size)
{
  static uint32_t tx[FHSS_CHANNEL_COUNT], rx[FHSS_CHANNEL_COUNT], busy[FHSS_CHANNEL_COUNT];
  radio_link_get_channel_stats(tx, rx, busy);
  int n = snprintf(text, size, "chan base_khz=%lu step_khz=%lu", (unsigned long)(FHSS_BASE_HZ / 1000u),
                   (unsigned long)(FHSS_SPACING_HZ / 1000u));
  const uint32_t *arrays[3] = {tx, rx, busy};
  const char *names[3] = {" tx=", " rx=", " busy="};
  for (int a = 0; (a < 3) && (n > 0) && ((size_t)n < size); a++)
  {
    n += snprintf(&text[n], size - (size_t)n, "%s", names[a]);
    for (uint32_t i = 0; (i < FHSS_CHANNEL_COUNT) && ((size_t)n < size); i++)
    {
      n += snprintf(&text[n], size - (size_t)n, (i == 0u) ? "%lu" : ",%lu", (unsigned long)arrays[a][i]);
    }
  }
  if ((n > 0) && ((size_t)n < size))
  {
    n += snprintf(&text[n], size - (size_t)n, "\n");
  }
  return n;
}

/* "modem [check] <sf 5..12> <bw 200|400|800|1600> <cr 5..8> [master_payload] [slave_payload]" */
static int handle_modem(char *args, char *text, size_t size)
{
  static const char usage[] =
      "error: usage: modem [check] <sf 5..12> <bw_khz 200|400|800|1600> <cr 5..8> [master_payload] [slave_payload]\n";
  const bool check_only = (strncmp(args, " check", 6) == 0);
  if (check_only)
  {
    args += 6;
  }
  radio_link_stats_t st;
  radio_link_get_stats(&st);

  long v[5] = {0, 0, 0, st.modem.master_payload, st.modem.slave_payload};
  int n = 0;
  for (char *tok = strtok(args, " \r\n"); (tok != NULL) && (n < 5); tok = strtok(NULL, " \r\n"))
  {
    char *end;
    v[n++] = strtol(tok, &end, 10);
    if (*end != '\0')
    {
      return snprintf(text, size, "%s", usage);
    }
  }
  uint8_t bw;
  switch (v[1])
  {
    case 200:  bw = SX1280_LORA_BW_200;  break;
    case 400:  bw = SX1280_LORA_BW_400;  break;
    case 800:  bw = SX1280_LORA_BW_800;  break;
    case 1600: bw = SX1280_LORA_BW_1600; break;
    default:   bw = 0; break;
  }
  if ((n < 3) || (v[0] < 5) || (v[0] > 12) || (bw == 0u) || (v[2] < 5) || (v[2] > 8) ||
      (v[3] < 1) || (v[3] > 255) || (v[4] < 1) || (v[4] > 255))
  {
    return snprintf(text, size, "%s", usage);
  }
  const radio_modem_cfg_t cfg = {
      .sf = (uint8_t)(v[0] << 4), .bw = bw, .cr = (uint8_t)(v[2] - 4),
      .master_payload = (uint8_t)v[3], .slave_payload = (uint8_t)v[4]};

  uint32_t frame_us, toa_m, toa_s;
  if (!radio_link_modem_preview(&cfg, &frame_us, &toa_m, &toa_s))
  {
    return snprintf(text, size, "error: invalid settings: payloads must be 32..255 bytes and each frame "
                                "under 60 ms on air\n");
  }
  if (check_only)
  {
    return snprintf(text, size, "ok check frame_us=%lu toa_master_us=%lu toa_slave_us=%lu\n",
                    (unsigned long)frame_us, (unsigned long)toa_m, (unsigned long)toa_s);
  }
  switch (radio_link_request_modem(&cfg))
  {
    case RADIO_MODEM_OK:
      return snprintf(text, size, "ok switching both boards to SF%ld %ld kHz CR4/%ld, frames %ld/%ld bytes "
                                  "(frame %lu us); watch modem result in the status\n",
                      v[0], v[1], v[2], v[3], v[4], (unsigned long)frame_us);
    case RADIO_MODEM_ERR_NOT_MASTER:
      return snprintf(text, size, "error: send modem commands to the master; it switches both boards\n");
    case RADIO_MODEM_ERR_NO_LINK:
      return snprintf(text, size, "error: no link to the slave, it has to confirm the switch\n");
    case RADIO_MODEM_ERR_BUSY:
      return snprintf(text, size, "error: a modem switch or carrier test is in progress\n");
    default:
      return snprintf(text, size, "%s", usage);
  }
}

/* Parse "<MHz>[.<fraction>]" into Hz (no floating point). */
static bool parse_mhz(const char *s, uint32_t *hz)
{
  char *end;
  const long mhz = strtol(s, &end, 10);
  if ((end == s) || (mhz < 0) || (mhz > 6000))
  {
    return false;
  }
  uint32_t frac_hz = 0;
  if (*end == '.')
  {
    uint32_t scale = 100000u;
    for (end++; (*end >= '0') && (*end <= '9'); end++)
    {
      frac_hz += (uint32_t)(*end - '0') * scale;
      scale /= 10u;
    }
  }
  if ((*end != '\0') && (*end != ' ') && (*end != '\n') && (*end != '\r'))
  {
    return false;
  }
  *hz = (uint32_t)mhz * 1000000u + frac_hz;
  return true;
}

/* Commands (anything else returns the status report):
 *   cw <MHz> <dBm> [timeout_s]   continuous carrier test, e.g. "cw 2440 -6 60"
 *   stop                         end the test and resume the link
 *   power <dBm>                  request link TX power until reboot (limited per TX antenna for the EIRP)
 *   antenna <tx> [rx]            antenna ports for transmitting / receiving until reboot
 *   rxgain high|normal           SX1280 LNA mode until reboot (diagnostics; high breaks RSSI/LBT)
 *   modem [check] <sf> <bw_khz> <cr> [mp] [sp]   change (or just check) the modem settings on both boards
 *   gen on|off                   master: slave fills its frames with dummy data (throughput test) */
static int handle_command(char *cmd, char *text, size_t size)
{
  static const char usage[] = "error: usage: cw <MHz 2401..2482> <dBm -18..5> [timeout_s 1..600] | stop\n";

  if (strncmp(cmd, "chan", 4) == 0)
  {
    return format_channels(text, size);
  }
  if (strncmp(cmd, "gen", 3) == 0)
  {
    const bool on = (strstr(cmd, "on") != NULL);
    if ((!on && (strstr(cmd, "off") == NULL)) || !radio_link_set_generator(on))
    {
      return snprintf(text, size, "error: usage: gen on|off (master only)\n");
    }
    return snprintf(text, size, "ok slave generator %s\n", on ? "on" : "off");
  }
  if (strncmp(cmd, "modem", 5) == 0)
  {
    return handle_modem(&cmd[5], text, size);
  }
  if (strncmp(cmd, "rxgain", 6) == 0)
  {
    const bool high = (strstr(cmd, "high") != NULL);
    if ((!high && (strstr(cmd, "normal") == NULL)) || !radio_link_set_rx_high_sensitivity(high))
    {
      return snprintf(text, size, "error: usage: rxgain high|normal\n");
    }
    return snprintf(text, size, "ok rx lna %s until reboot\n", high ? "high sensitivity" : "normal");
  }
  if (strncmp(cmd, "antenna", 7) == 0)
  {
    char *end;
    const long tx = strtol(&cmd[7], &end, 10);
    char *end2;
    long rx = strtol(end, &end2, 10);
    if (end2 == end)
    {
      rx = tx; /* one port: transmit and receive on it */
    }
    if ((end == &cmd[7]) || (tx < 1) || (tx > 3) || (rx < 1) || (rx > 3) ||
        !radio_link_set_antennas((uint8_t)tx, (uint8_t)rx))
    {
      return snprintf(text, size, "error: usage: antenna <tx port> [rx port], ports 1..3 (J3..J5); the TX port "
                                  "must have an antenna whose gain allows transmitting within the EIRP limit\n");
    }
    return snprintf(text, size, "ok antenna tx port %ld, rx port %ld until reboot\n", tx, rx);
  }
  if (strncmp(cmd, "power", 5) == 0)
  {
    char *end;
    const long dbm = strtol(&cmd[5], &end, 10);
    if ((end == &cmd[5]) || (dbm < -128) || (dbm > 127) || !radio_link_set_tx_power((int8_t)dbm))
    {
      return snprintf(text, size, "error: usage: power <dBm -18..5> (limited further for the TX antenna)\n");
    }
    return snprintf(text, size, "ok link tx power %ld dBm (SX1280 output) until reboot\n", dbm);
  }
  if (strncmp(cmd, "stop", 4) == 0)
  {
    radio_link_request_stop();
    return snprintf(text, size, "ok stopped, link resuming\n");
  }
  if (strncmp(cmd, "cw", 2) != 0)
  {
    return format_status(text, size);
  }

  char *tok_freq = strtok(&cmd[2], " \r\n");
  char *tok_dbm = strtok(NULL, " \r\n");
  char *tok_timeout = strtok(NULL, " \r\n");
  uint32_t hz;
  if ((tok_freq == NULL) || (tok_dbm == NULL) || !parse_mhz(tok_freq, &hz))
  {
    return snprintf(text, size, "%s", usage);
  }
  char *end;
  const long dbm = strtol(tok_dbm, &end, 10);
  const long timeout_s = (tok_timeout != NULL) ? strtol(tok_timeout, NULL, 10) : 60;
  if ((*end != '\0') || (dbm < -128) || (dbm > 127) || (timeout_s <= 0) ||
      !radio_link_request_cw(hz, (int8_t)dbm, (uint32_t)timeout_s))
  {
    return snprintf(text, size, "%s", usage);
  }
  return snprintf(text, size, "ok cw %lu.%03lu MHz %ld dBm (SX1280 output), stops after %ld s\n",
                  (unsigned long)(hz / 1000000u), (unsigned long)((hz / 1000u) % 1000u), dbm, timeout_s);
}

static void answer_status_requests(void)
{
  NX_PACKET *request;
  while (nx_udp_socket_receive(&s_status_socket, &request, NX_NO_WAIT) == NX_SUCCESS)
  {
    ULONG src_ip;
    UINT src_port;
    nx_udp_source_extract(request, &src_ip, &src_port);

    char cmd[64] = {0};
    ULONG cmd_len = 0;
    if (request->nx_packet_length < sizeof(cmd))
    {
      nx_packet_data_retrieve(request, cmd, &cmd_len);
    }
    nx_packet_release(request);

    char text[1400];
    const int n = handle_command(cmd, text, sizeof(text));
    if (n <= 0)
    {
      continue;
    }

    NX_PACKET *reply;
    if (nx_packet_allocate(s_pool, &reply, NX_UDP_PACKET, NX_NO_WAIT) != NX_SUCCESS)
    {
      continue;
    }
    const ULONG len = ((size_t)n < sizeof(text)) ? (ULONG)n : (ULONG)(sizeof(text) - 1u);
    if ((nx_packet_data_append(reply, text, len, s_pool, NX_NO_WAIT) != NX_SUCCESS) ||
        (nx_udp_socket_send(&s_status_socket, reply, src_ip, src_port) != NX_SUCCESS))
    {
      nx_packet_release(reply);
    }
  }
}

/* The ST driver only enables the link once at boot (and gives up after 5 s without a cable).
 * Re-enable it when a cable is plugged in later, and renegotiate speed/duplex after a re-plug. */
static void check_eth_link(void)
{
  ULONG status = 0;
  const bool phy_up =
      (nx_ip_interface_status_check(s_ip, 0, NX_IP_LINK_ENABLED, &status, NX_NO_WAIT) == NX_SUCCESS);
  const bool if_up = (s_ip->nx_ip_interface[0].nx_interface_link_up != NX_FALSE);

  if (phy_up && (!if_up || !s_phy_up_prev))
  {
    ULONG unused;
    nx_ip_driver_direct_command(s_ip, NX_LINK_DISABLE, &unused);
    nx_ip_driver_direct_command(s_ip, NX_LINK_ENABLE, &unused);
  }
  s_phy_up_prev = phy_up;
  board_set_fault(BOARD_FAULT_ETH_LINK, !phy_up);
}

static VOID net_thread_entry(ULONG arg)
{
  (void)arg;

  nx_ip_address_set(s_ip, APP_IP_ADDRESS, APP_NET_MASK); /* per role; the .ioc has one shared address */

  nx_udp_socket_create(s_ip, &s_data_socket, "bridge", NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                       NX_IP_TIME_TO_LIVE, UDP_QUEUE_MAX);
  nx_udp_socket_bind(&s_data_socket, APP_UDP_LISTEN_PORT, TX_WAIT_FOREVER);
  nx_udp_socket_receive_notify(&s_data_socket, udp_rx_notify);

  nx_udp_socket_create(s_ip, &s_echo_socket, "echo", NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                       NX_IP_TIME_TO_LIVE, UDP_QUEUE_MAX);
  nx_udp_socket_bind(&s_echo_socket, APP_UDP_ECHO_PORT, TX_WAIT_FOREVER);
  nx_udp_socket_receive_notify(&s_echo_socket, udp_rx_notify);

  nx_udp_socket_create(s_ip, &s_status_socket, "status", NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                       NX_IP_TIME_TO_LIVE, 2);
  nx_udp_socket_bind(&s_status_socket, APP_UDP_STATUS_PORT, TX_WAIT_FOREVER);
  nx_udp_socket_receive_notify(&s_status_socket, udp_rx_notify);

  ULONG next_link_check = tx_time_get();
  for (;;)
  {
    ULONG flags;
    tx_event_flags_get(&s_events, EV_UDP_RX | EV_RADIO_RX, TX_OR_CLEAR, &flags, LINK_CHECK_TICKS / 4u);

    drain_data_socket(&s_data_socket, 0);
    drain_data_socket(&s_echo_socket, DGRAM_FLAG_ECHO);
    drain_radio_queue();
    answer_status_requests();

    if ((LONG)(tx_time_get() - next_link_check) >= 0)
    {
      next_link_check += LINK_CHECK_TICKS;
      check_eth_link();
    }
  }
}

void net_bridge_start(NX_IP *ip, NX_PACKET_POOL *pool, dgram_queue_t *q_to_radio, dgram_queue_t *q_from_radio)
{
  s_ip = ip;
  s_pool = pool;
  s_q_to_radio = q_to_radio;
  s_q_from_radio = q_from_radio;

  tx_event_flags_create(&s_events, "net events");
  tx_thread_create(&s_thread, "net bridge", net_thread_entry, 0, s_stack, sizeof(s_stack),
                   NET_THREAD_PRIO, NET_THREAD_PRIO, TX_NO_TIME_SLICE, TX_AUTO_START);
}
