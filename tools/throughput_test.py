#!/usr/bin/env python3
"""Measure the real throughput of the radio link with only the master on Ethernet.

Two modes:

gen (default): slave -> master, the telemetry direction.
The script switches on the slave's traffic generator through the master ("gen on"). The slave
fills every frame with numbered dummy data, the master counts what arrives (and the gaps) and
drops it. The script reads the master's counters once per second. Nothing crosses Ethernet except
the status queries, so this measures the radio link alone.

echo: round trip through both directions, with real UDP datagrams.
Test datagrams go to the master's echo port (UDP 5003). The master sends them over the radio
marked "echo", the slave sends them straight back, and the master outputs them on UDP 5001 like
any received data. The script checks every datagram that comes back and reports throughput,
loss and round-trip time.

Because every byte crosses the link twice, the result is the throughput of the slower direction
(with equal frame sizes the two directions are the same).

Flow control: at most --window datagrams are outstanding, so the board's 8-slot queues never
overflow. A datagram that has not come back after --timeout seconds counts as lost.

Needs firmware with the echo port, and APP_UDP_DEST_IP set to broadcast (default) or this computer.

Examples:
    python3 tools/throughput_test.py                         # generator, 30 s
    python3 tools/throughput_test.py --duration 120          # longer run, e.g. during a range test
    python3 tools/throughput_test.py --mode echo             # round trip with 512 byte datagrams
    python3 tools/throughput_test.py --size 100 --duration 60
    python3 tools/throughput_test.py --rate 5 --size 200     # fixed load instead of "as fast as possible"
"""

import argparse
import os
import socket
import statistics
import struct
import time

from telemetry_status import get_status

HEADER = struct.Struct("<4sId")  # magic, sequence number, send time
MAGIC = b"TPUT"


def link_summary(host):
    st = get_status(host)[1]
    if not st:
        return None, "master does not answer on the status port"
    modem = (f"SF{st.get('modem.sf', '?')} {st.get('modem.bw', '?')} kHz CR4/{st.get('modem.cr', '?')} "
             f"frames {st.get('modem.mp', '?')}/{st.get('modem.sp', '?')} B") if "modem.sf" in st else "modem ?"
    text = (f"{modem}, hop {st.get('frame_us', 0) / 1000:.1f} ms, capacity {st.get('capacity_bps', 0) / 1000:.1f} kbit/s "
            f"each way, TX {st.get('tx_dbm', '?')} dBm, link {'up' if st.get('connected') else 'DOWN'}")
    return st, text


def command(host, text):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(text.encode(), (host, 5002))
        return s.recvfrom(512)[0].decode().strip()


