"""ZyDeck Link: a phone or tablet running ZyDeck in controller mode, as a MIDI controller for Mixxx here.

What it does, with no special permissions (no Accessibility, no Automation, no USB debugging):
  * makes a MIDI port, "ZyDeck Link", for Mixxx (load the ZyDeck mapping on it in Preferences > Controllers)
  * listens on every MIDI input for ZyDeck's hello over the USB cable (F0 7D 60 ... F7) and answers with this
    computer's address and a key (F0 7D 61); the device then offers to link over Wi-Fi, and the cable can go
  * relays the link: TCP port 8771, lines "M <hex MIDI>" both ways between the device and the ZyDeck Link
    port, and "J <json>" for track names
  * names tracks from their length (Mixxx's mapping only sees numbers), over the cable (F0 7D 62/63) or Wi-Fi
  * serves Mixxx's analysed waveforms and beat grids to the device (GET /waveform/<id>, /beats/<id>, ?k=key)
  * installs the ZyDeck mapping into Mixxx's controllers folder (a button)

SysEx text is the hex of UTF-8 JSON (MIDI data bytes are 7-bit). Run: python zydeck_link.py [--no-window]
"""

import argparse
import asyncio
import gzip
import json
import os
import platform
import re
import secrets
import socket
import sqlite3
import struct
import sys
import threading
import time
import zlib
from functools import lru_cache
from pathlib import Path

import rtmidi

APP = "ZyDeck Link"
PORT_NAME = "ZyDeck Link"
TCP_PORT = 8771
LINK_HELLO, LINK_OFFER, NAME_ASK, NAME_REPLY = 0x60, 0x61, 0x62, 0x63
HERE = Path(getattr(sys, "_MEIPASS", Path(__file__).parent))   # the mapping files, also when bundled

if sys.platform == "darwin":
    DATA = Path.home() / "Library/Application Support/ZyDeck Link"
    MIXXX_DIRS = [Path.home() / "Library/Containers/org.mixxx.mixxx/Data/Library/Application Support/Mixxx",
                  Path.home() / "Library/Application Support/Mixxx"]
else:
    DATA = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "zydeck-link"
    MIXXX_DIRS = [Path.home() / ".mixxx", Path.home() / ".local/share/mixxx",
                  Path.home() / ".var/app/org.mixxx.Mixxx/.mixxx"]


def mixxx_dir() -> Path | None:
    for d in MIXXX_DIRS:
        if (d / "mixxxdb.sqlite").is_file():
            return d
    return None


def sx_text(kind: int, obj) -> list[int]:
    return [0xF0, 0x7D, kind, *json.dumps(obj, separators=(",", ":")).encode().hex().encode(), 0xF7]


def sx_json(msg) -> dict | None:
    try:
        return json.loads(bytes.fromhex(bytes(msg[3:-1]).decode("ascii")))
    except (ValueError, UnicodeDecodeError):
        return None


def lan_addresses() -> list[str]:
    """This computer's LAN IPv4 addresses, the default route's first."""
    found = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.0.1", 9))   # no packet is sent: it only picks the outgoing interface
        found.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            found.append(info[4][0])
    except OSError:
        pass
    private = re.compile(r"^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)")
    out = []
    for a in found:
        if private.match(a) and a not in out:
            out.append(a)
    return out


# ---- Mixxx's library ---------------------------------------------------------------------------------

