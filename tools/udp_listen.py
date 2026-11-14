#!/usr/bin/env python3
"""Print every datagram that comes out of the radio link (boards send to UDP port 5001).

Examples:
    python3 tools/udp_listen.py            # hex + ASCII dump
    python3 tools/udp_listen.py --text     # decode as text (for line-based telemetry)
"""

import argparse
import socket
import time


def hexdump(data, width=16):
    for offset in range(0, len(data), width):
        chunk = data[offset:offset + width]
        hex_part = " ".join(f"{b:02x}" for b in chunk).ljust(width * 3)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"    {offset:04x}  {hex_part} {ascii_part}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=5001)
    parser.add_argument("--text", action="store_true", help="print payload as text instead of a hex dump")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))
    print(f"Listening on UDP :{args.port} (Ctrl+C to quit)")

    count = 0
    last = None
    try:
        while True:
            data, addr = sock.recvfrom(4096)
            now = time.time()
            count += 1
            gap = f"+{(now - last) * 1000:.0f} ms" if last else ""
            last = now
            print(f"[{time.strftime('%H:%M:%S')}.{int(now % 1 * 1000):03d}] #{count} from {addr[0]}  "
                  f"{len(data)} B  {gap}")
            if args.text:
                print("    " + data.decode(errors="replace").rstrip())
            else:
                hexdump(data)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
