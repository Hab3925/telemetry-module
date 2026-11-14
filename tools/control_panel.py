#!/usr/bin/env python3
"""Local web control panel for the telemetry boards.

Runs a small web server on this computer that serves tools/control_panel.html and forwards
the page's requests to the boards' status port (UDP 5002). A browser page cannot send UDP
itself, so this script is the bridge. Standard library only.

Examples:
    python3 tools/control_panel.py                       # http://127.0.0.1:8080, opens the browser
    python3 tools/control_panel.py --port 9000 --no-browser
    python3 tools/control_panel.py --hosts 192.168.1.50  # only the master
    python3 tools/control_panel.py --log                 # start logging right away (otherwise: button)
"""

import argparse
import csv
import datetime
import json
import os
import pathlib
import signal
import socket
import statistics
import sys
import struct
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from telemetry_status import DEFAULT_HOSTS, STATUS_PORT, parse_status, query_status

PAGE = pathlib.Path(__file__).with_name("control_panel.html")
ALLOWED_COMMANDS = ("power", "rxgain", "cw", "stop", "modem", "gen", "antenna")
ECHO_PORT = 5003
LISTEN_PORT = 5001
ECHO_HEADER = struct.Struct("<4sId")  # magic, sequence number, send time
ECHO_MAGIC = b"TPUT"


