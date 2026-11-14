#!/usr/bin/env python3
"""Automated conducted TX power sweep: the board's carrier test + Signal Hound Spike over SCPI.

For each frequency the board transmits an unmodulated carrier ("cw" command) and steps through
SX1280 settings from low to high. For every step Spike sweeps, peak-searches and returns the
marker. The script writes one CSV row per step and prints the TX chain gain (SX1280 setting -> J3).

MADE FOR A DIRECT CABLE TO THE ANALYZER (NO ATTENUATOR). Protection for the analyzer input:
  - Before you connect the cable, the link power is set to -18 dBm ("power" command), because the
    master transmits every hop at APP_SX1280_TX_POWER_DBM (-3 dBm -> about +14 dBm at J3).
    A reboot restores that power: if the board resets, disconnect the cable first.
  - The SX1280 setting never goes above --max-setting (default -11, about +6 dBm at J3 with
    17 dB of gain) and never above -8.
  - On each frequency the sweep stops going up once the next step could pass --analyzer-limit.
  - Any reading above the limit stops the whole run.

SETUP:
  - Other board switched off. Cable on the board's TX antenna port (status: antenna tx=1 = J3).
  - Spike running, analyzer connected, 15-30 min warm-up. Spike listens on TCP 5025 (one client
    at a time: close other SCPI connections). The script sets span, RBW, reference level etc.
  - Readings are stored raw (Spike reference offset 0); --cable-loss is added in the CSV.

Examples:
    python3 tools/spike_power_sweep.py --cable-loss 0.4                       # master, 40 channels, -18..-11 dBm
    python3 tools/spike_power_sweep.py --host 192.168.1.51 --cable-loss 0.4   # the slave
    python3 tools/spike_power_sweep.py --freqs 2402 2440 2480 --powers -18 -12
    python3 tools/spike_power_sweep.py --spike-host 192.168.1.20 --csv logs/txcal-master.csv
"""

import argparse
import csv
import datetime
import math
import pathlib
import socket
import statistics
import time

from telemetry_status import STATUS_PORT, parse_status, query_status

HOP_CHANNELS_MHZ = [2401 + 2 * n for n in range(40)]
SETTING_CEILING_DBM = -8   # never above this without an attenuator, whatever --max-setting says
SPIKE_PORT = 5025
FREQ_TOLERANCE_HZ = 150e3  # marker must be this close to the carrier (crystal error is ~tens of kHz)


def board_command(host, text, timeout=1.0):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto(text.encode(), (host, STATUS_PORT))
        try:
            reply, _ = s.recvfrom(2048)
        except socket.timeout:
            raise SystemExit(f"The board at {host} did not answer '{text}' on UDP {STATUS_PORT}: "
                             "check the Ethernet cable, the IP address (--host) and that the board is powered.")
        return reply.decode(errors="replace").strip()


def board_status(host):
    text = query_status(host, timeout=1.0)
    if text is None:
        raise SystemExit(f"No answer from the board at {host} (UDP {STATUS_PORT}).")
    return parse_status(text)


