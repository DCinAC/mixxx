"""Zydek's Web tab: search YouTube (or open a Bilibili link / BV code) and download the audio, with yt-dlp.

Runs inside Mixxx in an embedded Python (src/zydek/zydekpython.cpp), which calls start() once and then
api(name, args_json) for each /api/web/<name> request from the phone page. Calls return quickly: searches
and downloads are jobs running in their own threads; the page polls /api/web/job?id=.

No stem separation on the phone itself (too slow, too much battery): "stems" sends a track to the Zydek stem
server on a PC (stemserver/ on the build PC, set in Settings) and brings back the .stem.mp4.
"""

import json
import os
import re
import sys
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
import uuid

import _zydekjs

MUSIC_SUBDIR = "Zydek Web"
SEARCH_RESULTS = 20
# Mixxx on Android reads AAC (m4a) but not Opus in WebM, nor Dolby (E-AC-3): ask for AAC first (YouTube's
# itag 140; Bilibili's normal audio).
AUDIO_FORMAT = ("bestaudio[acodec^=mp4a]/bestaudio[ext=m4a][acodec!=ec-3][acodec!=flac]"
                "/bestaudio[acodec!=ec-3][acodec!=flac]/bestaudio[acodec!=ec-3]")
# Bilibili premium accounts get Hi-Res lossless: FLAC in an MP4 container, which Mixxx's FFmpeg reads.
HIRES_FORMAT = "bestaudio[acodec=flac]/" + AUDIO_FORMAT
# Bilibili sessions: written by the app's login (MainActivity.java), read (and refreshed) by yt-dlp.
COOKIE_FILE = "bilibili-cookies.txt"
PREFS_FILE = "zydek-web.json"
DEFAULT_PREFS = {"biliHiRes": True, "stemServer": "", "stemPreset": "best"}
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
        if "JS challenge" in msg:   # shows when YouTube needed QuickJS
            _zydekjs.log("yt-dlp: " + msg)

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
    _dirs.update(data=data_dir, cache=cache_dir, music=_music_folder(music_dir))
    _patch_quickjs()
    # Import yt-dlp (slow the first time: Python compiles it) while nobody waits for it yet.
    threading.Thread(target=lambda: __import__("yt_dlp"), daemon=True).start()


def _music_folder(app_music):
    """The phone's shared Music folder when Zydek may write there (all files access), else the app's own."""
    for base in ("/storage/emulated/0/Music", app_music):
        folder = os.path.join(base, MUSIC_SUBDIR)
        try:
            os.makedirs(folder, exist_ok=True)
            probe = os.path.join(folder, ".zydek-write-test")
            open(probe, "w").close()
            os.remove(probe)
            return folder
        except OSError:
            continue
    return os.path.join(app_music, MUSIC_SUBDIR)


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


def _prefs():
    try:
        with open(os.path.join(_dirs["data"], PREFS_FILE)) as f:
            return {**DEFAULT_PREFS, **json.load(f)}
    except (OSError, ValueError):
        return dict(DEFAULT_PREFS)


def _cookies():
    path = os.path.join(_dirs["data"], COOKIE_FILE)
    return path if os.path.exists(path) else None


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
    if _cookies():
        opts["cookiefile"] = _cookies()
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
        "site": ("bilibili-intl" if "intl" in site or "bilibili.tv" in url else "bilibili") if "bili" in site or "bilibili" in url
                else "youtube" if "youtube" in site else site,
    }


