"""Zydek's Web tab: search YouTube (or open a Bilibili link / BV code) and download the audio, with yt-dlp.

Runs inside Mixxx in an embedded Python (src/zydek/zydekpython.cpp), which calls start() once and then
api(name, args_json) for each /api/web/<name> request from the phone page. Calls return quickly: searches
and downloads are jobs running in their own threads; the page polls /api/web/job?id=.

No stem separation here: on a phone that would take too long and too much battery.
"""

import json
import os
import re
import sys
import threading
import time
import traceback
import urllib.request
import uuid

import _zydekjs

MUSIC_SUBDIR = "Zydek Web"
SEARCH_RESULTS = 20
# Mixxx on Android reads AAC (m4a) but not Opus in WebM: ask for AAC first (YouTube's itag 140, Bilibili's
# audio is AAC anyway).
AUDIO_FORMAT = "bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]/bestaudio/best"
QUICKJS_STACK = 24 * 1024 * 1024
THREAD_STACK = 64 * 1024 * 1024

_JUNK = re.compile(
    r"\s*[\(\[][^\)\]]*\b(official|video|audio|lyrics?|visuali[sz]er|hd|hq|4k|mv|m/v|clip)\b[^\)\]]*[\)\]]",
    re.IGNORECASE,
)
_BV = re.compile(r"\b(BV[0-9A-Za-z]{10})\b")
_URL = re.compile(r"https?://\S+")

_dirs = {}
_jobs = {}
_lock = threading.Lock()


class _Log:
    """sys.stdout / sys.stderr and yt-dlp's logger, into Mixxx's log."""

    def __init__(self, prefix=""):
        self.prefix, self.buf = prefix, ""

    def write(self, s):
        self.buf += s
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            if line.strip():
                _zydekjs.log(self.prefix + line)
        return len(s)

    def flush(self):
        pass

    # yt-dlp's logger interface
    def debug(self, msg):
        pass

    def info(self, msg):
        pass

    def warning(self, msg):
        _zydekjs.log("yt-dlp warning: " + msg)

    def error(self, msg):
        _zydekjs.log("yt-dlp error: " + msg)


def start(data_dir, cache_dir, music_dir):
    sys.stdout = _Log()
    sys.stderr = _Log("stderr: ")
    sys.setswitchinterval(0.002)        # Mixxx's controller thread waits for the GIL on each call: keep it short
    threading.stack_size(THREAD_STACK)  # QuickJS needs a deep stack for YouTube's player code
    import certifi
    os.environ["SSL_CERT_FILE"] = certifi.where()
    _dirs.update(data=data_dir, cache=cache_dir, music=os.path.join(music_dir, MUSIC_SUBDIR))
    _patch_quickjs()
    # Import yt-dlp (slow the first time: Python compiles it) while nobody waits for it yet.
    threading.Thread(target=lambda: __import__("yt_dlp"), daemon=True).start()


def _patch_quickjs():
    """yt-dlp runs QuickJS as a program (qjs); here it runs inside Mixxx instead (_zydekjs.run)."""
    from yt_dlp.extractor.youtube.jsc._builtin import quickjs
    from yt_dlp.extractor.youtube.jsc.provider import JsChallengeProviderError
    from yt_dlp.utils._jsruntime import JsRuntimeInfo

    info = JsRuntimeInfo(name="quickjs-ng", path="(built into Zydek)", version="0.17.0", version_tuple=(0, 17, 0))

    def run(self, stdin, /):
        try:
            return _zydekjs.run(stdin, QUICKJS_STACK)
        except RuntimeError as e:
            raise JsChallengeProviderError(f"QuickJS: {e}") from e

    quickjs.QuickJSJCP.runtime_info = property(lambda self: info)
    quickjs.QuickJSJCP._run_js_runtime = run


# ---- jobs -----------------------------------------------------------------------------------------

