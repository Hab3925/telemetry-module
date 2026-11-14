#!/usr/bin/env python3
"""Live monitor of the radio link, polling each board's status port (UDP 5002).

Shows per board: connection state, RSSI/SNR, link quality (share of expected frames received
in the last interval), payload bandwidth over the radio, and per-interval counts of lost frames,
CRC errors, LBT skips etc.

Examples:
    python3 tools/link_monitor.py                       # both boards, 1 s interval
    python3 tools/link_monitor.py --hosts 192.168.1.50  # only the master
    python3 tools/link_monitor.py --csv range_test.csv  # also log every sample (for range tests)
"""

import argparse
import csv
import sys
import time

from telemetry_status import DEFAULT_HOSTS, get_status

# Counters shown as "change since last sample"
DELTA_FIELDS = [
    ("frames.lost", "lost"),
    ("frames.crc_err", "crc"),
    ("frames.lbt_busy", "lbt"),
    ("frames.lbt_invalid", "lbt_inv"),
    ("frames.tx_timeout", "tx_to"),
    ("frames.sync_lost", "sync_lost"),
    ("dgram.radio_tx", "dg_tx"),
    ("dgram.radio_rx", "dg_rx"),
    ("dgram.q_to_radio_drop", "q_drop"),
]


def link_quality(prev, cur, dt):
    """Percent of the frames we should have received from the peer during dt."""
    frame_us = cur.get("frame_us") or 0
    if prev is None or frame_us <= 0 or dt <= 0:
        return None
    expected = dt * 1e6 / frame_us
    received = cur["frames.rx"] - prev["frames.rx"]
    if received < 0:  # board rebooted
        return None
    return min(100.0, 100.0 * received / expected)


def interval_average(prev, cur, sum_key, fallback_key):
    """Average over all frames received in the interval (firmware keeps running sums).
    Falls back to the last frame's value with older firmware or when no frame arrived."""
    if prev is not None and sum_key in cur and sum_key in prev:
        frames = cur["frames.rx"] - prev["frames.rx"]
        if frames > 0:
            return (cur[sum_key] - prev[sum_key]) / frames
    return cur.get(fallback_key)


def rate_kbps(prev, cur, key, dt):
    """Payload bit rate of a byte counter over dt, in kbit/s (None if unavailable)."""
    if prev is None or dt <= 0 or key not in cur or key not in prev or cur[key] < prev[key]:
        return None
    return (cur[key] - prev[key]) * 8 / dt / 1000


def fmt(value, width, spec=""):
    text = "-" if value is None else format(value, spec)
    return text.rjust(width)


