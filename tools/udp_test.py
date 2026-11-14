#!/usr/bin/env python3
"""End-to-end test through the radio link.

Sends numbered, timestamped datagrams to one board's UDP input port (5000) and receives what
the other board outputs (broadcast to port 5001). Reports delivery, corruption, ordering,
latency and throughput.

Examples:
    python3 tools/udp_test.py                          # master -> slave, 100 datagrams of 8..200 B at 5/s
    python3 tools/udp_test.py --to 192.168.1.51        # slave -> master
    python3 tools/udp_test.py --count 50 --size 1472   # fragmentation test with full-size datagrams
    python3 tools/udp_test.py --rate 20 --size 100     # load test (link carries ~1.6 kB/s per direction)
"""

import argparse
import os
import random
import socket
import struct
import time

HEADER = struct.Struct("<4sId")  # magic, sequence number, send time
MAGIC = b"TLMT"


def percentile(values, p):
    if not values:
        return float("nan")
    values = sorted(values)
    return values[min(len(values) - 1, int(round(p / 100 * (len(values) - 1))))]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--to", default="192.168.1.50", help="board that receives the datagrams over Ethernet")
    parser.add_argument("--port", type=int, default=5000, help="board UDP input port")
    parser.add_argument("--listen-port", type=int, default=5001, help="port the far board sends to")
    parser.add_argument("--count", type=int, default=100, help="number of datagrams")
    parser.add_argument("--rate", type=float, default=5.0, help="datagrams per second")
    parser.add_argument("--size", type=int, nargs="+", default=[8, 200],
                        help="fixed size, or min and max for random sizes (bytes, 16..1472)")
    parser.add_argument("--wait", type=float, default=3.0, help="seconds to wait for stragglers after sending")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    lo = max(HEADER.size, args.size[0])
    hi = max(lo, min(1472, args.size[-1]))
    rng = random.Random(args.seed)

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.bind(("0.0.0.0", args.listen_port))
    rx.settimeout(0.01)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    sent = {}
    received = {}
    latencies = []
    order = []
    duplicates = corrupted = foreign = 0
    sources = set()
    bytes_received = 0

    def receive_until(deadline):
        nonlocal duplicates, corrupted, foreign, bytes_received
        while time.time() < deadline:
            try:
                data, addr = rx.recvfrom(4096)
            except socket.timeout:
                continue
            now = time.time()
            if len(data) < HEADER.size or data[:4] != MAGIC:
                foreign += 1
                continue
            _, seq, t_sent = HEADER.unpack_from(data)
            sources.add(addr[0])
            if seq in received:
                duplicates += 1
                continue
            if sent.get(seq) != data:
                corrupted += 1
                continue
            received[seq] = now
            order.append(seq)
            latencies.append((now - t_sent) * 1000)
            bytes_received += len(data)
            print(f"\r  sent {len(sent)}/{args.count}  received {len(received)}", end="", flush=True)

    print(f"Sending {args.count} datagrams of {lo}..{hi} B at {args.rate:g}/s to {args.to}:{args.port}, "
          f"listening on :{args.listen_port}")
    t_start = time.time()
    interval = 1.0 / args.rate
    for seq in range(args.count):
        size = rng.randint(lo, hi)
        payload = HEADER.pack(MAGIC, seq, time.time()) + os.urandom(size - HEADER.size)
        sent[seq] = payload
        tx.sendto(payload, (args.to, args.port))
        receive_until(t_start + (seq + 1) * interval)
    receive_until(time.time() + args.wait)
    elapsed = time.time() - t_start
    print()

    missing = [s for s in sent if s not in received]
    out_of_order = sum(1 for a, b in zip(order, order[1:]) if b < a)
    print(f"\nFrom:          {', '.join(sorted(sources)) or '-'}")
    print(f"Delivered:     {len(received)}/{len(sent)} ({100 * len(received) / max(1, len(sent)):.1f} %)")
    print(f"Corrupted:     {corrupted}    duplicates: {duplicates}    out of order: {out_of_order}"
          f"    other traffic: {foreign}")
    if missing:
        shown = ", ".join(str(s) for s in missing[:20]) + (" ..." if len(missing) > 20 else "")
        print(f"Missing seq:   {shown}")
    if latencies:
        print(f"Latency ms:    min {min(latencies):.0f}  median {percentile(latencies, 50):.0f}  "
              f"p95 {percentile(latencies, 95):.0f}  max {max(latencies):.0f}")
    print(f"Throughput:    {bytes_received / elapsed / 1000:.2f} kB/s received ({elapsed:.1f} s)")


if __name__ == "__main__":
    main()