def run_generator(args, before):
    """Slave -> master throughput from the master's generator counters."""
    if "gen.rx_bytes" not in before:
        print("This firmware has no traffic generator: flash both boards with the current build.")
        return
    frame_us = before.get("frame_us", 0)
    sp = before.get("modem.sp", 0)
    slave_capacity = (sp - 9) * 8e6 / frame_us if frame_us and sp else None
    reply = command(args.host, "gen on")
    if not reply.startswith("ok"):
        print(f"Master refused: {reply}")
        return
    print(f"Slave generator on for {args.duration:g} s"
          + (f" (slave -> master capacity {slave_capacity / 1000:.1f} kbit/s)" if slave_capacity else "") + "\n")
    print(f"{'time':>5} {'kbit/s':>8} {'lost':>5} {'RSSI':>6} {'SNR':>5}")

    start = time.time()
    first = prev = before
    rates = []
    try:
        while time.time() - start < args.duration:
            time.sleep(max(0.0, 1.0 - (time.time() - start) % 1.0))
            cur = get_status(args.host)[1]
            if cur is None:
                print(f"{time.time() - start:5.0f}  master does not answer")
                continue
            dt = 1.0
            kbps = (cur["gen.rx_bytes"] - prev["gen.rx_bytes"]) * 8 / dt / 1000
            lost = cur["gen.lost"] - prev["gen.lost"]
            frames = cur["frames.rx"] - prev["frames.rx"]
            rssi = (cur["rssi_sum"] - prev["rssi_sum"]) / frames if frames > 0 else None
            snr = (cur["snr_sum"] - prev["snr_sum"]) / frames if frames > 0 else None
            if time.time() - start > 1.5:  # first second: generator still starting
                rates.append(kbps)
            print(f"{time.time() - start:5.0f} {kbps:8.2f} {lost:5d} "
                  f"{'-' if rssi is None else f'{rssi:6.0f}'} {'-' if snr is None else f'{snr:5.0f}'}"
                  + ("  LINK DOWN" if not cur.get("connected") else ""), flush=True)
            prev = cur
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        try:
            command(args.host, "gen off")
        except OSError:
            print("Could not switch the generator off: send 'gen off' to the master, or it stops when the link drops.")

    last = prev
    segs = last["gen.rx_segments"] - first["gen.rx_segments"]
    lost = last["gen.lost"] - first["gen.lost"]
    print("\nResult (slave -> master)")
    if rates:
        print(f"  Throughput:  average {statistics.mean(rates):.2f} kbit/s, min {min(rates):.2f}, max {max(rates):.2f}")
        if slave_capacity:
            print(f"  Capacity:    {slave_capacity / 1000:.1f} kbit/s -> {100 * statistics.mean(rates) * 1000 / slave_capacity:.0f} % achieved")
    print(f"  Frames:      {segs} received, {lost} lost ({100 * lost / max(1, segs + lost):.1f} %)")
    print(f"  Master saw:  {last['frames.crc_err'] - first['frames.crc_err']} CRC errors, "
          f"{last['frames.lbt_busy'] - first['frames.lbt_busy']} LBT skips (own), sync now {'up' if last.get('connected') else 'DOWN'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mode", choices=["gen", "echo"], default="gen")
    parser.add_argument("--host", default="192.168.1.50", help="the master's address")
    parser.add_argument("--echo-port", type=int, default=5003)
    parser.add_argument("--listen-port", type=int, default=5001)
    parser.add_argument("--size", type=int, default=512, help="datagram size in bytes (16..1472)")
    parser.add_argument("--duration", type=float, default=30.0, help="seconds to run")
    parser.add_argument("--window", type=int, default=6, help="max datagrams outstanding (1..8)")
    parser.add_argument("--rate", type=float, help="send at this many datagrams/s instead of as fast as possible")
    parser.add_argument("--timeout", type=float, default=5.0, help="seconds before a datagram counts as lost")
    args = parser.parse_args()

    size = max(HEADER.size, min(1472, args.size))
    window = max(1, min(8, args.window))

    before, text = link_summary(args.host)
    print(f"Link: {text}")
    if before is None:
        return
    if not before.get("connected"):
        print("Warning: the radio link is down, nothing will come back.")
    if args.mode == "gen":
        run_generator(args, before)
        return

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    rx.bind(("0.0.0.0", args.listen_port))
    rx.settimeout(0.005)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    outstanding = {}  # seq -> (payload, send time)
    rtts, last_seq = [], -1
    sent = echoed = lost = corrupted = reordered = foreign = 0
    bytes_back = 0
    interval_bytes, interval_rtts = 0, []
    seq = 0

    mode = f"{args.rate:g} datagrams/s" if args.rate else f"as fast as possible (window {window})"
    print(f"Sending {size} byte datagrams to {args.host}:{args.echo_port} for {args.duration:g} s, {mode}\n")
    print(f"{'time':>5} {'sent':>6} {'back':>6} {'lost':>5} {'kbit/s':>8} {'RTT ms':>8}")

    start = time.time()
    next_send = start
    next_report = start + 1.0
    end_send = start + args.duration
    while True:
        now = time.time()
        sending = now < end_send

        # send
        if sending:
            if args.rate:
                may_send = now >= next_send
            else:
                may_send = len(outstanding) < window
            if may_send:
                payload = HEADER.pack(MAGIC, seq, now) + os.urandom(size - HEADER.size)
                tx.sendto(payload, (args.host, args.echo_port))
                outstanding[seq] = (payload, now)
                seq += 1
                sent += 1
                next_send += 1.0 / args.rate if args.rate else 0

        # receive
        try:
            data, _ = rx.recvfrom(4096)
        except socket.timeout:
            data = None
        if data is not None:
            t = time.time()
            if len(data) < HEADER.size or data[:4] != MAGIC:
                foreign += 1
            else:
                _, s, t_sent = HEADER.unpack_from(data)
                entry = outstanding.pop(s, None)
                if entry is None:
                    pass  # already counted as lost, or a duplicate
                elif entry[0] != data:
                    corrupted += 1
                else:
                    echoed += 1
                    bytes_back += len(data)
                    interval_bytes += len(data)
                    rtt = (t - t_sent) * 1000
                    rtts.append(rtt)
                    interval_rtts.append(rtt)
                    if s < last_seq:
                        reordered += 1
                    last_seq = max(last_seq, s)

        # expire
        now = time.time()
        for s in [s for s, (_, t_sent) in outstanding.items() if now - t_sent > args.timeout]:
            del outstanding[s]
            lost += 1

        # report once per second
        if now >= next_report:
            rtt = f"{statistics.median(interval_rtts):.0f}" if interval_rtts else "-"
            print(f"{now - start:5.0f} {sent:6d} {echoed:6d} {lost:5d} {interval_bytes * 8 / 1000:8.2f} {rtt:>8}", flush=True)
            interval_bytes, interval_rtts = 0, []
            next_report += 1.0

        if not sending and not outstanding:
            break

    active = min(time.time() - start, args.duration)
    after, text = link_summary(args.host)

    print("\nResult")
    print(f"  Datagrams:   {sent} sent, {echoed} back ({100 * echoed / max(1, sent):.1f} %), {lost} lost, "
          f"{corrupted} corrupted, {reordered} out of order, {foreign} other traffic")
    print(f"  Throughput:  {bytes_back * 8 / 1000 / active:.2f} kbit/s of data, each way "
          f"({bytes_back / 1000:.1f} kB in {active:.0f} s)")
    if rtts:
        print(f"  Round trip:  min {min(rtts):.0f}  median {statistics.median(rtts):.0f}  "
              f"p95 {sorted(rtts)[int(0.95 * (len(rtts) - 1))]:.0f}  max {max(rtts):.0f} ms")
    if after:
        lost_frames = after.get("frames.lost", 0) - before.get("frames.lost", 0)
        crc = after.get("frames.crc_err", 0) - before.get("frames.crc_err", 0)
        busy = after.get("frames.lbt_busy", 0) - before.get("frames.lbt_busy", 0)
        drops = after.get("dgram.q_to_radio_drop", 0) - before.get("dgram.q_to_radio_drop", 0)
        print(f"  Master saw:  {lost_frames} slave frames lost, {crc} CRC errors, {busy} LBT skips, "
              f"{drops} queue drops, RSSI {after.get('rssi')} dBm, SNR {after.get('snr')} dB")
        if before.get("capacity_bps"):
            print(f"  Capacity:    {before['capacity_bps'] / 1000:.1f} kbit/s each way at these settings "
                  f"-> {100 * bytes_back * 8 / active / before['capacity_bps']:.0f} % used")


if __name__ == "__main__":
    main()