class EchoTest:
    """Round-trip throughput test: datagrams to the board's echo port come back on UDP 5001."""

    def __init__(self):
        self.lock = threading.Lock()
        self.thread = None
        self.stop_flag = threading.Event()
        self.state = {"running": False}

    def start(self, host, size, window, duration):
        with self.lock:
            if self.thread and self.thread.is_alive():
                return False, "an echo test is already running"
            self.stop_flag.clear()
            timeout = self._timeout(host, size, window)
            self.state = {"running": True, "host": host, "size": size, "window": window, "duration": duration,
                          "started": time.time(), "sent": 0, "back": 0, "late": 0, "lost": 0, "corrupted": 0,
                          "bytes_back": 0, "kbps": 0.0, "rtt_median": None, "rtt_p95": None, "history": [],
                          "timeout_s": timeout, "error": None}
            self.thread = threading.Thread(target=self._run, args=(host, size, window, duration, timeout), daemon=True)
            self.thread.start()
            return True, "started"

    def stop(self):
        self.stop_flag.set()

    @staticmethod
    def _timeout(host, size, window):
        """Time after which a datagram counts as lost, from the link's current frame sizes and hop time.
        A datagram needs ceil(size / data per frame) hops in each direction, and up to `window` of them
        queue behind each other in the slower direction."""
        text = query_status(host, timeout=0.6)
        st = parse_status(text) if text else {}
        frame_s = st.get("frame_us", 0) / 1e6
        mp, sp = st.get("modem.mp", 0), st.get("modem.sp", 0)
        if not frame_s or mp <= 22 or sp <= 22:
            return 5.0
        per_frame = lambda p: p - 22  # worst case: header + control block + segment header
        hops_up, hops_down = -(-size // per_frame(mp)), -(-size // per_frame(sp))
        expected = (window * max(hops_up, hops_down) + hops_up + hops_down) * frame_s
        return max(1.0, 3.0 * expected)

    def snapshot(self):
        with self.lock:
            return dict(self.state)

    def _update(self, **kw):
        with self.lock:
            self.state.update(kw)

    def _run(self, host, size, window, duration, timeout):
        try:
            rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
            rx.bind(("0.0.0.0", LISTEN_PORT))
            rx.settimeout(0.005)
        except OSError as e:
            self._update(running=False, error=f"cannot listen on UDP {LISTEN_PORT}: {e}")
            return
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        outstanding, expired, rtts = {}, {}, []
        sent = back = late = lost = corrupted = bytes_back = interval_bytes = seq = 0
        history = []
        start = time.time()
        next_report = start + 1.0
        while True:
            now = time.time()
            sending = (now - start < duration) and not self.stop_flag.is_set()
            if sending and len(outstanding) < window:
                payload = ECHO_HEADER.pack(ECHO_MAGIC, seq, now) + os.urandom(size - ECHO_HEADER.size)
                tx.sendto(payload, (host, ECHO_PORT))
                outstanding[seq] = (payload, now)
                seq += 1
                sent += 1
            try:
                data, _ = rx.recvfrom(4096)
            except socket.timeout:
                data = None
            if data and len(data) >= ECHO_HEADER.size and data[:4] == ECHO_MAGIC:
                _, s, t_sent = ECHO_HEADER.unpack_from(data)
                entry = outstanding.pop(s, None)
                if entry is None and s in expired:
                    entry = expired.pop(s)  # arrived after the timeout: count it as back, but late
                    lost -= 1
                    late += 1
                if entry is not None:
                    if entry[0] == data:
                        back += 1
                        bytes_back += len(data)
                        interval_bytes += len(data)
                        rtts.append((time.time() - t_sent) * 1000)
                    else:
                        corrupted += 1
            now = time.time()
            if len(rtts) >= 10:  # adapt to the measured round trips once there are some
                recent = sorted(rtts[-50:])
                timeout = max(timeout, 3.0 * recent[int(0.95 * (len(recent) - 1))] / 1000)
            for s in [s for s, (_, t) in outstanding.items() if now - t > timeout]:
                expired[s] = outstanding.pop(s)
                lost += 1
            if now >= next_report:
                history.append(interval_bytes * 8 / 1000)
                interval_bytes = 0
                next_report += 1.0
                recent = rtts[-200:]
                self._update(timeout_s=timeout, sent=sent, back=back, late=late, lost=lost, corrupted=corrupted,
                             bytes_back=bytes_back,
                             kbps=history[-1], history=history[-120:],
                             rtt_median=statistics.median(recent) if recent else None,
                             rtt_p95=sorted(recent)[int(0.95 * (len(recent) - 1))] if recent else None,
                             elapsed=now - start)
            if not sending and (not outstanding or self.stop_flag.is_set()):
                break
        rx.close()
        tx.close()
        elapsed = min(time.time() - start, duration)
        self._update(running=False, sent=sent, back=back, late=late, lost=lost + len(outstanding), corrupted=corrupted,
                     bytes_back=bytes_back, avg_kbps=bytes_back * 8 / 1000 / max(elapsed, 0.001), elapsed=elapsed)


ECHO = EchoTest()


def query_channels(host):
    """Per-channel counters from the master ("chan" command), or None."""
    try:
        text = send_command(host, "chan", timeout=0.6)
    except OSError:
        return None
    if not text.startswith("chan"):
        return None
    out = {}
    for token in text.split()[1:]:
        key, _, value = token.partition("=")
        out[key] = [int(v) for v in value.split(",")] if "," in value else int(value)
    return out


class Logger:
    """Writes one CSV row per board per second, plus notes/commands, and per-channel counters every 10 s."""

    COLUMNS = ["time", "elapsed_s", "host", "role", "connected", "rssi_dbm", "snr_db", "lq_pct", "frames_rx",
               "frames_lost", "crc_err", "lbt_busy", "lbt_invalid", "sync_lost", "cca_dbm", "temp_c", "tx_dbm",
               "sf", "bw_khz", "cr", "master_frame_b", "slave_frame_b", "hop_us", "data_tx_kbps", "data_rx_kbps",
               "gen_kbps", "gen_lost", "echo_kbps", "event"]

    def __init__(self, hosts, directory):
        self.hosts = hosts
        self.dir = pathlib.Path(directory)
        self.lock = threading.Lock()
        self.thread = None
        self.stop_flag = threading.Event()
        self.state = {"active": False}
        self.writer = self.file = self.chan_writer = self.chan_file = None

    def start(self, name=""):
        with self.lock:
            if self.state.get("active"):
                return False, "already logging to " + self.state["file"]
            self.dir.mkdir(parents=True, exist_ok=True)
            stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
            safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in name.strip())[:40]
            base = self.dir / (stamp + (f"-{safe}" if safe else ""))
            self.file = open(f"{base}.csv", "w", newline="")
            self.chan_file = open(f"{base}-channels.csv", "w", newline="")
            self.writer = csv.writer(self.file)
            self.chan_writer = csv.writer(self.chan_file)
            self.writer.writerow(self.COLUMNS)
            self.chan_writer.writerow(["time", "elapsed_s", "host", "channel", "freq_mhz", "tx", "rx", "lbt_busy"])
            self.state = {"active": True, "file": f"{base}.csv", "rows": 0, "started": time.time()}
            self.stop_flag.clear()
            self.thread = threading.Thread(target=self._run, daemon=True)
            self.thread.start()
        self.event("", "logging started")
        return True, f"logging to {base}.csv"

    def stop(self):
        if not self.state.get("active"):
            return False, "not logging"
        self.stop_flag.set()
        self.thread.join(timeout=3)
        self.event("", "logging stopped")
        with self.lock:
            self.file.close()
            self.chan_file.close()
            self.state["active"] = False
        return True, "stopped, saved " + self.state["file"]

    def snapshot(self):
        with self.lock:
            return dict(self.state)

    def event(self, host, text):
        with self.lock:
            if not self.state.get("active"):
                return
            now = time.time()
            row = {"time": datetime.datetime.fromtimestamp(now).isoformat(timespec="milliseconds"),
                   "elapsed_s": f"{now - self.state['started']:.1f}", "host": host, "event": text}
            self.writer.writerow([row.get(c, "") for c in self.COLUMNS])
            self.file.flush()

    def _row(self, host, cur, prev, now):
        st, dt = cur, (now - prev[0]) if prev else 0
        old = prev[1] if prev else None

        def delta(key):
            return (st.get(key, 0) - old.get(key, 0)) if old and key in st and key in old else ""

        frames = delta("frames.rx")
        avg = lambda k: f"{(st[k] - old[k]) / frames:.1f}" if old and frames and k in st and k in old else ""
        rate = lambda k: f"{(st[k] - old[k]) * 8 / dt / 1000:.3f}" if old and dt and k in st and k in old else ""
        lq = f"{min(100.0, 100 * frames / (dt * 1e6 / st['frame_us'])):.1f}" if old and dt and st.get("frame_us") else ""
        echo = ECHO.snapshot()
        echo_kbps = f"{echo['kbps']:.3f}" if echo.get("running") and echo.get("host") == host else ""
        temp = st.get("board.temp_c")
        return {"time": datetime.datetime.fromtimestamp(now).isoformat(timespec="milliseconds"),
                "elapsed_s": f"{now - self.state['started']:.1f}", "host": host, "role": st.get("role"),
                "connected": st.get("connected"), "rssi_dbm": avg("rssi_sum") or st.get("rssi"),
                "snr_db": avg("snr_sum") or st.get("snr"), "lq_pct": lq, "frames_rx": frames,
                "frames_lost": delta("frames.lost"), "crc_err": delta("frames.crc_err"),
                "lbt_busy": delta("frames.lbt_busy"), "lbt_invalid": delta("frames.lbt_invalid"),
                "sync_lost": delta("frames.sync_lost"), "cca_dbm": st.get("cca_rssi"),
                "temp_c": temp if isinstance(temp, (int, float)) else "", "tx_dbm": st.get("tx_dbm"),
                "sf": st.get("modem.sf"), "bw_khz": st.get("modem.bw"), "cr": st.get("modem.cr"),
                "master_frame_b": st.get("modem.mp"), "slave_frame_b": st.get("modem.sp"), "hop_us": st.get("frame_us"),
                "data_tx_kbps": rate("bytes.radio_tx"), "data_rx_kbps": rate("bytes.radio_rx"),
                "gen_kbps": rate("gen.rx_bytes") if st.get("role") == "master" else "",
                "gen_lost": delta("gen.lost") if st.get("role") == "master" else "", "echo_kbps": echo_kbps}

    def _run(self):
        prev = {}
        next_chan = time.time()
        while not self.stop_flag.is_set():
            tick = time.time()
            for host in self.hosts:
                text = query_status(host, timeout=0.4)
                now = time.time()
                if text is None:
                    prev.pop(host, None)
                    with self.lock:
                        self.state["rows"] += 1
                        self.writer.writerow([datetime.datetime.fromtimestamp(now).isoformat(timespec="milliseconds"),
                                              f"{now - self.state['started']:.1f}", host] + [""] * (len(self.COLUMNS) - 4)
                                             + ["no reply"])
                    continue
                st = parse_status(text)
                row = self._row(host, st, prev.get(host), now)
                prev[host] = (now, st)
                with self.lock:
                    self.writer.writerow([row.get(c, "") for c in self.COLUMNS])
                    self.state["rows"] += 1
                if st.get("role") == "master" and time.time() >= next_chan:
                    ch = query_channels(host)
                    if ch and "tx" in ch:
                        with self.lock:
                            for i in range(len(ch["tx"])):
                                self.chan_writer.writerow([row["time"], row["elapsed_s"], host, i,
                                                          (ch["base_khz"] + i * ch["step_khz"]) / 1000,
                                                          ch["tx"][i], ch["rx"][i], ch["busy"][i]])
                            self.chan_file.flush()
                    next_chan = time.time() + 10
            with self.lock:
                self.file.flush()
            self.stop_flag.wait(max(0.0, 1.0 - (time.time() - tick)))