class Library:
    def __init__(self):
        self._rows: list[tuple[float, str, str, int]] = []
        self._loaded = 0.0
        self._lock = threading.Lock()

    def _connect(self):
        d = mixxx_dir()
        return sqlite3.connect(f"file:{d / 'mixxxdb.sqlite'}?mode=ro", uri=True, timeout=2) if d else None

    def _load(self):
        con = self._connect()
        if not con:
            return
        try:
            self._rows = [(dur, (title or Path(loc).stem).strip(), (artist or "").strip(), tid)
                          for dur, title, artist, loc, tid in con.execute(
                              "SELECT l.duration, l.title, l.artist, tl.location, l.id FROM library l "
                              "JOIN track_locations tl ON tl.id = l.location WHERE l.duration > 0 ORDER BY l.mixxx_deleted")]
            self._loaded = time.time()
        except sqlite3.Error:
            pass
        finally:
            con.close()

    def name(self, ms: int) -> dict | None:
        """A track by its length (ms): Mixxx stores lengths to the millisecond."""
        for attempt in range(6):
            with self._lock:
                if attempt or time.time() - self._loaded > 5:
                    self._load()   # a track added a moment ago
                best = min(self._rows, key=lambda r: abs(r[0] * 1000 - ms), default=None)
            if best and abs(best[0] * 1000 - ms) <= 1000:
                return {"t": best[1], "a": best[2], "id": best[3]}
            time.sleep(0.5)
        return None

    def waveform(self, track_id: int) -> bytes | None:
        d, con = mixxx_dir(), self._connect()
        if not con:
            return None
        try:
            row = con.execute("SELECT id FROM track_analysis WHERE track_id = ? AND type = 1 ORDER BY id DESC LIMIT 1",
                              (track_id,)).fetchone()
        finally:
            con.close()
        p = d / "analysis" / str(row[0]) if row else None
        return packed_waveform(str(p), p.stat().st_mtime) if p and p.is_file() else None

    def beats(self, track_id: int) -> dict | None:
        con = self._connect()
        if not con:
            return None
        try:
            row = con.execute("SELECT beats, beats_version, samplerate, duration, bpm_lock FROM library WHERE id = ?",
                              (track_id,)).fetchone()
        finally:
            con.close()
        if not row or not row[0] or not row[2]:
            return None
        blob, version, rate, duration, locked = row
        return decode_beats(bytes(blob), version or "", rate, duration or 0, bool(locked))


# protobuf, just enough for Mixxx's waveforms and beats
def _varint(b, i):
    v = s = 0
    while True:
        x = b[i]
        i += 1
        v |= (x & 0x7F) << s
        s += 7
        if x < 0x80:
            return v, i


def _fields(b):
    i, n = 0, len(b)
    while i < n:
        key, i = _varint(b, i)
        f, t = key >> 3, key & 7
        if t == 0:
            v, i = _varint(b, i)
        elif t == 1:
            v = struct.unpack_from("<d", b, i)[0]
            i += 8
        elif t == 2:
            ln, i = _varint(b, i)
            v = bytes(b[i:i + ln])
            i += ln
        elif t == 5:
            v = struct.unpack_from("<f", b, i)[0]
            i += 4
        else:
            raise ValueError(f"wire type {t}")
        yield f, t, v


def _signal(b: bytes) -> bytes:
    out = bytearray()
    for f, t, v in _fields(b):
        if f == 1 and t == 0:
            out.append(min(255, v))
        elif f == 1 and t == 2:
            j = 0
            while j < len(v):
                x, j = _varint(v, j)
                out.append(min(255, x))
    return bytes(out)


@lru_cache(maxsize=16)
def packed_waveform(path: str, _mtime: float) -> bytes:
    """Mixxx's analysis file (qCompress'd protobuf, waveform.proto) as ZyDeck's "MXWF" + rate + count +
    all/low/mid/high per visual sample (louder of left/right), gzipped."""
    raw = Path(path).read_bytes()
    data = zlib.decompress(raw[4:]) if len(raw) > 4 and struct.unpack(">I", raw[:4])[0] else raw
    rate, sig = 441.0, {}
    for f, _, v in _fields(data):
        if f == 1:
            rate = v
        elif f == 3:
            sig["all"] = _signal(v)
        elif f == 4:
            for g, _, w in _fields(v):
                if g in (1, 2, 3):
                    sig[("low", "mid", "high")[g - 1]] = _signal(w)
    merged = {k: bytes(max(a, b) for a, b in zip(s[0::2], s[1::2])) for k, s in sig.items()}
    n = min((len(merged.get(k, b"")) for k in ("all", "low", "mid", "high")), default=0)
    body = bytearray(n * 4)
    for k, off in (("all", 0), ("low", 1), ("mid", 2), ("high", 3)):
        body[off::4] = merged[k][:n]
    return gzip.compress(b"MXWF" + struct.pack("<fI", rate, n) + bytes(body), compresslevel=5)