def color(text, ok):
    if not sys.stdout.isatty():
        return text
    return ("\033[32m" if ok else "\033[31m") + text + "\033[0m"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hosts", nargs="+", default=DEFAULT_HOSTS, help="board IP addresses")
    parser.add_argument("--interval", type=float, default=1.0, help="seconds between samples")
    parser.add_argument("--csv", help="append every sample to this CSV file")
    parser.add_argument("--no-clear", action="store_true", help="print a new block per sample instead of redrawing")
    args = parser.parse_args()

    prev = {host: (None, None) for host in args.hosts}
    csv_writer = None
    csv_file = None
    if args.csv:
        csv_file = open(args.csv, "a", newline="")
        csv_writer = csv.writer(csv_file)
        if csv_file.tell() == 0:
            csv_writer.writerow(["time", "host", "role", "connected", "rssi", "snr", "cca_rssi", "lq_percent",
                                 "tx_kbps", "rx_kbps", "tx_use_percent", "temp_c"]
                                + [name for _, name in DELTA_FIELDS] + ["frames_tx", "frames_rx"])

    redraw = sys.stdout.isatty() and not args.no_clear
    header = (f"{'board':<15}{'role':>7}{'link':>6}{'LQ%':>6}{'RSSI':>6}{'SNR':>5}{'CCA':>6}"
              f"{'TXkbps':>8}{'RXkbps':>8}{'TXuse%':>8}{'Temp':>7}"
              + "".join(f"{name:>{max(len(name) + 1, 6)}}" for _, name in DELTA_FIELDS))
    try:
        while True:
            start = time.time()
            lines = []
            for host in args.hosts:
                t, st = get_status(host)
                t_prev, st_prev = prev[host]
                if st is None:
                    lines.append(f"{host:<15}" + color("  no reply", False))
                    continue

                dt = (t - t_prev) if t_prev else 0
                lq = link_quality(st_prev, st, dt)
                temp = st.get("board.temp_c") if isinstance(st.get("board.temp_c"), (int, float)) else None
                rssi = interval_average(st_prev, st, "rssi_sum", "rssi")
                snr = interval_average(st_prev, st, "snr_sum", "snr")
                tx_kbps = rate_kbps(st_prev, st, "bytes.radio_tx", dt)
                rx_kbps = rate_kbps(st_prev, st, "bytes.radio_rx", dt)
                capacity = st.get("bytes.capacity_bps")
                tx_use = (100 * tx_kbps * 1000 / capacity) if (tx_kbps is not None and capacity) else None
                deltas = []
                for key, _ in DELTA_FIELDS:
                    if st_prev is None or key not in st or st[key] < st_prev.get(key, 0):
                        deltas.append(None)
                    else:
                        deltas.append(st[key] - st_prev[key])

                link = color(f"{'UP' if st.get('connected') else 'DOWN':>6}", bool(st.get("connected")))
                line = (f"{host:<15}{st.get('role', '?'):>7}{link}{fmt(lq, 6, '.0f')}"
                        f"{fmt(rssi, 6, '.0f')}{fmt(snr, 5, '.0f')}{fmt(st.get('cca_rssi'), 6)}"
                        f"{fmt(tx_kbps, 8, '.2f')}{fmt(rx_kbps, 8, '.2f')}{fmt(tx_use, 8, '.0f')}"
                        f"{fmt(temp, 7, '.1f')}"
                        + "".join(fmt(d, max(len(name) + 1, 6)) for d, (_, name) in zip(deltas, DELTA_FIELDS)))
                lines.append(line)

                if csv_writer:
                    csv_writer.writerow([f"{t:.3f}", host, st.get("role"), st.get("connected"),
                                         "" if rssi is None else f"{rssi:.1f}", "" if snr is None else f"{snr:.1f}",
                                         st.get("cca_rssi"), "" if lq is None else f"{lq:.1f}",
                                         "" if tx_kbps is None else f"{tx_kbps:.3f}",
                                         "" if rx_kbps is None else f"{rx_kbps:.3f}",
                                         "" if tx_use is None else f"{tx_use:.1f}",
                                         "" if temp is None else f"{temp:.1f}"]
                                        + ["" if d is None else d for d in deltas]
                                        + [st.get("frames.tx"), st.get("frames.rx")])
                prev[host] = (t, st)

            if csv_file:
                csv_file.flush()
            stamp = time.strftime("%H:%M:%S")
            if redraw:
                sys.stdout.write("\033[H\033[J")
                print(f"Telemetry link monitor  {stamp}  (interval {args.interval:g} s, Ctrl+C to quit)\n")
                print(header)
                print("\n".join(lines))
                caps = [f"{h}: {prev[h][1]['bytes.capacity_bps'] / 1000:.1f} kbit/s" for h in args.hosts
                        if prev[h][1] and "bytes.capacity_bps" in prev[h][1]]
                print("\nRSSI/SNR = average over all frames received in the interval. "
                      "LQ% = frames received from the peer / frames expected. Counters are per interval.")
                print("TX/RX kbps = UDP payload over the radio. TXuse% = share of this board's send capacity"
                      + (f" ({', '.join(caps)})." if caps else "."))
            else:
                print(f"--- {stamp}")
                print(header)
                print("\n".join(lines))
            sys.stdout.flush()
            time.sleep(max(0.0, args.interval - (time.time() - start)))
    except KeyboardInterrupt:
        pass
    finally:
        if csv_file:
            csv_file.close()


if __name__ == "__main__":
    main()