def _bili_parts(url):
    """A bilibili.com video and its parts (P1, P2, ...) from Bilibili's video info API, or None.

    yt-dlp's quick listing gives multi-part videos as bare links (no titles), so ask Bilibili directly."""
    m = _BV.search(url)
    if not m or "bilibili.com" not in url and "b23.tv" not in url:
        return None
    import http.cookiejar
    jar = http.cookiejar.MozillaCookieJar()
    if _cookies():
        jar.load(_cookies(), ignore_discard=True, ignore_expires=True)
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    req = urllib.request.Request(f"https://api.bilibili.com/x/web-interface/view?bvid={m.group(1)}",
                                 headers={"User-Agent": _UA, "Referer": "https://www.bilibili.com/"})
    data = json.load(opener.open(req, timeout=15))
    if data.get("code") != 0:
        return None
    v = data["data"]
    pages = v.get("pages") or []
    owner = (v.get("owner") or {}).get("name", "")
    only = urllib.parse.parse_qs(urllib.parse.urlparse(url).query).get("p", [None])[0]
    base = f"https://www.bilibili.com/video/{v['bvid']}"
    if len(pages) <= 1:
        return [{"id": v["bvid"], "title": v.get("title", ""), "channel": owner, "duration": v.get("duration"),
                 "views": (v.get("stat") or {}).get("view"), "thumbnail": v.get("pic", ""), "url": base, "site": "bilibili"}]
    out = []
    for pg in pages:
        if only and str(pg.get("page")) != only:
            continue
        part = pg.get("part") or f"Part {pg.get('page')}"
        out.append({"id": f"{v['bvid']}_p{pg.get('page')}", "title": f"P{pg.get('page')} · {part}",
                    "channel": f"{owner} · {v.get('title', '')}", "duration": pg.get("duration"),
                    "views": None, "thumbnail": pg.get("first_frame") or v.get("pic", ""),
                    "url": f"{base}?p={pg.get('page')}", "site": "bilibili"})
    return out


def _search(query):
    target, kind = _target(query)

    def run(job):
        if kind == "link":
            try:
                parts = _bili_parts(target)
                if parts:
                    return parts
            except Exception as e:
                _zydekjs.log(f"Bilibili video info failed, using yt-dlp: {e}")
        with _ydl({"extract_flat": "in_playlist", "skip_download": True}) as ydl:
            info = ydl.extract_info(target, download=False)
        entries = info.get("entries") if info.get("entries") is not None else [info]
        hint = info.get("extractor_key") or ""
        out = [_entry(e, hint) for e in entries if e and (e.get("url") or e.get("webpage_url") or e.get("id"))]
        for i, e in enumerate(out):   # bare links (a playlist's parts): at least say which part
            if not e["title"]:
                p = urllib.parse.parse_qs(urllib.parse.urlparse(e["url"]).query).get("p", [None])[0]
                e["title"] = f"{info.get('title') or 'Part'} · P{p or i + 1}"
                e["channel"] = e["channel"] or info.get("uploader") or ""
        return out

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