def decode_beats(blob: bytes, version: str, rate: int, duration: float, locked: bool) -> dict | None:
    """{"beats": [seconds], "bar": k, "locked"} as the phone's hub gives them: beat i starts a bar when
    (i + k) % 4 == 0, counting 4/4 bars from the first beat marker like Mixxx."""
    end = duration * rate
    try:
        if version.startswith("BeatGrid"):
            first, bpm = None, None
            for f, _, v in _fields(blob):
                if f == 1:
                    bpm = next((x for g, _, x in _fields(v) if g == 1), None)
                elif f == 2:
                    first = next((x for g, _, x in _fields(v) if g == 1), 0)
            if not bpm or bpm <= 0 or first is None:
                return None
            step = 60.0 * rate / bpm
            k = int(first // step)                   # beats before the marker, back to the start
            start = first - k * step
            frames = []
            x = start
            while x < end and len(frames) < 20000:
                frames.append(x)
                x += step
            bar = (-k) % 4
        elif version.startswith("BeatMap"):
            frames = [next((x for g, _, x in _fields(v) if g == 1), 0) for f, _, v in _fields(blob) if f == 1]
            frames = [x for x in frames if x < end][:20000]
            bar = 0
        else:
            return None
    except (ValueError, IndexError, struct.error):
        return None
    return {"beats": [round(x / rate, 4) for x in frames], "bar": bar, "locked": locked}


# ---- MIDI ------------------------------------------------------------------------------------------

class Midi:
    """The ZyDeck Link port for Mixxx, and every other MIDI input watched for ZyDeck's hello."""

    def __init__(self, link):
        self.link = link
        self.lock = threading.Lock()
        self.out = rtmidi.MidiOut(name=APP)
        self.out.open_virtual_port(PORT_NAME)
        self.inp = rtmidi.MidiIn(name=APP)
        self.inp.open_virtual_port(PORT_NAME)
        self.inp.ignore_types(sysex=False, timing=True, active_sense=True)
        self.inp.set_callback(lambda ev, _: link.from_mixxx(ev[0]))
        self.watched: dict[str, rtmidi.MidiIn] = {}
        self.replies: dict[str, rtmidi.MidiOut] = {}
        self.devices: dict[str, float] = {}   # port -> when ZyDeck said hello over it
        threading.Thread(target=self._watch, daemon=True).start()

    def to_mixxx(self, data: list[int]):
        with self.lock:
            self.out.send_message(data)

    def _watch(self):
        while True:
            try:
                probe = rtmidi.MidiIn(name=APP + " probe")
                ports = probe.get_ports()
                del probe
                for i, name in enumerate(ports):
                    if name in self.watched or PORT_NAME in name or "Through" in name:
                        continue
                    m = rtmidi.MidiIn(name=APP)
                    m.open_port(i)
                    m.ignore_types(sysex=False, timing=True, active_sense=True)
                    m.set_callback(lambda ev, _, port=name: self._from_cable(port, ev[0]))
                    self.watched[name] = m
                for name in list(self.watched):
                    if name not in ports:   # unplugged
                        self.watched.pop(name).close_port()
                        self.replies.pop(name, None)
                        self.devices.pop(name, None)
            except rtmidi.RtMidiError:
                pass
            time.sleep(2)

    def _reply(self, port: str, data: list[int]):
        out = self.replies.get(port)
        if out is None:
            out = rtmidi.MidiOut(name=APP)
            base = re.sub(r"\s+\d+:\d+$", "", port)   # ALSA adds client:port numbers
            names = out.get_ports()
            index = next((i for i, n in enumerate(names) if n == port), None)
            if index is None:
                index = next((i for i, n in enumerate(names) if re.sub(r"\s+\d+:\d+$", "", n) == base), None)
            if index is None:
                return
            out.open_port(index)
            self.replies[port] = out
        out.send_message(data)

    def _from_cable(self, port: str, msg: list[int]):
        if len(msg) < 4 or msg[0] != 0xF0 or msg[1] != 0x7D or msg[2] not in (LINK_HELLO, NAME_ASK):
            return   # the mapping's own messages: Mixxx reads those from this port itself
        j = sx_json(msg)
        if j is None:
            return
        if msg[2] == LINK_HELLO:
            self.devices[port] = time.time()
            self.link.device_seen(j.get("name") or port, "USB cable")
            self._reply(port, sx_text(LINK_OFFER, self.link.offer()))
        else:
            threading.Thread(target=lambda: self._reply(port, sx_text(NAME_REPLY, self.link.name_reply(j))),
                             daemon=True).start()


# ---- the link ----------------------------------------------------------------------------------------

class Link:
    def __init__(self):
        DATA.mkdir(parents=True, exist_ok=True)
        self.conf_path = DATA / "link.json"
        try:
            self.conf = json.loads(self.conf_path.read_text())
        except (OSError, ValueError):
            self.conf = {}
        if not self.conf.get("key"):
            self.conf["key"] = secrets.token_hex(12)
            self._save()
        self.key = self.conf["key"]
        self.name = platform.node().split(".")[0] or "This computer"
        self.library = Library()
        self.clients: dict[asyncio.StreamWriter, str] = {}
        self.seen: dict[str, tuple[str, float]] = {}   # device -> (how, when)
        self.loop: asyncio.AbstractEventLoop | None = None
        self.midi = Midi(self)
        self.status = ""

    def _save(self):
        self.conf_path.write_text(json.dumps(self.conf, indent=1))

    def offer(self) -> dict:
        return {"name": self.name, "hosts": lan_addresses(), "port": TCP_PORT, "key": self.key}

    def device_seen(self, name: str, how: str):
        self.seen[name] = (how, time.time())

    def name_reply(self, ask: dict) -> dict:
        n = self.library.name(int(ask.get("ms") or 0)) if ask.get("ms") else None
        return {"s": bool(ask.get("s")), "i": int(ask.get("i") or 0), "n": n}

    # Mixxx -> linked devices
    def from_mixxx(self, msg: list[int]):
        if self.loop and self.clients:
            line = b"M " + bytes(msg).hex().encode() + b"\n"
            self.loop.call_soon_threadsafe(self._broadcast, line)

    def _broadcast(self, line: bytes):
        for w in list(self.clients):
            try:
                w.write(line)
            except (ConnectionError, RuntimeError):
                self.clients.pop(w, None)

    async def serve(self):
        self.loop = asyncio.get_running_loop()
        server = await asyncio.start_server(self._client, "0.0.0.0", TCP_PORT)
        async with server:
            await server.serve_forever()

    async def _client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        sock = writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            first = await asyncio.wait_for(reader.readline(), 10)
        except (asyncio.TimeoutError, ConnectionError):
            writer.close()
            return
        if first.startswith(b"GET "):
            await self._http(first, reader, writer)
            return
        parts = first.decode(errors="replace").split()
        if len(parts) < 2 or parts[0] != "ZYDECK" or not secrets.compare_digest(parts[1], self.key):
            writer.write(b"NO\n")
            await writer.drain()
            writer.close()
            return
        try:
            device = bytes.fromhex(parts[2]).decode() if len(parts) > 2 else "ZyDeck"
        except ValueError:
            device = "ZyDeck"
        writer.write(b"OK " + self.name.encode().hex().encode() + b"\n")
        self.clients[writer] = device
        self.device_seen(device, "Wi-Fi")
        try:
            while line := await reader.readline():
                if line.startswith(b"M "):
                    try:
                        data = list(bytes.fromhex(line[2:].strip().decode()))
                    except ValueError:
                        continue
                    if data:
                        self.midi.to_mixxx(data)
                elif line.startswith(b"J "):
                    try:
                        ask = json.loads(line[2:])
                    except ValueError:
                        continue
                    reply = await asyncio.to_thread(self.name_reply, ask)
                    writer.write(b"J " + json.dumps(reply, separators=(",", ":")).encode() + b"\n")
        except ConnectionError:
            pass
        finally:
            self.clients.pop(writer, None)
            self.seen.pop(device, None)
            writer.close()

    async def _http(self, first: bytes, reader, writer):
        while (await reader.readline()).strip():
            pass   # headers
        m = re.match(rb"GET /(waveform|beats)/(\d+)(?:\?k=([0-9a-f]+))?", first)
        status, body, headers = 404, b"not found\n", {"Content-Type": "text/plain"}
        if m and m.group(3) and secrets.compare_digest(m.group(3).decode(), self.key):
            if m.group(1) == b"waveform":
                data = await asyncio.to_thread(self.library.waveform, int(m.group(2)))
                if data:
                    status, body = 200, data
                    headers = {"Content-Type": "application/octet-stream", "Content-Encoding": "gzip"}
            else:
                data = await asyncio.to_thread(self.library.beats, int(m.group(2)))
                if data:
                    status, body, headers = 200, json.dumps(data).encode(), {"Content-Type": "application/json"}
        elif m:
            status, body = 403, b"wrong key\n"
        head = f"HTTP/1.1 {status} {'OK' if status == 200 else 'No'}\r\nContent-Length: {len(body)}\r\n" \
               f"Access-Control-Allow-Origin: *\r\nCache-Control: max-age=300\r\nConnection: close\r\n"
        head += "".join(f"{k}: {v}\r\n" for k, v in headers.items())
        writer.write(head.encode() + b"\r\n" + body)
        await writer.drain()
        writer.close()


# ---- the mapping, and opening at login ---------------------------------------------------------------

def mapping_installed() -> bool:
    d = mixxx_dir()
    return bool(d and (d / "controllers" / "ZyDeck.midi.xml").is_file())


def mapping_files() -> dict[str, str]:
    """The ZyDeck mapping: bundled (mapping/), or made from the phone's own (res/controllers) when run from
    the source tree. It's the same mapping, named for the desktop."""
    bundled = HERE / "mapping"
    if (bundled / "ZyDeck.midi.xml").is_file():
        return {f.name: f.read_text() for f in bundled.iterdir() if f.suffix in (".xml", ".js")}
    src = HERE.parent.parent / "res" / "controllers"
    xml = (src / "Zydek-Tablet.midi.xml").read_text()
    xml = re.sub(r"<name>.*?</name>", "<name>ZyDeck</name>", xml, count=1)
    xml = re.sub(r"<description>.*?</description>",
                 "<description>A phone or tablet running ZyDeck in controller mode: over the USB cable (choose MIDI in "
                 "Android's USB notification) or over Wi-Fi through ZyDeck Link.</description>", xml, count=1, flags=re.S)
    xml = xml.replace('filename="Zydek-Tablet.script.js"', 'filename="ZyDeck.script.js"')
    return {"ZyDeck.midi.xml": xml, "ZyDeck.script.js": (src / "Zydek-Tablet.script.js").read_text()}


def install_mapping() -> str:
    target = (mixxx_dir() or MIXXX_DIRS[0]) / "controllers"
    target.mkdir(parents=True, exist_ok=True)
    for name, text in mapping_files().items():
        (target / name).write_text(text)
    return str(target)


AGENT = Path.home() / "Library/LaunchAgents/org.zydeck.link.plist"
AUTOSTART = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "autostart/zydeck-link.desktop"


def _launch_command() -> list[str]:
    if getattr(sys, "frozen", False):
        exe = Path(sys.executable)
        app = next((p for p in exe.parents if p.suffix == ".app"), None)
        return ["/usr/bin/open", "-a", str(app)] if app else [str(exe)]
    return [sys.executable, str(Path(__file__).resolve())]


def open_at_login() -> bool:
    return AGENT.is_file() if sys.platform == "darwin" else AUTOSTART.is_file()


def set_open_at_login(on: bool):
    if sys.platform == "darwin":
        if on:
            args = "".join(f"<string>{a}</string>" for a in _launch_command())
            AGENT.parent.mkdir(parents=True, exist_ok=True)
            AGENT.write_text(f'<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
                             f'"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict>'
                             f"<key>Label</key><string>org.zydeck.link</string><key>ProgramArguments</key><array>{args}</array>"
                             f"<key>RunAtLoad</key><true/></dict></plist>\n")
        else:
            AGENT.unlink(missing_ok=True)
    else:
        if on:
            AUTOSTART.parent.mkdir(parents=True, exist_ok=True)
            AUTOSTART.write_text("[Desktop Entry]\nType=Application\nName=ZyDeck Link\n"
                                 f"Exec={' '.join(_launch_command())}\nX-GNOME-Autostart-enabled=true\n")
        else:
            AUTOSTART.unlink(missing_ok=True)


# ---- window ------------------------------------------------------------------------------------------

def window(link: Link):
    import tkinter as tk
    from tkinter import ttk

    root = tk.Tk()
    root.title(APP)
    root.minsize(420, 0)
    frm = ttk.Frame(root, padding=16)
    frm.grid(sticky="nsew")
    title = ttk.Label(frm, text=APP, font=("", 16, "bold"))
    title.grid(sticky="w")
    lines = {k: ttk.Label(frm, wraplength=420, justify="left") for k in ("port", "mixxx", "devices", "here")}
    for i, k in enumerate(("port", "mixxx", "devices", "here")):
        lines[k].grid(row=i + 1, sticky="w", pady=(8, 0))
    buttons = ttk.Frame(frm)
    buttons.grid(row=6, sticky="we", pady=(14, 0))
    install = ttk.Button(buttons, text="Install the ZyDeck mapping in Mixxx")
    install.grid(row=0, column=0, sticky="w")
    login = tk.BooleanVar(value=open_at_login())
    ttk.Checkbutton(buttons, text="Open at login (so Mixxx always finds the port)", variable=login,
                    command=lambda: set_open_at_login(login.get())).grid(row=1, column=0, sticky="w", pady=(8, 0))
    ttk.Button(buttons, text="Quit", command=root.destroy).grid(row=2, column=0, sticky="e", pady=(12, 0))

    def do_install():
        try:
            where = install_mapping()
            link.status = f"Installed in {where}. Restart Mixxx if it's open."
        except OSError as e:
            link.status = f"Couldn't install the mapping: {e}"
    install.configure(command=do_install)

    def refresh():
        lines["port"].configure(text=f"MIDI port for Mixxx: {PORT_NAME}. In Mixxx: Preferences › Controllers › "
                                     f"{PORT_NAME} › load ZyDeck, tick Enabled. Mixxx only finds MIDI ports when it "
                                     "starts: start ZyDeck Link first.")
        d = mixxx_dir()
        lines["mixxx"].configure(text=(link.status + "\n" if link.status else "") +
                                 (f"Mixxx library: {d}" if d else "Mixxx library not found: no track names or waveforms") +
                                 ("\nZyDeck mapping: installed" if mapping_installed() else "\nZyDeck mapping: not installed yet"))
        now = time.time()
        devs = [f"• {n} ({how})" for n, (how, t) in link.seen.items() if how == "Wi-Fi" or now - t < 10]
        lines["devices"].configure(text="Devices:\n" + ("\n".join(devs) if devs else
                                   "none yet. On the phone or tablet: ZyDeck › Settings › Controller mode, then plug in "
                                   "the USB cable and choose MIDI in Android's USB notification."))
        lines["here"].configure(text=f"This computer: {link.name} · {', '.join(lan_addresses()) or 'no network'} · port {TCP_PORT}")
        root.after(1000, refresh)
    refresh()
    root.mainloop()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--no-window", action="store_true", help="run without the window (Ctrl+C stops it)")
    ap.add_argument("--install-mapping", action="store_true", help="install the ZyDeck mapping and quit")
    ap.add_argument("--write-mapping", metavar="DIR", help="write the mapping files into DIR and quit (build.sh)")
    args = ap.parse_args()
    if args.install_mapping:
        print(install_mapping())
        return
    if args.write_mapping:
        Path(args.write_mapping).mkdir(parents=True, exist_ok=True)
        for name, text in mapping_files().items():
            (Path(args.write_mapping) / name).write_text(text)
        return
    link = Link()
    t = threading.Thread(target=lambda: asyncio.run(link.serve()), daemon=True)
    t.start()
    if args.no_window:
        print(f"{APP}: MIDI port '{PORT_NAME}', {', '.join(lan_addresses())} port {TCP_PORT}")
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            pass
    else:
        window(link)


if __name__ == "__main__":
    main()
