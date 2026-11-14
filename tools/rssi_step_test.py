#!/usr/bin/env python3
"""Step the link TX power on both boards and record the RSSI each board reports for the other.

Each board's RSSI measures the *other* board's transmission, so with both boards set to the same
power every step gives two independent readings. RSSI/SNR are averaged over all frames received
in each step (firmware running sums); "frames" shows the spread of single-frame readings.

Examples:
    python3 tools/rssi_step_test.py                     # -18..-4 dBm, current RX gain mode
    python3 tools/rssi_step_test.py --rxgain normal     # SX1280 LNA high sensitivity off first
    python3 tools/rssi_step_test.py --powers -18 -10 --seconds 6
"""

import argparse
import socket
import statistics
import time

from telemetry_status import STATUS_PORT, get_status

BOARDS = {"master": "192.168.1.50", "slave": "192.168.1.51"}


def command(host, text):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(text.encode(), (host, STATUS_PORT))
        reply = s.recvfrom(512)[0].decode().strip()
    if not reply.startswith("ok"):
        raise SystemExit(f"{host}: {reply}")
    return reply


def measure(seconds):
    """Per board: list of interval-average RSSI, single-frame RSSI and average SNR."""
    prev = {name: get_status(host)[1] for name, host in BOARDS.items()}
    result = {name: ([], [], []) for name in BOARDS}
    end = time.time() + seconds
    while time.time() < end:
        time.sleep(0.25)
        for name, host in BOARDS.items():
            cur = get_status(host)[1]
            old = prev[name]
            if old and cur and cur["frames.rx"] > old["frames.rx"]:
                frames = cur["frames.rx"] - old["frames.rx"]
                result[name][0].append((cur["rssi_sum"] - old["rssi_sum"]) / frames)
                result[name][1].append(cur["rssi"])
                result[name][2].append((cur["snr_sum"] - old["snr_sum"]) / frames)
            prev[name] = cur
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--powers", type=int, nargs=2, default=[-18, -4], metavar=("FROM", "TO"))
    parser.add_argument("--seconds", type=float, default=4.0, help="measurement time per step")
    parser.add_argument("--rxgain", choices=["high", "normal"], help="set SX1280 LNA mode on both boards first")
    parser.add_argument("--end-power", type=int, default=-18, help="power left on both boards afterwards")
    args = parser.parse_args()

    for name, host in BOARDS.items():
        if args.rxgain:
            command(host, f"rxgain {args.rxgain}")
        status = get_status(host)[1]
        if status is None:
            raise SystemExit(f"{name} ({host}) does not answer")
        print(f"{name}: rx_high_sens={status.get('rx_high_sens', '?')}")

    print(f"\n{'TX both':>8} | {'master hears slave':^30} | {'slave hears master':^30}")
    print(f"{'dBm':>8} | {'avg':>6} {'single frames':>14} {'SNR':>6} | {'avg':>6} {'single frames':>14} {'SNR':>6}")
    try:
        for power in list(range(args.powers[0], args.powers[1] + 1)):
            for host in BOARDS.values():
                command(host, f"power {power}")
            time.sleep(0.5)
            result = measure(args.seconds)
            row = f"{power:+8d} |"
            for name in ("master", "slave"):
                avg, single, snr = result[name]
                if avg:
                    row += f" {statistics.mean(avg):6.1f} {min(single):6d}..{max(single):<6d} {statistics.mean(snr):6.1f} |"
                else:
                    row += f" {'no frames':>30} |"
            print(row, flush=True)
    finally:
        for host in BOARDS.values():
            command(host, f"power {args.end_power}")


if __name__ == "__main__":
    main()