def _job(kind, title, fn):
    job = {"id": uuid.uuid4().hex[:10], "kind": kind, "title": title, "state": "running", "progress": 0.0,
           "message": "", "result": None, "error": None, "started": time.time()}
    with _lock:
        _jobs[job["id"]] = job
        for old in sorted(_jobs.values(), key=lambda j: j["started"])[:-40]:   # keep the last 40
            if old["state"] != "running":
                _jobs.pop(old["id"], None)

    def work():
        try:
            job["result"] = fn(job)
            job["state"] = "done"
            job["progress"] = 1.0
        except Exception as e:
            job["state"] = "error"
            job["error"] = _friendly(e)
            _zydekjs.log(f"{kind} failed: " + "".join(traceback.format_exception(e))[-1500:])

    threading.Thread(target=work, daemon=True).start()
    return job


def _friendly(e):
    msg = str(e)
    msg = re.sub(r"^ERROR: (\[[^\]]+\] )?", "", msg)
    if "Sign in to confirm" in msg:
        return "YouTube wants a sign-in for this video; try another one"
    return msg.strip()[:300] or e.__class__.__name__


def _ydl(extra=None):
    import yt_dlp
    opts = {
        "quiet": True,
        "no_warnings": False,
        "noprogress": True,
        "logger": _Log(),
        "cachedir": os.path.join(_dirs["cache"], "yt-dlp"),
        "fixup": "never",          # no ffmpeg on the phone; Mixxx reads DASH m4a as it is
        "socket_timeout": 20,
    }
    opts.update(extra or {})
    return yt_dlp.YoutubeDL(opts)


# ---- search ---------------------------------------------------------------------------------------

def _target(query):
    """What to look up: a Bilibili video for a BV code, a pasted link as it is, else a YouTube search."""
    q = query.strip()
    if m := _URL.search(q):
        return m.group(0), "link"
    if m := _BV.search(q):
        return f"https://www.bilibili.com/video/{m.group(1)}", "link"
    return f"ytsearch{SEARCH_RESULTS}:{q}", "search"


def _entry(e, site_hint=""):
    site = (e.get("ie_key") or e.get("extractor_key") or site_hint or "").lower()
    url = e.get("webpage_url") or e.get("url") or ""
    if "youtube" in site and e.get("id") and not url.startswith("http"):
        url = f"https://www.youtube.com/watch?v={e['id']}"
    thumbs = e.get("thumbnails") or []
    thumb = e.get("thumbnail") or (thumbs[-1]["url"] if thumbs else "")
    if not thumb and "youtube" in site and e.get("id"):
        thumb = f"https://i.ytimg.com/vi/{e['id']}/hqdefault.jpg"
    return {
        "id": e.get("id"),
        "title": e.get("title") or "",
        "channel": e.get("channel") or e.get("uploader") or "",
        "duration": e.get("duration"),
        "views": e.get("view_count"),
        "thumbnail": thumb,
        "url": url,
        "site": "bilibili" if "bili" in site or "bilibili" in url else "youtube" if "youtube" in site else site,
    }


def _search(query):
    target, kind = _target(query)

    def run(job):
        with _ydl({"extract_flat": "in_playlist", "skip_download": True}) as ydl:
            info = ydl.extract_info(target, download=False)
        entries = info.get("entries") if info.get("entries") is not None else [info]
        hint = info.get("extractor_key") or ""
        return [_entry(e, hint) for e in entries if e and (e.get("url") or e.get("webpage_url") or e.get("id"))]

    return _job("search", query, run)


# ---- download -------------------------------------------------------------------------------------

def guess_artist_title(info):
    artist = info.get("artist") or (info.get("artists") or [None])[0]
    track = info.get("track")
    if artist and track:
        return artist, track
    title = _JUNK.sub("", info.get("title") or "").strip()
    for sep in (" - ", " – ", " — ", " | "):
        if sep in title:
            a, t = title.split(sep, 1)
            return a.strip(), t.strip()
    channel = re.sub(r"\s*-\s*Topic$|VEVO$", "", info.get("channel") or info.get("uploader") or "").strip()
    return channel, title


def safe_filename(s):
    s = re.sub(r'[\\/:*?"<>|\x00-\x1f]', " ", s)
    return re.sub(r"\s+", " ", s).strip(" .")[:150] or "untitled"


