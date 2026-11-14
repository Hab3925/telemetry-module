"""Shared helpers: query and parse the status report a board sends back on UDP port 5002."""

import socket
import time

STATUS_PORT = 5002
DEFAULT_HOSTS = ["192.168.1.50", "192.168.1.51"]


def query_status(host, timeout=0.5, port=STATUS_PORT):
    """Return the raw status text from a board, or None if it did not answer."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        try:
            s.sendto(b"status", (host, port))
            data, _ = s.recvfrom(2048)
            return data.decode(errors="replace")
        except (socket.timeout, OSError):
            return None


def parse_status(text):
    """Parse the report into a flat dict.

    Lines look like "frames tx=1 rx=2 ..."; a leading word without '=' becomes a prefix,
    so that line yields "frames.tx" and "frames.rx". Numeric values are converted to int.
    """
    result = {}
    for line in text.splitlines():
        tokens = line.split()
        prefix = ""
        if tokens and "=" not in tokens[0]:
            prefix = tokens.pop(0) + "."
        for token in tokens:
            key, _, value = token.partition("=")
            try:
                result[prefix + key] = int(value)
            except ValueError:
                try:
                    result[prefix + key] = float(value)
                except ValueError:
                    result[prefix + key] = value
    return result


def get_status(host, timeout=0.5):
    """Query and parse in one go. Returns (timestamp, dict) or (timestamp, None)."""
    now = time.time()
    text = query_status(host, timeout)
    return now, (parse_status(text) if text else None)
