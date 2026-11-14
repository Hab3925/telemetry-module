#!/usr/bin/env python3
"""Conducted TX power sweep with a spectrum analyzer (e.g. Signal Hound BB60D + Spike).

The board is put into its continuous-carrier test mode (link stopped) and stepped through
SX1280 output settings on each frequency. At every step you type the power the analyzer shows
at J3, i.e. with the attenuator and cable loss entered as reference offset in the analyzer.
The script flags where the PA starts to compress and recommends APP_SX1280_TX_POWER_DBM.

SETUP (before running):
    J3 -> SMA cable -> 30 dB attenuator (>= 2 W) -> analyzer.  NEVER connect J3 directly:
    the analyzer takes max +10 dBm, the board can deliver > +20 dBm.
    Analyzer: centre = test frequency, span ~1 MHz, RBW <= 100 kHz, peak marker,
              reference offset = attenuator + cable loss (so the reading is power at J3).

At each prompt:  <number> = measured dBm,  Enter = skip,  s = skip rest of this frequency,  q = quit.

Examples:
    python3 tools/power_sweep.py                                   # master, 2402/2440/2480 MHz, -10..+5 dBm
    python3 tools/power_sweep.py --host 192.168.1.51               # the slave board
    python3 tools/power_sweep.py --freqs 2440 --powers -18 5 --antenna-gain 3
"""

import argparse
import csv
import socket
import time

STATUS_PORT = 5002


def command(host, text, timeout=1.0):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto(text.encode(), (host, STATUS_PORT))
        reply, _ = s.recvfrom(2048)
        return reply.decode(errors="replace").strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="192.168.1.50", help="board to measure")
    parser.add_argument("--freqs", type=float, nargs="+", default=[2402, 2440, 2480], help="MHz")
    parser.add_argument("--powers", type=int, nargs=2, default=[-10, 5], metavar=("FROM", "TO"),
                        help="SX1280 output range in dBm (-18..5), 1 dB steps")
    parser.add_argument("--antenna-gain", type=float, default=2.0, help="dBi of the antenna that will be used")
    parser.add_argument("--cable-loss", type=float, default=0.0,
                        help="dB between J3 and the antenna in the real installation")
    parser.add_argument("--margin", type=float, default=2.0,
                        help="dB below the 20 dBm EIRP limit (measurement uncertainty; BB60D is +-2 dB)")
    parser.add_argument("--timeout", type=int, default=120, help="carrier switches off after this many s per step")
    parser.add_argument("--csv", default="power_sweep.csv")
    args = parser.parse_args()

    lo, hi = max(-18, args.powers[0]), min(5, args.powers[1])
    max_conducted = 20.0 - args.antenna_gain + args.cable_loss - args.margin
    print("SETUP" + __doc__.split("SETUP")[1].split("Examples:")[0])
    print(f"Board {args.host}: {command(args.host, 'status').splitlines()[0]}")
    print(f"Target: conducted power at J3 <= {max_conducted:.1f} dBm "
          f"(20 dBm EIRP - {args.antenna_gain} dBi + {args.cable_loss} dB cable - {args.margin} dB margin)\n")
    input("Attenuator connected and analyzer ready? Press Enter to start transmitting...")

    results = {}  # freq -> list of (setting, measured)
    try:
        for freq in args.freqs:
            print(f"\n=== {freq:.3f} MHz  (set analyzer centre frequency, then measure each step)")
            results[freq] = []
            prev = None
            for dbm in range(lo, hi + 1):
                reply = command(args.host, f"cw {freq:.3f} {dbm} {args.timeout}")
                if not reply.startswith("ok"):
                    print(f"  board refused: {reply}")
                    break
                time.sleep(0.2)
                answer = input(f"  SX1280 {dbm:+3d} dBm -> measured at J3 (dBm): ").strip().lower()
                if answer == "q":
                    raise KeyboardInterrupt
                if answer == "s":
                    break
                if not answer:
                    continue
                try:
                    measured = float(answer.replace(",", "."))
                except ValueError:
                    print("  not a number, skipped")
                    continue
                results[freq].append((dbm, measured))
                note = []
                if prev is not None and dbm - prev[0] == 1:
                    step = measured - prev[1]
                    note.append(f"step {step:+.1f} dB")
                    if step < 0.7:
                        note.append("COMPRESSING")
                note.append(f"gain {measured - dbm:.1f} dB")
                if measured > max_conducted:
                    note.append("ABOVE TARGET")
                print("      " + ", ".join(note))
                prev = (dbm, measured)
                if measured > max_conducted + 3:
                    print("      well above target, stopping this frequency")
                    break
    except KeyboardInterrupt:
        print("\nAborted.")
    finally:
        try:
            print("Stopping carrier:", command(args.host, "stop"))
        except OSError:
            print("Could not reach the board: power cycle it, or the carrier stops after the timeout.")

    with open(args.csv, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["freq_mhz", "sx1280_dbm", "measured_j3_dbm"])
        for freq, rows in results.items():
            writer.writerows([freq, d, m] for d, m in rows)
    print(f"\nSaved {args.csv}")

    # Recommendation: highest setting that stays below target on every measured frequency
    measured_freqs = [f for f, rows in results.items() if rows]
    if not measured_freqs:
        return
    print(f"\n{'SX1280':>7}" + "".join(f"{f:>10.0f}" for f in measured_freqs))
    for dbm in range(lo, hi + 1):
        cells = []
        for f in measured_freqs:
            value = dict(results[f]).get(dbm)
            cells.append(f"{value:>10.1f}" if value is not None else f"{'-':>10}")
        print(f"{dbm:>+7d}" + "".join(cells))

    candidates = []
    for dbm in range(lo, hi + 1):
        values = [dict(results[f]).get(dbm) for f in measured_freqs]
        if all(v is not None for v in values) and max(values) <= max_conducted:
            candidates.append((dbm, max(values)))
    if candidates:
        dbm, worst = candidates[-1]
        print(f"\nRecommended: APP_SX1280_TX_POWER_DBM = {dbm}  "
              f"(worst case {worst:.1f} dBm at J3 -> {worst + args.antenna_gain - args.cable_loss:.1f} dBm EIRP)")
        print("Check the table: if that setting is in the compressing region, choose a lower one.")
    else:
        print("\nNo setting was measured on all frequencies below the target.")


if __name__ == "__main__":
    main()