class Spike:
    """Minimal SCPI client for Spike: newline terminated commands, one reply line per query."""

    def __init__(self, host, port, timeout, verbose=False):
        self.verbose = verbose
        self.timeout = timeout
        try:
            self.sock = socket.create_connection((host, port), timeout=timeout)
        except ConnectionRefusedError:
            raise SystemExit(f"Nothing is listening on {host}:{port}. Start Spike (with the analyzer connected "
                             "and sweeping) before running this script: the script controls Spike, not the BB60D.")
        except socket.timeout:
            raise SystemExit(f"Could not reach Spike at {host}:{port} within {timeout:.0f} s (--spike-host, firewall).")
        self.buf = b""

    def write(self, cmd):
        if self.verbose:
            print(f"  spike <- {cmd}")
        self.sock.sendall(cmd.encode() + b"\n")

    def query(self, cmd):
        self.write(cmd)
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                raise SystemExit(f"Spike did not answer '{cmd}' within {self.timeout:.0f} s. Check that Spike shows "
                                 "the BB60D connected and in swept analysis mode, and that no other program is "
                                 "connected to it on TCP 5025. Run with -v to see every command sent.")
            if not chunk:
                raise ConnectionError("Spike closed the connection")
            self.buf += chunk
        if self.verbose:
            print(f"  spike -> {self.buf.partition(b'\\n')[0].decode(errors='replace')}")
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode(errors="replace").strip()

    def errors(self):
        out = []
        for _ in range(20):
            err = self.query(":SYSTem:ERRor?")
            if err.split(",", 1)[0].strip() in ("0", "+0", ""):
                break
            out.append(err)
        return out

    def setup(self, args):
        for cmd in [":INSTrument:SELect SA",
                    ":INITiate:CONTinuous OFF",
                    f":SENSe:FREQuency:CENTer {HOP_CHANNELS_MHZ[0] * 1e6:.0f}",
                    f":SENSe:FREQuency:SPAN {args.span_khz * 1e3:.0f}",
                    ":SENSe:BANDwidth:RESolution:AUTO OFF",
                    f":SENSe:BANDwidth:RESolution {args.rbw_khz * 1e3:.0f}",
                    ":SENSe:BANDwidth:VIDeo:AUTO OFF",
                    f":SENSe:BANDwidth:VIDeo {args.rbw_khz * 1e3:.0f}",
                    ":SENSe:POWer:RF:RLEVel 10DBM",       # top of the analyzer range: no front end compression
                    ":SENSe:POWer:RF:RLEVel:OFFSet 0",   # raw readings; cable loss is applied in the CSV
                    ":SENSe:POWer:RF:ATTenuation:AUTO 1",
                    f":SENSe:SWEep:DETector:FUNCtion {args.detector}",
                    ":SENSe:SWEep:DETector:UNITs POWer",
                    ":TRACe:SELect 1",
                    ":TRACe:TYPE WRITe",
                    ":CALCulate:MARKer:SELect 1",
                    ":CALCulate:MARKer:STATe ON",
                    ":CALCulate:MARKer:TRACe 1"]:
            self.write(cmd)
        errors = self.errors()
        if errors:
            raise SystemExit("Spike rejected the setup:\n  " + "\n  ".join(errors))

    def measure(self, freq_hz, sweeps):
        """Peak marker over `sweeps` single sweeps: (median dBm, spread dB, marker Hz)."""
        self.write(f":SENSe:FREQuency:CENTer {freq_hz:.0f}")
        levels, xs = [], []
        for _ in range(sweeps):
            self.query(":INITiate; *OPC?")
            x, y = self.query(":CALCulate:MARKer:MAXimum; X?; Y?").split(";")
            xs.append(float(x))
            levels.append(float(y))
        return statistics.median(levels), max(levels) - min(levels), statistics.median(xs)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="192.168.1.50", help="board to measure")
    parser.add_argument("--spike-host", default="127.0.0.1", help="computer running Spike")
    parser.add_argument("--spike-port", type=int, default=SPIKE_PORT, help="Spike SCPI port")
    parser.add_argument("--freqs", type=float, nargs="+", default=HOP_CHANNELS_MHZ,
                        help="MHz (default: the 40 hop channels, 2401..2479)")
    parser.add_argument("--powers", type=int, nargs=2, default=[-18, -11], metavar=("FROM", "TO"),
                        help="SX1280 setting range in dBm, 1 dB steps")
    parser.add_argument("--max-setting", type=int, default=-11, help="highest SX1280 setting allowed")
    parser.add_argument("--analyzer-limit", type=float, default=6.0,
                        help="highest reading (dBm at the analyzer input) the sweep may reach")
    parser.add_argument("--cable-loss", type=float, default=0.0, help="dB between J3 and the analyzer")
    parser.add_argument("--sweeps", type=int, default=3, help="sweeps per step (median is used)")
    parser.add_argument("--span-khz", type=float, default=1000.0)
    parser.add_argument("--rbw-khz", type=float, default=30.0)
    parser.add_argument("--detector", default="AVERage", choices=["AVERage", "MAX"])
    parser.add_argument("--settle", type=float, default=0.3, help="s after each carrier change")
    parser.add_argument("--spike-timeout", type=float, default=15.0, help="s to wait for each Spike reply")
    parser.add_argument("-v", "--verbose", action="store_true", help="print every SCPI command and reply")
    parser.add_argument("--csv", default=str(pathlib.Path(__file__).resolve().parent.parent / "logs" /
                                             f"txcal-{datetime.datetime.now():%Y%m%d-%H%M%S}.csv"),
                        help="output file (default: logs/txcal-<time>.csv in the repository)")
    args = parser.parse_args()

    top = min(args.powers[1], args.max_setting, SETTING_CEILING_DBM)
    settings = list(range(max(-18, args.powers[0]), top + 1))
    if not settings:
        raise SystemExit("No SX1280 settings left in range.")

    # 1. Make the link itself safe before anything is connected.
    reply = board_command(args.host, "power -18")
    st = board_status(args.host)
    if not reply.startswith("ok") or st.get("tx_dbm") != -18:
        raise SystemExit(f"Could not set the link power to -18 dBm ({reply}); do not connect the analyzer.")
    role, port = st.get("role"), st.get("antenna.tx")
    print(f"Board {args.host}: {role}, link power now -18 dBm, TX antenna port {port} "
          f"(1 = J3, 2 = J4, 3 = J5), board {st.get('board.temp_c')} degC")
    print(f"Plan: {len(args.freqs)} frequencies x SX1280 {settings[0]}..{settings[-1]} dBm, "
          f"stop below {args.analyzer_limit:+.1f} dBm at the analyzer")
    input(f"Connect the cable from port {port} to the analyzer, then press Enter...")

    spike = Spike(args.spike_host, args.spike_port, timeout=args.spike_timeout, verbose=args.verbose)
    print("Spike:", spike.query("*IDN?"))
    spike.setup(args)

    rows = []
    stop_all = False
    try:
        for freq in args.freqs:
            freq_hz = freq * 1e6
            temp = board_status(args.host).get("board.temp_c")
            line = []
            for dbm in settings:
                reply = board_command(args.host, f"cw {freq:.3f} {dbm} 30")
                if not reply.startswith("ok"):
                    print(f"\n  board refused: {reply}")
                    stop_all = True
                    break
                time.sleep(args.settle)
                level, spread, x = spike.measure(freq_hz, args.sweeps)
                off_freq = abs(x - freq_hz) > FREQ_TOLERANCE_HZ
                j3 = level + args.cable_loss
                rows.append({"time": datetime.datetime.now().isoformat(timespec="seconds"), "host": args.host,
                             "role": role, "port": port, "freq_mhz": freq, "sx1280_dbm": dbm,
                             "reading_dbm": round(level, 2), "spread_db": round(spread, 2),
                             "marker_mhz": round(x / 1e6, 4), "cable_loss_db": args.cable_loss,
                             "j3_dbm": round(j3, 2), "gain_db": round(j3 - dbm, 2), "temp_c": temp,
                             "note": "off-frequency" if off_freq else ""})
                line.append(f"{dbm:+d}:{level:+.1f}{'?' if off_freq else ''}")
                if level > args.analyzer_limit:
                    print(f"\n  {level:+.1f} dBm is above the analyzer limit: stopping the run")
                    stop_all = True
                    break
                if dbm == settings[0] and level < -40:
                    print(f"\n  only {level:+.1f} dBm at the lowest setting: no carrier on the cable? stopping")
                    stop_all = True
                    break
                if level + 1.5 > args.analyzer_limit:
                    line.append("(limit)")
                    break  # the next 1 dB step could pass the limit
            print(f"{freq:8.3f} MHz  " + " ".join(line))
            if stop_all:
                break
    except KeyboardInterrupt:
        print("\nAborted.")
    finally:
        try:
            print("Stopping carrier:", board_command(args.host, "stop"))
        except OSError:
            print("Could not reach the board: power cycle it, or the carrier stops after 30 s.")
        try:
            errors = spike.errors()
            if errors:
                print("Spike reported:", "; ".join(errors))
        except OSError:
            pass

    if not rows:
        return
    with open(args.csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"\nSaved {len(rows)} points to {args.csv}")

    # Summary: gain over the linear part (each 1 dB step gives >= 0.7 dB) and frequency response.
    good = [r for r in rows if not r["note"]]
    by_freq = {}
    for r in good:
        by_freq.setdefault(r["freq_mhz"], []).append(r)
    compressing = []
    for freq, pts in by_freq.items():
        for a, b in zip(pts, pts[1:]):
            if b["sx1280_dbm"] - a["sx1280_dbm"] == 1 and b["reading_dbm"] - a["reading_dbm"] < 0.7:
                compressing.append(f"{freq:.0f} MHz at {b['sx1280_dbm']:+d}")
    gains = [r["gain_db"] for r in good]
    if not gains:
        print("No valid points (all off-frequency).")
        return
    per_freq = {f: statistics.mean(p["gain_db"] for p in pts) for f, pts in by_freq.items()}
    worst_f = max(per_freq, key=per_freq.get)
    print(f"Gain SX1280 setting -> J3: mean {statistics.mean(gains):.2f} dB, "
          f"min {min(gains):.2f}, max {max(gains):.2f} (at {worst_f:.0f} MHz)")
    print(f"Flatness over frequency: {max(per_freq.values()) - min(per_freq.values()):.2f} dB")
    print(f"Largest spread between sweeps: {max(r['spread_db'] for r in good):.2f} dB")
    if compressing:
        print("Steps under 0.7 dB (compression or noise): " + ", ".join(compressing))
    skipped = len(rows) - len(good)
    if skipped:
        print(f"{skipped} point(s) ignored: marker more than {FREQ_TOLERANCE_HZ / 1e3:.0f} kHz from the carrier")
    print(f"For APP_TX_CHAIN_GAIN_DB_X10 (upper bound, before analyzer uncertainty): "
          f"{math.ceil(max(gains) * 10)}  (currently 170 = 17.0 dB)")
    print("Link power stays at -18 dBm until the board reboots.")


if __name__ == "__main__":
    main()