def _download(url, title, client=None):
    def run(job):
        os.makedirs(_dirs["music"], exist_ok=True)

        def hook(d):
            if d["status"] == "downloading":
                total = d.get("total_bytes") or d.get("total_bytes_estimate")
                if total:
                    job["progress"] = min(0.98, d.get("downloaded_bytes", 0) / total)
                job["message"] = "Downloading"

        job["message"] = "Finding the audio"
        bili = "bilibili" in url or "b23.tv" in url or "bili2233" in url
        with _ydl({
            "format": HIRES_FORMAT if bili and _prefs()["biliHiRes"] else AUDIO_FORMAT,
            "noplaylist": True,
            "outtmpl": os.path.join(_dirs["music"], "%(title).120B [%(id)s].%(ext)s"),
            "progress_hooks": [hook],
            "overwrites": False,
            "restrictfilenames": False,
            # client=web forces YouTube's web player, which always needs the JS challenge (to test QuickJS)
            **({"extractor_args": {"youtube": {"player_client": [client]}}} if client else {}),
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
        fmt = (info.get("requested_downloads") or [{}])[0]
        return {"path": path, "artist": artist, "title": track, "duration": info.get("duration"),
                "codec": fmt.get("acodec") or info.get("acodec"), "kbps": round(fmt.get("tbr") or info.get("tbr") or 0),
                "playable": ext in (".m4a", ".mp4", ".mp3", ".ogg", ".opus", ".flac", ".wav")}

    return _job("download", title or url, run)


# ---- Stems from the PC (stemserver/server.py) ------------------------------------------------------

def _stem_url(server=None):
    server = (server if server is not None else _prefs()["stemServer"]).strip().rstrip("/")
    if not server:
        raise RuntimeError("Set the stem server's address in Settings first")
    if not re.match(r"^https?://", server):
        server = "http://" + server
    if not re.search(r":\d+$", urllib.parse.urlsplit(server).netloc):
        server += ":8770"
    return server


def _stem_request(url, method="GET", data=None, headers=None, timeout=15):
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        return urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        try:
            msg = json.loads(e.read()).get("detail") or e.reason
        except Exception:
            msg = e.reason
        raise RuntimeError(f"Stem server: {msg}") from None
    except (urllib.error.URLError, OSError) as e:
        raise RuntimeError(f"Can't reach the stem server at {url.split('/')[2]} ({getattr(e, 'reason', e)})") from None


def _stem_check(server):
    def run(job):
        with _stem_request(_stem_url(server) + "/health", timeout=5) as r:
            return json.load(r)
    return _job("stemcheck", "Stem server", run)


class _Upload:
    """The track file, read in chunks while urllib sends it, reporting how far it got."""

    def __init__(self, path, job):
        self.f, self.job, self.size, self.sent = open(path, "rb"), job, os.path.getsize(path), 0

    def read(self, n=-1):
        if self.job.get("cancel"):
            raise RuntimeError("Cancelled")
        b = self.f.read(1 << 16 if n is None or n < 0 else min(n, 1 << 16))
        self.sent += len(b)
        self.job["progress"] = 0.15 * self.sent / max(self.size, 1)
        return b


def _stems(path, preset):
    def run(job):
        base = _stem_url()
        if not os.path.exists(path):
            raise RuntimeError("The track's file isn't on this phone")
        job["message"] = "Sending to the PC"
        body = _Upload(path, job)
        try:
            with _stem_request(f"{base}/jobs?" + urllib.parse.urlencode({"name": os.path.basename(path), "preset": preset}),
                               "POST", body, {"Content-Length": str(body.size), "Content-Type": "application/octet-stream"},
                               timeout=120) as r:
                remote = json.load(r)
        finally:
            body.f.close()
        rid = remote["id"]
        try:
            while remote["status"] not in ("done", "error"):
                if job.get("cancel"):
                    raise RuntimeError("Cancelled")
                time.sleep(1)
                with _stem_request(f"{base}/jobs/{rid}") as r:
                    remote = json.load(r)
                st = remote["status"]
                job["message"] = (f"Waiting on the PC ({remote['ahead']} ahead)" if st == "queued" and remote.get("ahead")
                                  else "Waiting on the PC" if st == "queued"
                                  else "Separating on the PC" if st == "separating" else "Writing the stem file")
                job["progress"] = 0.15 + 0.7 * remote.get("progress", 0)
            if remote["status"] == "error":
                raise RuntimeError(remote.get("message") or "The stem server failed")
            job["message"] = "Downloading the stems"
            name = safe_filename(f"{remote['artist']} - {remote['title']}" if remote.get("artist")
                                 else remote.get("title") or os.path.splitext(os.path.basename(path))[0])
            os.makedirs(_dirs["music"], exist_ok=True)
            out = os.path.join(_dirs["music"], name + ".stem.mp4")
            n = 2
            while os.path.exists(out):
                out = os.path.join(_dirs["music"], f"{name} ({n}).stem.mp4")
                n += 1
            with _stem_request(f"{base}/jobs/{rid}/file", timeout=60) as r:
                total, got = int(r.headers.get("Content-Length") or 0), 0
                with open(out + ".part", "wb") as f:
                    while chunk := r.read(1 << 16):
                        f.write(chunk)
                        got += len(chunk)
                        if total:
                            job["progress"] = 0.85 + 0.15 * got / total
            os.replace(out + ".part", out)
            return {"path": out, "title": remote.get("title"), "artist": remote.get("artist"), "seconds": remote.get("seconds")}
        finally:
            try:   # the PC keeps nothing once we have it (or gave up)
                _stem_request(f"{base}/jobs/{rid}", "DELETE").close()
            except Exception:
                pass

    return _job("stems", os.path.basename(path), run)


# ---- Bilibili accounts ----------------------------------------------------------------------------

def _account():
    """Who's logged in where (a job: it asks Bilibili)."""
    def run(job):
        import http.cookiejar
        jar = http.cookiejar.MozillaCookieJar()
        if _cookies():
            jar.load(_cookies(), ignore_discard=True, ignore_expires=True)
        has = {d: any(c.name == "SESSDATA" and c.value and c.domain.endswith(d) for c in jar)
               for d in ("bilibili.com", "bilibili.tv")}
        out = {"cn": {"login": False}, "intl": {"login": has["bilibili.tv"]}, "hiRes": _prefs()["biliHiRes"]}
        if has["bilibili.com"]:
            opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
            req = urllib.request.Request("https://api.bilibili.com/x/web-interface/nav", headers={
                "User-Agent": "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 Chrome/120 Mobile Safari/537.36",
                "Referer": "https://www.bilibili.com/"})
            data = json.load(opener.open(req, timeout=15)).get("data") or {}
            vip = (data.get("vip") or {}).get("label", {}).get("text") or ""
            out["cn"] = {"login": bool(data.get("isLogin")), "name": data.get("uname") or "",
                         "premium": data.get("vipStatus") == 1, "vip": vip,
                         "expired": not data.get("isLogin")}   # cookie there, but Bilibili no longer accepts it
        return out
    return _job("account", "Bilibili", run)


# ---- Bilibili QR login -----------------------------------------------------------------------------
# Zydek's own login screen (phone page): ask the site for a QR login, show the code (and its link, which
# opens in the Bilibili app on this phone), wait for the confirmation, then keep the session cookies in the
# cookies.txt yt-dlp reads, as the app's web login does (MainActivity.java).

_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0.0.0 Safari/537.36"
_SITES = {
    "cn": {"domain": "bilibili.com", "referer": "https://www.bilibili.com/"},
    "intl": {"domain": "bilibili.tv", "referer": "https://www.bilibili.tv/"},
}
QR_LIFETIME = 170   # Bilibili's codes last 180 s
SESSION_LIFETIME = 180 * 24 * 3600


def _qr_login(site):
    def run(job):
        import http.cookiejar
        import segno
        jar = http.cookiejar.CookieJar()
        opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
        headers = {"User-Agent": _UA, "Referer": _SITES[site]["referer"]}

        def get(url):
            return json.load(opener.open(urllib.request.Request(url, headers=headers), timeout=15))

        def visit(url):   # a link that sets the session cookies
            opener.open(urllib.request.Request(url, headers=headers), timeout=15).read()

        if site == "cn":
            g = get("https://passport.bilibili.com/x/passport-login/web/qrcode/generate?source=main-fe-header")
            if g.get("code") != 0:
                raise RuntimeError(f"Bilibili gave no login code ({g.get('message')})")
            link, key = g["data"]["url"], g["data"]["qrcode_key"]
        else:
            base = "https://passport.bilibili.tv/x/intl/passport-login/qrcode/auth/"
            g = get(base + "url?s_locale=en_US&platform=web")
            if g.get("code") != 0:
                raise RuntimeError(f"bilibili.tv gave no login code ({g.get('message')})")
            link = g["data"]["qr_url"]
            key = urllib.parse.parse_qs(urllib.parse.urlparse(link).query)["ticket"][0]
        job["data"] = {"site": site, "link": link,
                       "svg": segno.make(link, error="m").svg_inline(scale=6, border=2, dark="#000", light="#fff", omitsize=True)}
        job["message"] = "waiting"
        deadline = time.time() + QR_LIFETIME
        while time.time() < deadline:
            time.sleep(2)
            if job.get("cancel"):
                job["message"] = "cancelled"
                return None
            if site == "cn":
                p = get("https://passport.bilibili.com/x/passport-login/web/qrcode/poll?source=main-fe-header&qrcode_key=" + key)
                code = (p.get("data") or {}).get("code", p.get("code"))
                if code == 86090:
                    job["message"] = "scanned"
                elif code == 86038:
                    break
                elif code == 0:
                    if p["data"].get("url"):
                        visit(p["data"]["url"])
                    return _keep_session(site, jar)
            else:
                p = get(base + "fetch?s_locale=en_US&platform=web&ticket=" + key)
                if p.get("code") == 0:
                    url = (p.get("data") or {}).get("redirect_url") or (p.get("data") or {}).get("url")
                    if url:
                        visit(url)
                    return _keep_session(site, jar)
                if p.get("code") == 10018100:
                    break
        job["message"] = "expired"
        raise RuntimeError("The code ran out: tap it for a new one")

    return _job("qrlogin", site, run)


def _keep_session(site, jar):
    import http.cookiejar
    domain = _SITES[site]["domain"]
    mine = [c for c in jar if c.domain.lstrip(".").endswith(domain)]
    if not any(c.name == "SESSDATA" and c.value for c in mine):
        raise RuntimeError("Bilibili confirmed the login but sent no session; try the login page instead")
    path = os.path.join(_dirs["data"], COOKIE_FILE)
    keep = http.cookiejar.MozillaCookieJar(path)
    if os.path.exists(path):
        keep.load(ignore_discard=True, ignore_expires=True)
    for c in list(keep):
        if c.domain.lstrip(".").endswith(domain):
            keep.clear(c.domain, c.path, c.name)
    for c in mine:
        if c.expires is None:
            c.expires, c.discard = int(time.time()) + SESSION_LIFETIME, False
        keep.set_cookie(c)
    keep.save(ignore_discard=True, ignore_expires=True)
    return {"site": site}


def _qr_save(job_id):
    """The job's QR code as a picture in Pictures/Zydek, for the Bilibili app's scan-from-photos."""
    import segno
    job = _jobs.get(job_id)
    if not job or not job.get("data"):
        raise RuntimeError("No QR code to save")
    folder = "/storage/emulated/0/Pictures/Zydek"
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, f"bilibili-login-{job['data']['site']}.png")
    segno.make(job["data"]["link"], error="m").save(path, kind="png", scale=12, border=4, dark="#000", light="#fff")
    return path


# ---- API (/api/web/<name>) ------------------------------------------------------------------------

def _public(job):
    return {k: job.get(k) for k in ("id", "kind", "title", "state", "progress", "message", "result", "error", "data")}


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
            return json.dumps({"ok": True, "job": _download(args["url"], args.get("title", ""), args.get("client"))["id"]})
        if name == "job":
            job = _jobs.get(args.get("id", ""))
            if not job:
                return json.dumps({"ok": False, "error": "No such job"})
            return json.dumps({"ok": True, **_public(job)})
        if name == "jobs":
            with _lock:
                jobs = [_public(j) for j in sorted(_jobs.values(), key=lambda j: -j["started"]) if j["kind"] == "download"]
            return json.dumps({"ok": True, "jobs": jobs})
        if name == "qr":        # ?text=  -> {svg}: any QR code the page wants to show
            import segno
            svg = segno.make(args.get("text", ""), error="m").svg_inline(scale=6, border=2, dark="#000", light="#fff", omitsize=True)
            return json.dumps({"ok": True, "svg": svg})
        if name == "qrlogin":   # ?site=cn|intl
            if args.get("site") not in _SITES:
                return json.dumps({"ok": False, "error": "Unknown site"})
            return json.dumps({"ok": True, "job": _qr_login(args["site"])["id"]})
        if name == "qrsave":    # ?id=<qrlogin job>
            return json.dumps({"ok": True, "path": _qr_save(args.get("id", ""))})
        if name == "cancel":    # ?id=
            if args.get("id") in _jobs:
                _jobs[args["id"]]["cancel"] = True
            return json.dumps({"ok": True})
        if name == "stems":     # ?path=  (with the model chosen in Settings, or ?preset=)
            if not args.get("path"):
                return json.dumps({"ok": False, "error": "No track"})
            _stem_url()   # no server set: say so now
            return json.dumps({"ok": True, "job": _stems(args["path"], args.get("preset") or _prefs()["stemPreset"])["id"]})
        if name == "stemcheck":   # ?server=  (what's typed in Settings; empty = the saved one)
            return json.dumps({"ok": True, "job": _stem_check(args.get("server"))["id"]})
        if name == "account":
            return json.dumps({"ok": True, "job": _account()["id"]})
        if name == "prefs":   # ?biliHiRes=0|1, ?stemServer=host:port, ?stemPreset= to change, nothing to read
            prefs = _prefs()
            if args.keys() & {"biliHiRes", "stemServer", "stemPreset"}:
                if "stemPreset" in args:
                    prefs["stemPreset"] = args["stemPreset"]
                if "biliHiRes" in args:
                    prefs["biliHiRes"] = args["biliHiRes"] not in ("0", "false", "")
                if "stemServer" in args:
                    prefs["stemServer"] = args["stemServer"].strip()
                with open(os.path.join(_dirs["data"], PREFS_FILE), "w") as f:
                    json.dump(prefs, f)
            return json.dumps({"ok": True, **prefs})
        if name == "status":
            js = _zydekjs.run("console.log([6 * 7, typeof BigInt, /\\p{L}+/u.test('音楽')].join())").strip()
            import yt_dlp.version
            return json.dumps({"ok": True, "python": sys.version.split()[0], "ytdlp": yt_dlp.version.__version__,
                               "quickjs": js, "folder": _dirs.get("music")})
        return json.dumps({"ok": False, "error": f"Unknown request: {name}"})
    except Exception as e:
        return json.dumps({"ok": False, "error": _friendly(e)})