LOGGER = None


def send_command(host, text, timeout=1.0):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto(text.encode(), (host, STATUS_PORT))
        reply, _ = s.recvfrom(2048)
        return reply.decode(errors="replace").strip()


def make_handler(hosts):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):  # keep the terminal quiet
            pass

        def reply(self, code, body, content_type="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            try:
                self.send_response(code)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass  # the browser gave up on this request (reload, closed tab, slow board): nothing to do

        def do_GET(self):
            url = urlparse(self.path)
            if url.path == "/":
                self.reply(200, PAGE.read_bytes(), "text/html; charset=utf-8")
            elif url.path == "/api/echo":
                self.reply(200, ECHO.snapshot())
            elif url.path == "/api/log":
                self.reply(200, LOGGER.snapshot())
            elif url.path == "/api/chan":
                host = parse_qs(url.query).get("host", [""])[0]
                if host not in hosts:
                    self.reply(400, {"error": "unknown host"})
                    return
                self.reply(200, query_channels(host) or {"error": "no channel data (old firmware or no answer)"})
            elif url.path == "/api/hosts":
                self.reply(200, {"hosts": hosts})
            elif url.path == "/api/status":
                host = parse_qs(url.query).get("host", [""])[0]
                if host not in hosts:
                    self.reply(400, {"error": "unknown host"})
                    return
                text = query_status(host, timeout=0.6)
                if text is None:
                    self.reply(200, {"host": host, "online": False})
                else:
                    self.reply(200, {"host": host, "online": True, "status": parse_status(text)})
            else:
                self.reply(404, {"error": "not found"})

        def do_POST(self):
            path = urlparse(self.path).path
            try:
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            except json.JSONDecodeError:
                self.reply(400, {"error": "invalid JSON"})
                return
            if path == "/api/log/start":
                ok, msg = LOGGER.start(str(body.get("name", "")))
                self.reply(200, {"ok": ok, "reply": msg})
                return
            if path == "/api/log/stop":
                ok, msg = LOGGER.stop()
                self.reply(200, {"ok": ok, "reply": msg})
                return
            if path == "/api/log/note":
                text = str(body.get("text", "")).strip()[:300]
                if text:
                    LOGGER.event("", "note: " + text)
                self.reply(200, {"ok": bool(text), "reply": "noted" if text else "empty note"})
                return
            if path == "/api/echo/start":
                host = body.get("host")
                if host not in hosts:
                    self.reply(400, {"error": "unknown host"})
                    return
                size = max(ECHO_HEADER.size, min(1472, int(body.get("size", 256))))
                window = max(1, min(8, int(body.get("window", 6))))
                duration = max(1.0, min(3600.0, float(body.get("duration", 30))))
                ok, msg = ECHO.start(host, size, window, duration)
                if ok:
                    LOGGER.event(host, f"echo test start: {size} B, {window} in flight, {duration:g} s")
                self.reply(200, {"ok": ok, "reply": msg})
                return
            if path == "/api/echo/stop":
                ECHO.stop()
                self.reply(200, {"ok": True, "reply": "stopping"})
                return
            if path != "/api/command":
                self.reply(404, {"error": "not found"})
                return
            host, cmd = body.get("host"), str(body.get("cmd", "")).strip()
            if host not in hosts or not cmd.startswith(ALLOWED_COMMANDS):
                self.reply(400, {"error": "unknown host or command"})
                return
            try:
                answer = send_command(host, cmd)
            except OSError:
                LOGGER.event(host, f"command: {cmd} -> no answer")
                self.reply(200, {"ok": False, "reply": "no answer from the board"})
                return
            LOGGER.event(host, f"command: {cmd} -> {answer}")
            self.reply(200, {"ok": answer.startswith("ok"), "reply": answer})

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hosts", nargs="+", default=DEFAULT_HOSTS, help="board IP addresses")
    parser.add_argument("--port", type=int, default=8080, help="local web server port")
    parser.add_argument("--no-browser", action="store_true", help="do not open the browser")
    parser.add_argument("--log-dir", default=str(pathlib.Path(__file__).resolve().parent.parent / "logs"),
                        help="where log files go (default: logs/ in the repository)")
    parser.add_argument("--log", action="store_true", help="start logging immediately (otherwise use the button)")
    args = parser.parse_args()

    global LOGGER
    LOGGER = Logger(args.hosts, args.log_dir)
    if args.log:
        print(LOGGER.start()[1])

    class QuietServer(ThreadingHTTPServer):
        daemon_threads = True

        def handle_error(self, request, client_address):
            if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
                return  # browser closed the connection early: harmless
            super().handle_error(request, client_address)

    server = QuietServer(("127.0.0.1", args.port), make_handler(args.hosts))
    url = f"http://127.0.0.1:{args.port}/"
    print(f"Control panel at {url}  (boards: {', '.join(args.hosts)})  Ctrl+C to quit")
    if not args.no_browser:
        threading.Timer(0.5, webbrowser.open, args=(url,)).start()
    def on_term(signum, frame):  # also close the log cleanly when killed (SIGTERM)
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, on_term)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if LOGGER.snapshot().get("active"):
            print(LOGGER.stop()[1])
        server.server_close()


if __name__ == "__main__":
    main()
