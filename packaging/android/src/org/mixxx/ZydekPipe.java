package org.mixxx;

import android.net.Uri;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.schabi.newpipe.extractor.Image;
import org.schabi.newpipe.extractor.InfoItem;
import org.schabi.newpipe.extractor.ListExtractor;
import org.schabi.newpipe.extractor.MediaFormat;
import org.schabi.newpipe.extractor.NewPipe;
import org.schabi.newpipe.extractor.Page;
import org.schabi.newpipe.extractor.ServiceList;
import org.schabi.newpipe.extractor.StreamingService;
import org.schabi.newpipe.extractor.downloader.Downloader;
import org.schabi.newpipe.extractor.downloader.Request;
import org.schabi.newpipe.extractor.downloader.Response;
import org.schabi.newpipe.extractor.exceptions.ReCaptchaException;
import org.schabi.newpipe.extractor.playlist.PlaylistInfo;
import org.schabi.newpipe.extractor.search.SearchInfo;
import org.schabi.newpipe.extractor.stream.AudioStream;
import org.schabi.newpipe.extractor.stream.DeliveryMethod;
import org.schabi.newpipe.extractor.stream.StreamInfo;
import org.schabi.newpipe.extractor.stream.StreamInfoItem;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/// ZyDeck's Web tab without Python: search YouTube (and SoundCloud, Bandcamp, PeerTube links) and download the
/// audio with NewPipe's extractor (TeamNewPipe/NewPipeExtractor), in the app's own process. Bilibili (a BV or av
/// code, a bilibili.com or b23.tv link) stays with yt-dlp in res/zydek/python/zydek_web.py: the hub
/// (src/zydek/zydekhub.cpp) sends each request to one or the other.
///
/// The same requests and answers as zydek_web.py's api() (/api/web/search, download, job, jobs, cancel), so the
/// pages don't know which one answered; its job ids start "np" (the hub sends job and cancel by that). Tags and
/// cover art are written by the hub (nativeTag: TagLib, as Mixxx reads them).
public final class ZydekPipe {
    private static final String TAG = "ZydekPipe";
    // NewPipe's own (its DownloaderImpl): YouTube answers it as it answers NewPipe
    static final String USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:153.0) Gecko/20100101 Firefox/153.0";
    // as zydek_web.py
    static final int SEARCH_RESULTS = 20;
    static final int LIST_MAX = 500;          // a playlist's videos, at most
    static final int MIX_MAX = 50;            // YouTube's mixes (list=RD...) go on and on: their first 50
    static final String MUSIC_SUBDIR = "Zydek Web";
    static final int BLOCK = 512 * 1024;      // a download's pieces (as NewPipe: YouTube slows down whole-file requests)
    static final int PARALLEL = 4;            // pieces fetched at once (a 5 MB track: about a second on Wi-Fi)
    static final int PREVIEWS_KEPT = 6;       // previews kept in the cache (a download of one of them: no fetch)
    static final Pattern URL_RE = Pattern.compile("https?://\\S+");
    static final Pattern JUNK = Pattern.compile(
            "\\s*[\\(\\[][^\\)\\]]*\\b(official|video|audio|lyrics?|visuali[sz]er|hd|hq|4k|mv|m/v|clip)\\b[^\\)\\]]*[\\)\\]]",
            Pattern.CASE_INSENSITIVE);
    static final Pattern RESERVED = Pattern.compile("^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\\..*)?$", Pattern.CASE_INSENSITIVE);

    /// TagLib, in the hub: title, artist, the link as the comment, cover art (JPEG or PNG; null: none)
    static native boolean nativeTag(String path, String title, String artist, String comment, byte[] cover);

    private ZydekPipe() {
    }

    // ---- NewPipe's network: HttpURLConnection (no OkHttp to bundle) ---------------------------------------

    static final class Net extends Downloader {
        @Override
        public Response execute(Request request) throws IOException, ReCaptchaException {
            final HttpURLConnection c = (HttpURLConnection) new URL(request.url()).openConnection();
            c.setConnectTimeout(20000);
            c.setReadTimeout(20000);
            c.setInstanceFollowRedirects(true);
            c.setRequestMethod(request.httpMethod());
            c.setRequestProperty("User-Agent", USER_AGENT);
            for (Map.Entry<String, List<String>> h : request.headers().entrySet()) {
                if (h.getValue().isEmpty()) {
                    continue;
                }
                c.setRequestProperty(h.getKey(), h.getValue().get(0));
                for (int i = 1; i < h.getValue().size(); i++) {
                    c.addRequestProperty(h.getKey(), h.getValue().get(i));
                }
            }
            final byte[] data = request.dataToSend();
            if (data != null) {
                c.setDoOutput(true);
                try (OutputStream out = c.getOutputStream()) {
                    out.write(data);
                }
            }
            final int code = c.getResponseCode();
            if (code == 429) {
                c.disconnect();
                throw new ReCaptchaException("reCaptcha Challenge requested", request.url());
            }
            String body = null;
            if (!"HEAD".equals(request.httpMethod())) {
                InputStream in = null;
                try {
                    in = code >= 400 ? c.getErrorStream() : c.getInputStream();
                    if (in != null) {
                        body = new String(readAll(in), StandardCharsets.UTF_8);
                    }
                } finally {
                    if (in != null) {
                        in.close();
                    }
                }
            }
            final Map<String, List<String>> headers = new LinkedHashMap<>();
            for (Map.Entry<String, List<String>> h : c.getHeaderFields().entrySet()) {
                if (h.getKey() != null) {
                    headers.put(h.getKey(), h.getValue());
                }
            }
            final String latest = c.getURL().toString();
            c.disconnect();
            return new Response(code, c.getResponseMessage(), headers, body, latest);
        }
    }

    private static boolean s_ready;

    private static synchronized void init() {
        if (!s_ready) {
            NewPipe.init(new Net());
            s_ready = true;
        }
    }

    static byte[] readAll(InputStream in) throws IOException {
        final ByteArrayOutputStream out = new ByteArrayOutputStream();
        final byte[] buf = new byte[16384];
        for (int n; (n = in.read(buf)) > 0;) {
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    // ---- jobs (as zydek_web.py's _job) ------------------------------------------------------------------

    interface Work {
        Object run(Job job) throws Exception;
    }

    static final class Job {
        final String id = "np" + UUID.randomUUID().toString().replace("-", "").substring(0, 10);
        final String kind;
        final String title;
        final long started = System.currentTimeMillis();
        volatile String state = "running";
        volatile double progress;
        volatile String message = "";
        volatile Object result;
        volatile String error;
        volatile double speed;
        volatile boolean cancel;

        Job(String kind, String title) {
            this.kind = kind;
            this.title = title;
        }

        JSONObject toJson() throws JSONException {
            final JSONObject o = new JSONObject();
            o.put("id", id).put("kind", kind).put("title", title).put("state", state).put("progress", progress)
                    .put("message", message).put("result", result == null ? JSONObject.NULL : result)
                    .put("error", error == null ? JSONObject.NULL : error).put("data", JSONObject.NULL).put("speed", speed);
            return o;
        }
    }

    private static final Map<String, Job> s_jobs = new LinkedHashMap<>();
    private static final ExecutorService s_pool = Executors.newCachedThreadPool(r -> {
        final Thread t = new Thread(r, "ZydekPipe");
        t.setDaemon(true);
        return t;
    });

    static Job job(String kind, String title, Work work) {
        final Job job = new Job(kind, title);
        synchronized (s_jobs) {
            s_jobs.put(job.id, job);
            // the last 40 (the finished ones beyond that go)
            final List<Job> all = new ArrayList<>(s_jobs.values());
            for (int i = 0; i < all.size() - 40; i++) {
                if (!"running".equals(all.get(i).state)) {
                    s_jobs.remove(all.get(i).id);
                }
            }
        }
        s_pool.execute(() -> {
            try {
                job.result = work.run(job);
                job.state = "done";
                job.progress = 1.0;
            } catch (Throwable e) {
                job.state = "error";
                job.error = friendly(e);
                Log.w(TAG, job.kind + " failed", e);
            }
        });
        return job;
    }

    static String friendly(Throwable e) {
        String msg = e.getMessage();
        if (e instanceof ReCaptchaException) {
            return "YouTube wants to check this phone isn't a robot: try again in a while";
        }
        if (msg == null || msg.isEmpty()) {
            msg = e.getClass().getSimpleName();
        }
        if (msg.contains("Sign in to confirm")) {
            return "YouTube wants a sign-in for this video; try another one";
        }
        return msg.length() > 300 ? msg.substring(0, 300) : msg;
    }

    // ---- search ---------------------------------------------------------------------------------------

    static String siteOf(StreamingService service) {
        return service.getServiceInfo().getName().toLowerCase(java.util.Locale.ROOT);
    }

    static String bestImage(List<Image> images) {
        String url = "";
        int best = -2;
        for (Image i : images) {
            if (i.getHeight() > best) {
                best = i.getHeight();
                url = i.getUrl();
            }
        }
        return url;
    }

    static String idOf(StreamingService service, String url) {
        try {
            return service.getStreamLHFactory().getId(url);
        } catch (Exception e) {
            return null;
        }
    }

    static Object nullable(long v) {
        return v < 0 ? JSONObject.NULL : (Object) v;
    }

    static JSONObject entry(StreamingService service, StreamInfoItem it) throws JSONException {
        final String id = idOf(service, it.getUrl());
        return new JSONObject().put("id", id == null ? JSONObject.NULL : id).put("title", it.getName())
                .put("channel", it.getUploaderName() == null ? "" : it.getUploaderName())
                .put("duration", nullable(it.getDuration())).put("views", nullable(it.getViewCount()))
                .put("thumbnail", bestImage(it.getThumbnails())).put("url", it.getUrl()).put("site", siteOf(service));
    }

    /// A shared link without what it says about the sharing, and what else to offer (as zydek_web.py's _clean_link):
    /// YouTube's (youtu.be, shorts, live, music.youtube.com) are youtube.com/watch?v=..., one in a playlist (&list=)
    /// is the video with the playlist offered (whole: the playlist itself)
    static String[] cleanLink(String url, boolean whole) {
        final Uri u = Uri.parse(url);
        final String host = u.getHost() == null ? "" : u.getHost().toLowerCase(java.util.Locale.ROOT);
        if (!host.equals("youtu.be") && !host.endsWith("youtube.com")) {
            return new String[] {url, null, null};
        }
        final List<String> path = u.getPathSegments();
        String vid = host.equals("youtu.be") ? (path.isEmpty() ? null : path.get(0))
                : path.size() > 1 && (path.get(0).equals("shorts") || path.get(0).equals("live") || path.get(0).equals("embed"))
                ? path.get(1) : u.getQueryParameter("v");
        final String list = u.getQueryParameter("list");
        if (vid != null && list != null && whole) {
            return new String[] {"https://www.youtube.com/watch?v=" + vid + "&list=" + list, null, "list"};
        }
        if (vid != null) {
            return new String[] {"https://www.youtube.com/watch?v=" + vid, list, null};
        }
        if (list != null) {
            return new String[] {"https://www.youtube.com/playlist?list=" + list, null, "list"};
        }
        return new String[] {url, null, null};
    }

    static Job search(String query, boolean whole) {
        return job("search", query, job -> {
            init();
            final JSONArray out = new JSONArray();
            final Matcher m = URL_RE.matcher(query.trim());
            if (!m.find()) {   // words: YouTube's videos
                final StreamingService yt = ServiceList.YouTube;
                final SearchInfo info = SearchInfo.getInfo(yt, yt.getSearchQHFactory().fromQuery(query.trim(),
                        Collections.singletonList("videos"), ""));
                for (InfoItem it : info.getRelatedItems()) {
                    if (it instanceof StreamInfoItem) {
                        out.put(entry(yt, (StreamInfoItem) it));
                        if (out.length() >= SEARCH_RESULTS) {
                            break;
                        }
                    }
                }
                return out;
            }
            final String given = m.group();
            final String[] clean = cleanLink(given, whole);
            final String url = clean[0];
            if (!url.equals(given)) {   // (the page shows the link as it's looked up)
                out.put(new JSONObject().put("kind", "link").put("url", url));
            }
            if (clean[1] != null) {     // a video in a playlist or a mix: that list on offer
                final String vid = Uri.parse(url).getQueryParameter("v");
                out.put(new JSONObject().put("kind", "list").put("what", clean[1].startsWith("RD") ? "mix" : "playlist")
                        .put("site", "youtube").put("url", "https://www.youtube.com/watch?v=" + vid + "&list=" + clean[1])
                        .put("whole", true));
            }
            final StreamingService service;
            try {
                service = NewPipe.getServiceByUrl(url);
            } catch (Exception e) {
                throw new IOException("ZyDeck can't open that link (YouTube, SoundCloud, Bandcamp and PeerTube, or Bilibili)");
            }
            final StreamingService.LinkType type = "list".equals(clean[2]) ? StreamingService.LinkType.PLAYLIST
                    : service.getLinkTypeByUrl(url);
            if (type == StreamingService.LinkType.STREAM) {
                final StreamInfo s = StreamInfo.getInfo(service, url);
                final String id = idOf(service, url);
                out.put(new JSONObject().put("id", id == null ? JSONObject.NULL : id).put("title", s.getName())
                        .put("channel", s.getUploaderName() == null ? "" : s.getUploaderName())
                        .put("duration", nullable(s.getDuration())).put("views", nullable(s.getViewCount()))
                        .put("thumbnail", bestImage(s.getThumbnails())).put("url", s.getUrl()).put("site", siteOf(service)));
                return out;
            }
            if (type != StreamingService.LinkType.PLAYLIST) {
                throw new IOException("Paste a video's or a playlist's link");
            }
            final boolean mix = url.contains("list=RD");
            final int max = mix ? MIX_MAX : LIST_MAX;
            final PlaylistInfo list = PlaylistInfo.getInfo(service, url);
            out.put(new JSONObject().put("kind", "name").put("title", list.getName()));
            int n = 0;
            List<StreamInfoItem> items = list.getRelatedItems();
            Page next = list.hasNextPage() ? list.getNextPage() : null;
            while (true) {
                for (StreamInfoItem it : items) {
                    if (n++ >= max) {
                        return out;
                    }
                    out.put(entry(service, it));
                }
                if (next == null || job.cancel) {
                    return out;
                }
                job.message = n + " found";
                final ListExtractor.InfoItemsPage<StreamInfoItem> more = PlaylistInfo.getMoreItems(service, url, next);
                items = more.getItems();
                next = more.hasNextPage() ? more.getNextPage() : null;
            }
        });
    }

    // ---- download -------------------------------------------------------------------------------------

    /// Mixxx on Android reads AAC (m4a) but not Opus in WebM: the best AAC there is in one piece, in the video's
    /// own language (not a dub); MP3 where that's all (SoundCloud)
    static AudioStream pick(List<AudioStream> streams) {
        AudioStream best = null;
        int bestScore = Integer.MIN_VALUE;
        for (AudioStream a : streams) {
            if (!a.isUrl() || a.getDeliveryMethod() != DeliveryMethod.PROGRESSIVE_HTTP) {
                continue;
            }
            final MediaFormat f = a.getFormat();
            final int kind = f == MediaFormat.M4A ? 2 : f == MediaFormat.MP3 ? 1 : -1;
            if (kind < 0) {
                continue;
            }
            final Object track = a.getAudioTrackType();
            final boolean original = track == null || "ORIGINAL".equals(track.toString());
            final int score = (original ? 1_000_000 : 0) + kind * 100_000 + Math.max(0, a.getAverageBitrate());
            if (score > bestScore) {
                bestScore = score;
                best = a;
            }
        }
        return best;
    }

    static String[] guessArtistTitle(String name, String uploader) {
        final String title = JUNK.matcher(name == null ? "" : name).replaceAll("").trim();
        for (String sep : new String[] {" - ", " – ", " — ", " | "}) {
            final int i = title.indexOf(sep);
            if (i >= 0) {
                return new String[] {title.substring(0, i).trim(), title.substring(i + sep.length()).trim()};
            }
        }
        final String channel = (uploader == null ? "" : uploader).replaceAll("\\s*-\\s*Topic$|VEVO$", "").trim();
        return new String[] {channel, title};
    }

    /// The same as zydek_web.py's safe_filename and the C++ one (src/zydek/zydeckshared.h safeFileName); Java
    /// counts characters as the C++ does (UTF-16)
    static String safeFileName(String s) {
        s = s.replaceAll("[\\\\/:*?\"<>|\\x00-\\x1f]", " ");
        s = strip(s.trim().replaceAll("\\s+", " "));
        if (s.length() > 150) {
            s = strip(s.substring(0, 150));
        }
        if (RESERVED.matcher(s).matches()) {
            s = "_" + s;
        }
        return s.isEmpty() ? "untitled" : s;
    }

    static String strip(String s) {   // (Python's strip(" ."))
        int a = 0, b = s.length();
        while (a < b && (s.charAt(a) == ' ' || s.charAt(a) == '.')) {
            a++;
        }
        while (b > a && (s.charAt(b - 1) == ' ' || s.charAt(b - 1) == '.')) {
            b--;
        }
        return s.substring(a, b);
    }

    /// The phone's shared Music folder when ZyDeck may write there (all files access), else the app's own
    /// (zydek_web.py's _music_folder: the same choice)
    static File musicFolder(String appMusic) {
        for (String base : new String[] {"/storage/emulated/0/Music", appMusic}) {
            if (base == null || base.isEmpty()) {
                continue;
            }
            final File folder = new File(base, MUSIC_SUBDIR);
            //noinspection ResultOfMethodCallIgnored
            folder.mkdirs();
            final File probe = new File(folder, ".zydek-write-test");
            try {
                new FileOutputStream(probe).close();
                //noinspection ResultOfMethodCallIgnored
                probe.delete();
                return folder;
            } catch (IOException e) {
                // the next
            }
        }
        return new File(appMusic, MUSIC_SUBDIR);
    }

    static Job download(String url, String title, String appMusic, String cacheDir) {
        return job("download", title == null || title.isEmpty() ? url : title, job -> {
            init();
            job.message = "Finding the audio";
            final StreamingService service = NewPipe.getServiceByUrl(url);
            final StreamInfo info = StreamInfo.getInfo(service, url);
            final AudioStream a = pick(info.getAudioStreams());
            if (a == null) {
                throw new IOException("No audio Mixxx can read (AAC or MP3) for this one");
            }
            final String ext = a.getFormat().getSuffix();
            final String[] at = guessArtistTitle(info.getName(), info.getUploaderName());
            final File folder = musicFolder(appMusic);
            File file = new File(folder, safeFileName(at[0].isEmpty() ? at[1] : at[0] + " - " + at[1]) + "." + ext);
            if (file.exists()) {   // (that name taken: the title and its id, as yt-dlp names them)
                final String id = idOf(service, url);
                file = new File(folder, safeFileName(info.getName() + (id == null ? "" : " [" + id + "]")) + "." + ext);
            }
            if (!file.exists()) {
                final File cached = previewFile(cacheDir, service, url, ext);
                if (cached != null && cached.length() > 0) {   // previewed: the same audio, no need to fetch it again
                    job.message = "Downloading";
                    copy(cached, file);
                } else {
                    fetch(job, a.getContent(), file, 0.98);
                }
                job.message = "Tagging";
                byte[] cover = null;
                try {
                    cover = coverArt(service, url, info);
                } catch (Exception e) {
                    Log.w(TAG, "no cover art: " + e);
                }
                try {
                    nativeTag(file.getPath(), at[1], at[0], info.getUrl(), cover);
                } catch (UnsatisfiedLinkError e) {
                    Log.w(TAG, "no tagging: " + e);
                }
            }
            final String codec = a.getCodec();
            return new JSONObject().put("path", file.getPath()).put("artist", at[0]).put("title", at[1])
                    .put("duration", nullable(info.getDuration())).put("codec", codec == null ? ext : codec)
                    .put("kbps", Math.max(0, a.getAverageBitrate())).put("playable", true);
        });
    }

    // ---- preview: the whole audio, quickly, for the preview deck (the headphones) ----------------------

    /// Where a link's preview is kept (the app's cache: zydeck-preview/<site>-<id>.<ext>); null without an id
    static File previewFile(String cacheDir, StreamingService service, String url, String ext) {
        final String id = idOf(service, url);
        if (cacheDir == null || cacheDir.isEmpty() || id == null) {
            return null;
        }
        return new File(new File(cacheDir, "zydeck-preview"), siteOf(service) + "-" + safeFileName(id) + "." + ext);
    }

    static Job preview(String url, String title, String cacheDir) {
        return job("preview", title == null || title.isEmpty() ? url : title, job -> {
            init();
            job.message = "Finding the audio";
            final StreamingService service;
            try {
                service = NewPipe.getServiceByUrl(url);
            } catch (Exception e) {
                throw new IOException("Preview works for YouTube, SoundCloud, Bandcamp and PeerTube links");
            }
            for (String ext : new String[] {"m4a", "mp3"}) {   // previewed before: straight from the cache
                final File kept = previewFile(cacheDir, service, url, ext);
                if (kept != null && kept.length() > 0) {
                    //noinspection ResultOfMethodCallIgnored
                    kept.setLastModified(System.currentTimeMillis());
                    return new JSONObject().put("path", kept.getPath()).put("cached", true);
                }
            }
            final StreamInfo info = StreamInfo.getInfo(service, url);
            final AudioStream a = pick(info.getAudioStreams());
            if (a == null) {
                throw new IOException("No audio Mixxx can read (AAC or MP3) for this one");
            }
            final File file = previewFile(cacheDir, service, url, a.getFormat().getSuffix());
            if (file == null) {
                throw new IOException("Nowhere to keep the preview");
            }
            //noinspection ResultOfMethodCallIgnored
            file.getParentFile().mkdirs();
            if (!file.exists() || file.length() == 0) {
                fetch(job, a.getContent(), file, 1.0);
            }
            //noinspection ResultOfMethodCallIgnored
            file.setLastModified(System.currentTimeMillis());
            trimPreviews(file.getParentFile());
            final String[] at = guessArtistTitle(info.getName(), info.getUploaderName());
            return new JSONObject().put("path", file.getPath()).put("artist", at[0]).put("title", at[1])
                    .put("duration", nullable(info.getDuration()));
        });
    }

    /// The newest PREVIEWS_KEPT previews stay (the hub takes the older ones out of Mixxx's library)
    static void trimPreviews(File dir) {
        final File[] all = dir.listFiles();
        if (all == null || all.length <= PREVIEWS_KEPT) {
            return;
        }
        java.util.Arrays.sort(all, (x, y) -> Long.compare(y.lastModified(), x.lastModified()));
        for (int i = PREVIEWS_KEPT; i < all.length; i++) {
            //noinspection ResultOfMethodCallIgnored
            all[i].delete();
        }
    }

    static void copy(File from, File to) throws IOException {
        final File part = new File(to.getPath() + ".part");
        try (InputStream in = new java.io.FileInputStream(from); OutputStream out = new FileOutputStream(part)) {
            final byte[] buf = new byte[65536];
            for (int n; (n = in.read(buf)) > 0;) {
                out.write(buf, 0, n);
            }
        }
        if (!part.renameTo(to)) {
            //noinspection ResultOfMethodCallIgnored
            part.delete();
            throw new IOException("Couldn't save " + to.getName());
        }
    }

    /// The audio in pieces (Range), PARALLEL of them at once, into a .part file renamed when it's all there;
    /// progress up to span. (One piece first: it says how big the whole is.)
    static void fetch(Job job, String url, File to, double span) throws IOException {
        final File part = new File(to.getPath() + ".part");
        job.message = "Downloading";
        final long[] total = {-1};
        final java.util.concurrent.atomic.AtomicLong done = new java.util.concurrent.atomic.AtomicLong();
        final long[] tick = {System.nanoTime(), 0};
        final Runnable progress = () -> {
            if (total[0] > 0) {
                job.progress = Math.min(span, span * done.get() / total[0]);
            }
            synchronized (tick) {
                final long now = System.nanoTime();
                if (now - tick[0] > 500_000_000L) {   // bytes a second, over the last half second
                    job.speed = (done.get() - tick[1]) / ((now - tick[0]) / 1e9);
                    tick[0] = now;
                    tick[1] = done.get();
                }
            }
        };
        try (java.io.RandomAccessFile out = new java.io.RandomAccessFile(part, "rw")) {
            out.setLength(0);
            final boolean whole = piece(job, url, out, 0, total, done, progress);
            if (!whole && total[0] > BLOCK) {
                final long pieces = (total[0] + BLOCK - 1) / BLOCK;
                final java.util.concurrent.atomic.AtomicLong next = new java.util.concurrent.atomic.AtomicLong(1);
                final IOException[] failed = {null};
                final List<Thread> workers = new ArrayList<>();
                for (int w = 0; w < PARALLEL; w++) {
                    final Thread t = new Thread(() -> {
                        try {
                            for (long k; (k = next.getAndIncrement()) < pieces && failed[0] == null;) {
                                piece(job, url, out, k * BLOCK, total, done, progress);
                            }
                        } catch (IOException e) {
                            synchronized (failed) {
                                if (failed[0] == null) {
                                    failed[0] = e;
                                }
                            }
                        }
                    }, "ZydekPipe-fetch");
                    t.setDaemon(true);
                    t.start();
                    workers.add(t);
                }
                for (Thread t : workers) {
                    try {
                        t.join();
                    } catch (InterruptedException e) {
                        throw new IOException("Cancelled");
                    }
                }
                if (failed[0] != null) {
                    throw failed[0];
                }
            }
            if (total[0] > 0 && out.length() < total[0]) {
                throw new IOException("The download stopped short (" + out.length() + " of " + total[0] + " bytes)");
            }
        } catch (IOException e) {
            //noinspection ResultOfMethodCallIgnored
            part.delete();
            throw e;
        }
        job.speed = 0;
        if (!part.renameTo(to)) {
            //noinspection ResultOfMethodCallIgnored
            part.delete();
            throw new IOException("Couldn't save " + to.getName());
        }
    }

    /// One piece (BLOCK bytes from `from`), written at its place; tries again a few times. true: the server sent the
    /// whole file instead (no pieces), all written.
    static boolean piece(Job job, String url, java.io.RandomAccessFile out, long from, long[] total,
            java.util.concurrent.atomic.AtomicLong done, Runnable progress) throws IOException {
        for (int attempt = 0; ; attempt++) {
            if (job.cancel) {
                throw new IOException("Cancelled");
            }
            HttpURLConnection c = null;
            long got = 0;
            try {
                c = (HttpURLConnection) new URL(url).openConnection();
                c.setConnectTimeout(20000);
                c.setReadTimeout(20000);
                c.setRequestProperty("User-Agent", USER_AGENT);
                c.setRequestProperty("Range", "bytes=" + from + "-" + (from + BLOCK - 1));
                final int code = c.getResponseCode();
                if (code != 206 && code != 200) {
                    throw new IOException("The audio's server said " + code);
                }
                final boolean whole = code == 200;
                if (whole && from > 0) {
                    throw new IOException("The audio's server doesn't send pieces");
                }
                if (whole) {
                    total[0] = c.getContentLengthLong();
                } else if (total[0] < 0) {
                    final String range = c.getHeaderField("Content-Range");   // bytes a-b/total
                    final int slash = range == null ? -1 : range.lastIndexOf('/');
                    if (slash > 0 && !range.endsWith("*")) {
                        total[0] = Long.parseLong(range.substring(slash + 1).trim());
                    }
                }
                final byte[] buf = new byte[65536];
                long at = from;
                try (InputStream in = c.getInputStream()) {
                    for (int n; (n = in.read(buf)) > 0;) {
                        if (job.cancel) {
                            throw new IOException("Cancelled");
                        }
                        synchronized (out) {
                            out.seek(at);
                            out.write(buf, 0, n);
                        }
                        at += n;
                        got += n;
                        done.addAndGet(n);
                        progress.run();
                    }
                }
                return whole;
            } catch (IOException e) {
                done.addAndGet(-got);   // (this piece again from its start)
                if (attempt >= 3 || job.cancel || "Cancelled".equals(e.getMessage())) {
                    throw e;
                }
                try {
                    Thread.sleep(400L * (attempt + 1));
                } catch (InterruptedException ie) {
                    throw new IOException("Cancelled");
                }
            } finally {
                if (c != null) {
                    c.disconnect();
                }
            }
        }
    }

    /// The video's picture as JPEG or PNG (YouTube's WebP ones: its JPEG of the same instead)
    static byte[] coverArt(StreamingService service, String url, StreamInfo info) throws IOException {
        final List<String> tries = new ArrayList<>();
        final String best = bestImage(info.getThumbnails());
        if (!best.isEmpty()) {
            tries.add(best);
        }
        final String id = idOf(service, url);
        if (service == ServiceList.YouTube && id != null) {
            tries.add("https://i.ytimg.com/vi/" + id + "/hqdefault.jpg");
        }
        for (String t : tries) {
            final HttpURLConnection c = (HttpURLConnection) new URL(t).openConnection();
            c.setConnectTimeout(15000);
            c.setReadTimeout(15000);
            c.setRequestProperty("User-Agent", USER_AGENT);
            try (InputStream in = c.getInputStream()) {
                final byte[] data = readAll(in);
                final boolean jpeg = data.length > 3 && (data[0] & 0xff) == 0xff && (data[1] & 0xff) == 0xd8;
                final boolean png = data.length > 4 && (data[0] & 0xff) == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G';
                if (jpeg || png) {
                    return data;
                }
            } catch (IOException e) {
                // the next
            } finally {
                c.disconnect();
            }
        }
        return null;
    }

    // ---- API (the hub: /api/web/<name>) ------------------------------------------------------------------

    /// zydek_web.py's api() for search, download, job, jobs and cancel, and preview (NewPipe's only): JSON in, JSON
    /// out, quick (the work runs in jobs). The hub adds "_music" (the app's own Music folder, for when the shared one
    /// isn't ours) and "_cache" (the app's cache folder, for previews).
    public static String api(String name, String argsJson) {
        try {
            final JSONObject args = argsJson == null || argsJson.isEmpty() ? new JSONObject() : new JSONObject(argsJson);
            switch (name) {
                case "search": {
                    final String q = args.optString("q", "").trim();
                    if (q.isEmpty()) {
                        return fail("Type something to search for");
                    }
                    return ok().put("job", search(q, "1".equals(args.optString("whole"))).id).toString();
                }
                case "download": {
                    final String url = args.optString("url", "");
                    if (url.isEmpty()) {
                        return fail("No link to download");
                    }
                    return ok().put("job", download(url, args.optString("title", ""), args.optString("_music", ""),
                            args.optString("_cache", "")).id).toString();
                }
                case "preview": {   // the whole audio into the app's cache, for the preview deck: {path, artist, title}
                    final String url = args.optString("url", "");
                    if (url.isEmpty()) {
                        return fail("No link to preview");
                    }
                    return ok().put("job", preview(url, args.optString("title", ""), args.optString("_cache", "")).id).toString();
                }
                case "job": {
                    final Job job;
                    synchronized (s_jobs) {
                        job = s_jobs.get(args.optString("id", ""));
                    }
                    if (job == null) {
                        return fail("No such job");
                    }
                    final JSONObject o = job.toJson();
                    o.put("ok", true);
                    return o.toString();
                }
                case "jobs": {   // the downloads, newest first
                    final List<Job> all;
                    synchronized (s_jobs) {
                        all = new ArrayList<>(s_jobs.values());
                    }
                    Collections.reverse(all);
                    final JSONArray out = new JSONArray();
                    for (Job j : all) {
                        if ("download".equals(j.kind)) {
                            out.put(j.toJson());
                        }
                    }
                    return ok().put("jobs", out).toString();
                }
                case "cancel": {
                    synchronized (s_jobs) {
                        final Job job = s_jobs.get(args.optString("id", ""));
                        if (job != null) {
                            job.cancel = true;
                        }
                    }
                    return ok().toString();
                }
                default:
                    return fail("Unknown request: " + name);
            }
        } catch (Throwable e) {
            return fail(friendly(e));
        }
    }

    static JSONObject ok() throws JSONException {
        return new JSONObject().put("ok", true);
    }

    static String fail(String error) {
        try {
            return new JSONObject().put("ok", false).put("error", error).toString();
        } catch (JSONException e) {
            return "{\"ok\":false}";
        }
    }
}