def _tag(path, info, artist, title):
    """Title, artist, link and cover art, so Mixxx's library shows the track properly."""
    try:
        from mutagen.mp4 import MP4, MP4Cover
        audio = MP4(path)
        audio["\xa9nam"] = [title]
        if artist:
            audio["\xa9ART"] = [artist]
        audio["\xa9cmt"] = [info.get("webpage_url") or ""]
        thumb = info.get("thumbnail")
        if thumb:
            req = urllib.request.Request(thumb, headers={"User-Agent": "Mozilla/5.0", "Referer": info.get("webpage_url") or ""})
            data = urllib.request.urlopen(req, timeout=15).read()
            kind = MP4Cover.FORMAT_JPEG if data[:3] == b"\xff\xd8\xff" else MP4Cover.FORMAT_PNG if data[:4] == b"\x89PNG" else None
            if kind is not None:
                audio["covr"] = [MP4Cover(data, imageformat=kind)]
        audio.save()
    except Exception as e:
        _zydekjs.log(f"couldn't tag {path}: {e}")


def _download(url, title):
    def run(job):
        os.makedirs(_dirs["music"], exist_ok=True)

        def hook(d):
            if d["status"] == "downloading":
                total = d.get("total_bytes") or d.get("total_bytes_estimate")
                if total:
                    job["progress"] = min(0.98, d.get("downloaded_bytes", 0) / total)
                job["message"] = "Downloading"

        job["message"] = "Finding the audio"
        with _ydl({
            "format": AUDIO_FORMAT,
            "noplaylist": True,
            "outtmpl": os.path.join(_dirs["music"], "%(title).120B [%(id)s].%(ext)s"),
            "progress_hooks": [hook],
            "overwrites": False,
            "restrictfilenames": False,
        }) as ydl:
            info = ydl.extract_info(url, download=True)
            if info.get("entries"):
                info = next(e for e in info["entries"] if e)
            path = info["requested_downloads"][0]["filepath"]
        artist, track = guess_artist_title(info)
        ext = os.path.splitext(path)[1].lower()
        if ext in (".m4a", ".mp4"):
            job["message"] = "Tagging"
            _tag(path, info, artist, track)
            nice = os.path.join(os.path.dirname(path), safe_filename(f"{artist} - {track}" if artist else track) + ext)
            if not os.path.exists(nice):
                os.replace(path, nice)
                path = nice
        return {"path": path, "artist": artist, "title": track, "duration": info.get("duration"),
                "playable": ext in (".m4a", ".mp4", ".mp3", ".ogg", ".opus", ".flac", ".wav")}

    return _job("download", title or url, run)


# ---- API (/api/web/<name>) ------------------------------------------------------------------------

def _public(job):
    return {k: job[k] for k in ("id", "kind", "title", "state", "progress", "message", "result", "error")}


def api(name, args_json):
    try:
        args = json.loads(args_json or "{}")
        if name == "search":
            if not args.get("q", "").strip():
                return json.dumps({"ok": False, "error": "Type something to search for"})
            return json.dumps({"ok": True, "job": _search(args["q"])["id"]})
        if name == "download":
            if not args.get("url"):
                return json.dumps({"ok": False, "error": "No link to download"})
            return json.dumps({"ok": True, "job": _download(args["url"], args.get("title", ""))["id"]})
        if name == "job":
            job = _jobs.get(args.get("id", ""))
            if not job:
                return json.dumps({"ok": False, "error": "No such job"})
            return json.dumps({"ok": True, **_public(job)})
        if name == "jobs":
            with _lock:
                jobs = [_public(j) for j in sorted(_jobs.values(), key=lambda j: -j["started"]) if j["kind"] == "download"]
            return json.dumps({"ok": True, "jobs": jobs})
        if name == "status":
            return json.dumps({"ok": True, "python": sys.version.split()[0], "folder": _dirs.get("music")})
        return json.dumps({"ok": False, "error": f"Unknown request: {name}"})
    except Exception as e:
        return json.dumps({"ok": False, "error": _friendly(e)})
