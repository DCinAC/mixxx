#include "web.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <cmath>
#include <memory>

#include "library.h"

namespace zc {

namespace {

constexpr int kSearchResults = 20;
// AAC (m4a) first, as the phone asks: it plays everywhere Mixxx does; then anything but Dolby and FLAC-in-MP4
const QString kAudioFormat = QStringLiteral(
        "bestaudio[acodec^=mp4a]/bestaudio[ext=m4a][acodec!=ec-3][acodec!=flac]"
        "/bestaudio[acodec!=ec-3][acodec!=flac]/bestaudio[acodec!=ec-3]");
const QByteArray kUserAgent =
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
constexpr qint64 kKeepMs = 10 * 60 * 1000;   // a finished job, kept this long for its page to read
constexpr int kStemPort = 8770;

const QRegularExpression kUrl(QStringLiteral(R"(https?://\S+)"));
const QRegularExpression kBv(QStringLiteral(R"(\b(BV[0-9A-Za-z]{10})\b)"));
const QRegularExpression kJunk(
        QStringLiteral(R"(\s*[\(\[][^\)\]]*\b(official|video|audio|lyrics?|visuali[sz]er|hd|hq|4k|mv|m/v|clip)\b[^\)\]]*[\)\]])"),
        QRegularExpression::CaseInsensitiveOption);

qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

// Bundled with Connect (beside its program), else the computer's own (an app started from the Finder has a short
// PATH: Homebrew's folders are looked in too)
QString findTool(const QString& name) {
#if defined(Q_OS_WIN)
    const QString bundled = QCoreApplication::applicationDirPath() + QLatin1Char('/') + name + QStringLiteral(".exe");
#else
    const QString bundled = QCoreApplication::applicationDirPath() + QLatin1Char('/') + name;
#endif
    if (QFileInfo(bundled).isExecutable()) {
        return bundled;
    }
    const QString brew = QStandardPaths::findExecutable(name, {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")});
    return brew.isEmpty() ? QStandardPaths::findExecutable(name) : brew;
}

QString safeFileName(QString s) {
    static const QRegularExpression bad(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])"));
    static const QRegularExpression spaces(QStringLiteral(R"(\s+)"));
    s.replace(bad, QStringLiteral(" "));
    s.replace(spaces, QStringLiteral(" "));
    s = s.trimmed();
    while (s.startsWith(QLatin1Char('.'))) {   // (no hidden files)
        s.remove(0, 1);
    }
    s = s.left(150).trimmed();
    while (s.endsWith(QLatin1Char('.'))) {
        s.chop(1);
    }
    return s.isEmpty() ? QStringLiteral("untitled") : s;
}

// "name.ext" in dir, or "name (2).ext", "name (3).ext"... whichever isn't taken
QString freePath(const QString& dir, const QString& name, const QString& ext) {
    QString path = QDir(dir).filePath(name + ext);
    for (int n = 2; QFileInfo::exists(path); ++n) {
        path = QDir(dir).filePath(QStringLiteral("%1 (%2)%3").arg(name).arg(n).arg(ext));
    }
    return path;
}

// Artist and title for the file and its tags: the site's own (YouTube Music), else "Artist - Title" in the video's
// title (the usual junk like "(Official Video)" taken out), else the channel ("... - Topic" and "VEVO" taken off)
QPair<QString, QString> artistTitle(const QJsonObject& info) {
    QString artist = info.value(QStringLiteral("artist")).toString();
    if (artist.isEmpty()) {
        artist = info.value(QStringLiteral("artists")).toArray().at(0).toString();
    }
    const QString track = info.value(QStringLiteral("track")).toString();
    if (!artist.isEmpty() && !track.isEmpty()) {
        return {artist, track};
    }
    const QString title = info.value(QStringLiteral("title")).toString().remove(kJunk).trimmed();
    for (const QString sep : {QStringLiteral(" - "), QStringLiteral(" – "), QStringLiteral(" — "), QStringLiteral(" | ")}) {
        const int at = title.indexOf(sep);
        if (at > 0) {
            return {title.left(at).trimmed(), title.mid(at + sep.size()).trimmed()};
        }
    }
    static const QRegularExpression topic(QStringLiteral(R"(\s*-\s*Topic$|VEVO$)"));
    QString channel = info.value(QStringLiteral("channel")).toString();
    if (channel.isEmpty()) {
        channel = info.value(QStringLiteral("uploader")).toString();
    }
    return {channel.remove(topic).trimmed(), title};
}

// A search result, as the page shows it
QJsonObject entry(const QJsonObject& e, const QString& hint) {
    const QString site = (e.value(QStringLiteral("ie_key")).toString() + e.value(QStringLiteral("extractor_key")).toString() + hint).toLower();
    const QString id = e.value(QStringLiteral("id")).toString();
    QString url = e.value(QStringLiteral("webpage_url")).toString();
    if (url.isEmpty()) {
        url = e.value(QStringLiteral("url")).toString();
    }
    if (site.contains(QLatin1String("youtube")) && !id.isEmpty() && !url.startsWith(QLatin1String("http"))) {
        url = QStringLiteral("https://www.youtube.com/watch?v=") + id;
    }
    QString thumb = e.value(QStringLiteral("thumbnail")).toString();
    if (thumb.isEmpty()) {
        const QJsonArray thumbs = e.value(QStringLiteral("thumbnails")).toArray();
        thumb = thumbs.isEmpty() ? QString() : thumbs.at(thumbs.size() - 1).toObject().value(QStringLiteral("url")).toString();
    }
    if (thumb.isEmpty() && site.contains(QLatin1String("youtube")) && !id.isEmpty()) {
        thumb = QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(id);
    }
    QString channel = e.value(QStringLiteral("channel")).toString();
    if (channel.isEmpty()) {
        channel = e.value(QStringLiteral("uploader")).toString();
    }
    const bool bili = site.contains(QLatin1String("bili")) || url.contains(QLatin1String("bilibili"));
    const bool intl = site.contains(QLatin1String("intl")) || url.contains(QLatin1String("bilibili.tv"));
    return {{"id", id},
            {"title", e.value(QStringLiteral("title")).toString()},
            {"channel", channel},
            {"duration", e.value(QStringLiteral("duration"))},
            {"views", e.value(QStringLiteral("view_count"))},
            {"thumbnail", thumb},
            {"url", url},
            {"site", bili ? (intl ? QStringLiteral("bilibili-intl") : QStringLiteral("bilibili"))
                          : site.contains(QLatin1String("youtube")) ? QStringLiteral("youtube")
                                                                    : site}};
}

// yt-dlp's last "ERROR: ..." line, without the prefix (or its last line)
QString toolError(const QByteArray& err) {
    const QStringList lines = QString::fromUtf8(err).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        if (it->startsWith(QLatin1String("ERROR:"))) {
            return it->mid(6).trimmed();
        }
    }
    return lines.isEmpty() ? QStringLiteral("yt-dlp stopped") : lines.last().trimmed();
}

// A request nobody waits on: deleted once it's done (not before: that would cancel it)
void forget(QNetworkReply* reply) {
    QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

QString cacheDir() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/yt-dlp");
}

// The stem server's address as typed: "http://" and the port added if they're left out
QString stemBase(QString server) {
    server = server.trimmed();
    while (server.endsWith(QLatin1Char('/'))) {
        server.chop(1);
    }
    if (server.isEmpty()) {
        return {};
    }
    if (!server.contains(QLatin1String("://"))) {
        server.prepend(QStringLiteral("http://"));
    }
    const QUrl u(server);
    return u.port() < 0 ? server + QLatin1Char(':') + QString::number(kStemPort) : server;
}

} // namespace

Web::Web(Library* library, QObject* parent)
        : QObject(parent),
          m_library(library) {
}

Web::~Web() {
    for (Job* j : std::as_const(m_jobs)) {
        if (j->process) {
            j->process->disconnect(this);
            j->process->kill();
        }
        if (j->reply) {
            j->reply->disconnect(this);
            j->reply->abort();
        }
        delete j;
    }
}

QString Web::ytDlp() {
    return findTool(QStringLiteral("yt-dlp"));
}

QString Web::ffmpeg() {
    return findTool(QStringLiteral("ffmpeg"));
}

QString Web::folder() const {
    const QString set = QSettings().value(QStringLiteral("web/folder")).toString();
    if (!set.isEmpty()) {
        return set;
    }
    // "ZyDeck Web" in whichever music folder has one (Mixxx ZyDeck looks the same way), else made in the first
    const QStringList roots = m_library->rootDirs();
    for (const QString& root : roots) {
        const QString f = QDir(root).filePath(QStringLiteral("ZyDeck Web"));
        if (QFileInfo(f).isDir()) {
            return f;
        }
    }
    return roots.isEmpty() ? QString() : QDir(roots.first()).filePath(QStringLiteral("ZyDeck Web"));
}

// Mixxx's built-in effects by id, for effects.xml (which names none): their names in Mixxx's manifests
namespace {
const QHash<QString, QString>& builtinEffectNames() {
    static const QHash<QString, QString> names{
            {"org.mixxx.effects.autogaincontrol", "Auto Gain Control"}, {"org.mixxx.effects.autopan", "Autopan"},
            {"org.mixxx.effects.balance", "Stereo Balance"}, {"org.mixxx.effects.bessel4lvmixeq", "Bessel4 LV-Mix Isolator"},
            {"org.mixxx.effects.bessel8lvmixeq", "Bessel8 LV-Mix Isolator"},
            {"org.mixxx.effects.biquadfullkilleq", "Biquad Full Kill Equalizer"}, {"org.mixxx.effects.bitcrusher", "Bitcrusher"},
            {"org.mixxx.effects.compressor", "Compressor"}, {"org.mixxx.effects.distortion", "Distortion"},
            {"org.mixxx.effects.echo", "Echo"}, {"org.mixxx.effects.filter", "Filter"}, {"org.mixxx.effects.flanger", "Flanger"},
            {"org.mixxx.effects.gain", "Gain"}, {"org.mixxx.effects.glitch", "Glitch"},
            {"org.mixxx.effects.graphiceq", "Graphic Equalizer"}, {"org.mixxx.effects.keycomparison", "Key Comparison"},
            {"org.mixxx.effects.linkwitzrileyeq", "LinkwitzRiley8 Isolator"},
            {"org.mixxx.effects.loudnesscontour", "Loudness Contour"}, {"org.mixxx.effects.metronome", "Metronome"},
            {"org.mixxx.effects.moogladder4filter", "Moog Ladder 4 Filter"},
            {"org.mixxx.effects.parametriceq", "Parametric Equalizer"}, {"org.mixxx.effects.phaser", "Phaser"},
            {"org.mixxx.effects.pitchshift", "Pitch Shift"}, {"org.mixxx.effects.reverb", "Reverb"},
            {"org.mixxx.effects.threebandbiquadeq", "Biquad Equalizer"}, {"org.mixxx.effects.tremolo", "Tremolo/Trans"},
            {"org.mixxx.effects.whitenoise", "White Noise"}, {"org.zydeck.effects.sidechain", "Sidechain"}};
    return names;
}
} // namespace

QJsonObject Web::effects() {
    const QString dir = mixxxDir();
    if (dir.isEmpty()) {
        return {{"ok", false}, {"error", "Mixxx's settings aren't here"}};
    }
    const QFileInfo live(dir + QStringLiteral("/zydeck-effects.json")), xml(dir + QStringLiteral("/effects.xml"));
    QJsonArray names;
    if (live.isFile() && (!xml.isFile() || live.lastModified() >= xml.lastModified())) {   // (Mixxx ZyDeck's, kept up to date)
        QFile f(live.filePath());
        if (f.open(QIODevice::ReadOnly)) {
            for (const QJsonValue& e : QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("effects")).toArray()) {
                names.append(e.toObject().value(QStringLiteral("name")).toString());
            }
        }
    }
    if (names.isEmpty()) {   // effects.xml's <VisibleEffects>: <Effect><Id>...</Id> in order
        QFile f(xml.filePath());
        if (!f.open(QIODevice::ReadOnly)) {
            return {{"ok", false}, {"error", "Mixxx hasn't saved its effects yet (it does as it quits)"}};
        }
        QXmlStreamReader r(&f);
        bool inVisible = false;
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement() && r.name() == QLatin1String("VisibleEffects")) {
                inVisible = true;
            } else if (r.isEndElement() && r.name() == QLatin1String("VisibleEffects")) {
                break;
            } else if (inVisible && r.isStartElement() && r.name() == QLatin1String("Id")) {
                const QString id = r.readElementText();
                names.append(builtinEffectNames().value(id, id.section(QLatin1Char('/'), -1).section(QLatin1Char('#'), -1)));
            }
        }
    }
    return {{"ok", true}, {"effects", names}};
}

QJsonObject Web::handle(const QString& what, const QUrlQuery& query, int* status) {
    *status = 200;
    const auto arg = [&query](const char* name) {
        return query.queryItemValue(QString::fromLatin1(name), QUrl::FullyDecoded);
    };
    if (what == QLatin1String("job")) {
        const Job* j = m_jobs.value(arg("id"));
        if (!j) {
            *status = 404;
            return {{"ok", false}, {"error", "No such job (Connect restarted?)"}};
        }
        QJsonObject out{{"state", j->state}, {"progress", j->progress}, {"message", j->message}, {"title", j->title}};
        if (j->state == QLatin1String("done")) {
            out.insert(QStringLiteral("result"), j->result);
        } else if (j->state == QLatin1String("error")) {
            out.insert(QStringLiteral("error"), j->error);
        }
        return out;
    }
    if (what == QLatin1String("cancel")) {
        Job* j = m_jobs.value(arg("id"));
        if (j && j->state == QLatin1String("running")) {
            j->cancelled = true;
            if (j->process) {
                j->process->kill();
            }
            if (j->reply) {
                j->reply->abort();
            }
            if (!j->remoteId.isEmpty()) {
                forget(stemRequest(QStringLiteral("DELETE"), QStringLiteral("/jobs/") + j->remoteId));
            }
            fail(j, QStringLiteral("Cancelled"));
        }
        return {{"ok", true}};
    }
    if (what == QLatin1String("fx")) {
        return effects();
    }
    if (what == QLatin1String("status")) {
        return {{"ytdlp", !ytDlp().isEmpty()}, {"ffmpeg", !ffmpeg().isEmpty()}, {"folder", folder()},
                {"stemServer", QSettings().value(QStringLiteral("stems/server")).toString()}};
    }
    if (what == QLatin1String("search") || what == QLatin1String("download")) {
        if (ytDlp().isEmpty()) {
            *status = 409;
            return {{"ok", false}, {"error", "yt-dlp isn't on this computer (it comes with ZyDeck Connect's builds)"}};
        }
        const QString q = what == QLatin1String("search") ? arg("q").trimmed() : arg("url").trimmed();
        if (q.isEmpty()) {
            *status = 400;
            return {{"ok", false}, {"error", "Nothing to look for"}};
        }
        if (what == QLatin1String("search")) {
            Job* j = start(QStringLiteral("search"), q);
            search(j, q);
            return {{"job", j->id}};
        }
        Job* j = start(QStringLiteral("download"), arg("title").isEmpty() ? q : arg("title"));
        download(j, q);
        return {{"job", j->id}};
    }
    if (what == QLatin1String("stems")) {
        const QString path = arg("path");
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            *status = 400;
            return {{"ok", false}, {"error", "The track's file isn't on this computer"}};
        }
        if (!arg("server").isEmpty()) {   // (the phone's setting, kept for Mixxx ZyDeck's own requests)
            QSettings().setValue(QStringLiteral("stems/server"), arg("server"));
        }
        Job* j = start(QStringLiteral("stems"), QFileInfo(path).completeBaseName());
        stems(j, path, arg("preset").isEmpty() ? QStringLiteral("best") : arg("preset"));
        return {{"job", j->id}};
    }
    *status = 404;
    return {{"ok", false}, {"error", "No such thing"}};
}

Web::Job* Web::start(const QString& kind, const QString& title) {
    prune();
    Job* j = new Job;
    j->id = QString::number(QRandomGenerator::global()->generate64() & 0xffffffffffffULL, 16);
    j->kind = kind;
    j->title = title;
    m_jobs.insert(j->id, j);
    return j;
}

void Web::finish(Job* job, const QJsonValue& result) {
    if (job->state != QLatin1String("running")) {
        return;
    }
    job->state = QStringLiteral("done");
    job->progress = 1;
    job->result = result;
    job->endedMs = nowMs();
}

void Web::fail(Job* job, const QString& error) {
    if (job->state != QLatin1String("running")) {
        return;
    }
    job->state = QStringLiteral("error");
    job->error = error;
    job->endedMs = nowMs();
}

void Web::prune() {
    const qint64 now = nowMs();
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it.value()->endedMs && now - it.value()->endedMs > kKeepMs) {
            delete it.value();
            it = m_jobs.erase(it);
        } else {
            ++it;
        }
    }
}

QProcess* Web::run(Job* job, const QStringList& args) {
    auto* p = new QProcess(this);
    p->setProgram(ytDlp());
    p->setArguments(QStringList{QStringLiteral("--no-warnings"), QStringLiteral("--socket-timeout"), QStringLiteral("20"),
                            QStringLiteral("--cache-dir"), cacheDir()} + args);
    job->process = p;
    connect(p, &QProcess::errorOccurred, this, [this, job, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            fail(job, QStringLiteral("yt-dlp wouldn't start"));
            p->deleteLater();
        }
    });
    p->start();
    return p;
}

// ---- search ---------------------------------------------------------------------------------------------------
// A pasted link as it is, a BV code as its Bilibili video, else a YouTube search
void Web::search(Job* job, const QString& query) {
    QString target;
    bool link = false;
    if (const auto m = kUrl.match(query); m.hasMatch()) {
        target = m.captured(0);
        link = true;
    } else if (const auto b = kBv.match(query); b.hasMatch()) {
        target = QStringLiteral("https://www.bilibili.com/video/") + b.captured(1);
        link = true;
    } else {
        target = QStringLiteral("ytsearch%1:%2").arg(kSearchResults).arg(query);
    }
    const auto bv = kBv.match(target);
    if (!link || !bv.hasMatch() || !target.contains(QLatin1String("bilibili.com"))) {
        searchWith(job, target);
        return;
    }
    // A Bilibili video: its parts (P1, P2, ...) with their names from Bilibili itself (yt-dlp's quick listing gives
    // them as bare links); yt-dlp if that doesn't work out
    job->message = QStringLiteral("Asking Bilibili");
    QNetworkRequest r(QUrl(QStringLiteral("https://api.bilibili.com/x/web-interface/view?bvid=") + bv.captured(1)));
    r.setRawHeader("User-Agent", kUserAgent);
    r.setRawHeader("Referer", "https://www.bilibili.com/");
    QNetworkReply* reply = m_net.get(r);
    job->reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, job, reply, target] {
        reply->deleteLater();
        if (job->cancelled) {
            return;
        }
        const QJsonObject data = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || data.value(QStringLiteral("code")).toInt(-1) != 0) {
            searchWith(job, target);
            return;
        }
        const QJsonObject v = data.value(QStringLiteral("data")).toObject();
        const QJsonArray pages = v.value(QStringLiteral("pages")).toArray();
        const QString owner = v.value(QStringLiteral("owner")).toObject().value(QStringLiteral("name")).toString();
        const QString bvid = v.value(QStringLiteral("bvid")).toString();
        const QString base = QStringLiteral("https://www.bilibili.com/video/") + bvid;
        const QString only = QUrlQuery(QUrl(target)).queryItemValue(QStringLiteral("p"));
        QJsonArray out;
        if (pages.size() <= 1) {
            out.append(QJsonObject{{"id", bvid}, {"title", v.value(QStringLiteral("title"))}, {"channel", owner},
                    {"duration", v.value(QStringLiteral("duration"))},
                    {"views", v.value(QStringLiteral("stat")).toObject().value(QStringLiteral("view"))},
                    {"thumbnail", v.value(QStringLiteral("pic"))}, {"url", base}, {"site", "bilibili"}});
        } else {
            for (const QJsonValue& pv : pages) {
                const QJsonObject pg = pv.toObject();
                const QString n = QString::number(pg.value(QStringLiteral("page")).toInt());
                if (!only.isEmpty() && only != n) {
                    continue;
                }
                QString part = pg.value(QStringLiteral("part")).toString();
                if (part.isEmpty()) {
                    part = QStringLiteral("Part ") + n;
                }
                QString thumb = pg.value(QStringLiteral("first_frame")).toString();
                if (thumb.isEmpty()) {
                    thumb = v.value(QStringLiteral("pic")).toString();
                }
                out.append(QJsonObject{{"id", bvid + QStringLiteral("_p") + n}, {"title", QStringLiteral("P%1 · %2").arg(n, part)},
                        {"channel", owner + QStringLiteral(" · ") + v.value(QStringLiteral("title")).toString()},
                        {"duration", pg.value(QStringLiteral("duration"))}, {"views", QJsonValue()},
                        {"thumbnail", thumb}, {"url", base + QStringLiteral("?p=") + n}, {"site", "bilibili"}});
            }
        }
        finish(job, out);
    });
}

void Web::searchWith(Job* job, const QString& target) {
    job->message = QStringLiteral("Searching");
    QProcess* p = run(job, {QStringLiteral("--flat-playlist"), QStringLiteral("-J"), target});
    connect(p, &QProcess::finished, this, [this, job, p](int code) {
        const QByteArray out = p->readAllStandardOutput(), err = p->readAllStandardError();
        p->deleteLater();
        if (job->cancelled) {
            return;
        }
        if (code != 0) {
            fail(job, toolError(err));
            return;
        }
        const QJsonObject info = QJsonDocument::fromJson(out).object();
        const QJsonArray entries = info.contains(QStringLiteral("entries")) ? info.value(QStringLiteral("entries")).toArray()
                                                                          : QJsonArray{info};
        const QString hint = info.value(QStringLiteral("extractor_key")).toString();
        QJsonArray list;
        int i = 0;
        for (const QJsonValue& v : entries) {
            const QJsonObject e = v.toObject();
            ++i;
            if (e.isEmpty() || (e.value(QStringLiteral("url")).toString().isEmpty() &&
                                       e.value(QStringLiteral("webpage_url")).toString().isEmpty() &&
                                       e.value(QStringLiteral("id")).toString().isEmpty())) {
                continue;
            }
            QJsonObject x = entry(e, hint);
            if (x.value(QStringLiteral("title")).toString().isEmpty()) {   // a playlist's bare parts: at least which one
                const QString p = QUrlQuery(QUrl(x.value(QStringLiteral("url")).toString())).queryItemValue(QStringLiteral("p"));
                QString name = info.value(QStringLiteral("title")).toString();
                x.insert(QStringLiteral("title"), QStringLiteral("%1 · P%2").arg(name.isEmpty() ? QStringLiteral("Part") : name,
                                                                         p.isEmpty() ? QString::number(i) : p));
                if (x.value(QStringLiteral("channel")).toString().isEmpty()) {
                    x.insert(QStringLiteral("channel"), info.value(QStringLiteral("uploader")));
                }
            }
            list.append(x);
        }
        finish(job, list);
    });
}

// ---- download -------------------------------------------------------------------------------------------------
void Web::download(Job* job, const QString& url) {
    const QString dir = folder();
    if (dir.isEmpty()) {
        fail(job, QStringLiteral("Mixxx has no music folder yet"));
        return;
    }
    if (!QDir().mkpath(dir + QStringLiteral("/.incoming"))) {
        fail(job, QStringLiteral("Can't write to %1 (is the network drive connected?)").arg(dir));
        return;
    }
    QStringList args{QStringLiteral("-f"), kAudioFormat, QStringLiteral("--no-playlist"), QStringLiteral("--newline"),
            QStringLiteral("--no-overwrites"), QStringLiteral("--progress-template"),
            QStringLiteral("download:ZDP %(progress.downloaded_bytes)s %(progress.total_bytes)s %(progress.total_bytes_estimate)s"),
            QStringLiteral("-o"), dir + QStringLiteral("/.incoming/%(title).120B [%(id)s].%(ext)s"),
            QStringLiteral("--no-simulate"), QStringLiteral("--print"), QStringLiteral("after_move:ZDJ %()j")};
    const QString ff = ffmpeg();
    if (!ff.isEmpty()) {
        args << QStringLiteral("--ffmpeg-location") << ff << QStringLiteral("--embed-thumbnail")
             << QStringLiteral("--convert-thumbnails") << QStringLiteral("jpg");
    } else {
        args << QStringLiteral("--fixup") << QStringLiteral("never");   // (no ffmpeg: Mixxx reads a DASH m4a as it is)
    }
    args << url;
    job->message = QStringLiteral("Finding the audio");
    auto info = std::make_shared<QJsonObject>();
    QProcess* p = run(job, args);
    connect(p, &QProcess::readyReadStandardOutput, this, [job, p, info] {
        while (p->canReadLine()) {
            const QByteArray line = p->readLine().trimmed();
            if (line.startsWith("ZDP ")) {
                const QList<QByteArray> n = line.mid(4).split(' ');
                const double got = n.value(0).toDouble(), total = n.value(1).toDouble() > 0 ? n.value(1).toDouble() : n.value(2).toDouble();
                if (total > 0) {
                    job->progress = std::min(0.95, got / total);
                }
                job->message = QStringLiteral("Downloading");
            } else if (line.startsWith("ZDJ ")) {
                *info = QJsonDocument::fromJson(line.mid(4)).object();
            }
        }
    });
    connect(p, &QProcess::finished, this, [this, job, p, info](int code) {
        const QByteArray err = p->readAllStandardError();
        p->deleteLater();
        if (job->cancelled) {
            return;
        }
        if (code != 0) {
            fail(job, toolError(err));
        } else if (info->isEmpty()) {
            fail(job, QStringLiteral("yt-dlp didn't say where the file went"));
        } else {
            downloaded(job, *info);
        }
    });
}

// Tagged (with ffmpeg) and moved out of .incoming as "Artist - Title": Mixxx ZyDeck takes it in from there
void Web::downloaded(Job* job, const QJsonObject& info) {
    QString path = info.value(QStringLiteral("filepath")).toString();
    if (path.isEmpty()) {
        path = info.value(QStringLiteral("requested_downloads")).toArray().at(0).toObject().value(QStringLiteral("filepath")).toString();
    }
    if (!QFileInfo::exists(path)) {
        fail(job, QStringLiteral("The download went missing"));
        return;
    }
    const QPair<QString, QString> names = artistTitle(info);
    const QString artist = names.first, title = names.second;
    const QString ext = QLatin1Char('.') + QFileInfo(path).suffix().toLower();
    const QJsonObject fmt = info.value(QStringLiteral("requested_downloads")).toArray().at(0).toObject();
    QString codec = fmt.value(QStringLiteral("acodec")).toString();
    if (codec.isEmpty()) {
        codec = info.value(QStringLiteral("acodec")).toString();
    }
    const double tbr = fmt.value(QStringLiteral("tbr")).toDouble(info.value(QStringLiteral("tbr")).toDouble());
    const QString page = info.value(QStringLiteral("webpage_url")).toString();
    const QString dir = folder();
    const auto move = [this, job, artist, title, ext, codec, tbr, info, dir](const QString& from) {
        const QString to = freePath(dir, safeFileName(artist.isEmpty() ? title : artist + QStringLiteral(" - ") + title), ext);
        if (!QFile::rename(from, to)) {
            fail(job, QStringLiteral("Couldn't move the download into %1").arg(dir));
            return;
        }
        static const QStringList playable{".m4a", ".mp4", ".mp3", ".ogg", ".opus", ".flac", ".wav", ".webm"};
        finish(job, QJsonObject{{"path", to}, {"artist", artist}, {"title", title}, {"duration", info.value(QStringLiteral("duration"))},
                            {"codec", codec}, {"kbps", static_cast<int>(std::lround(tbr))}, {"playable", playable.contains(ext)}});
    };
    const QString ff = ffmpeg();
    if (ff.isEmpty()) {
        move(path);
        return;
    }
    // the title, artist and link in the file's tags (the cover is in already), copied, not encoded again
    job->message = QStringLiteral("Tagging");
    const QString tagged = path + QStringLiteral(".tagged") + ext;
    auto* p = new QProcess(this);
    job->process = p;
    p->setProgram(ff);
    p->setArguments({QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-i"), path, QStringLiteral("-map"), QStringLiteral("0"), QStringLiteral("-c"), QStringLiteral("copy"),
            QStringLiteral("-metadata"), QStringLiteral("title=") + title, QStringLiteral("-metadata"), QStringLiteral("artist=") + artist,
            QStringLiteral("-metadata"), QStringLiteral("comment=") + page, tagged});
    connect(p, &QProcess::finished, this, [job, p, path, tagged, move](int code) {
        p->deleteLater();
        if (job->cancelled) {
            QFile::remove(tagged);
            return;
        }
        if (code == 0 && QFileInfo(tagged).size() > 0 && QFile::remove(path) && QFile::rename(tagged, path)) {
            move(path);
        } else {   // (untagged is still fine: Mixxx reads "Artist - Title" from the name)
            QFile::remove(tagged);
            move(path);
        }
    });
    p->start();
}

// ---- stems from the PC ----------------------------------------------------------------------------------------
QNetworkReply* Web::stemRequest(const QString& method, const QString& path, QIODevice* body) {
    const QString base = stemBase(QSettings().value(QStringLiteral("stems/server")).toString());
    QNetworkRequest r(QUrl(base + path));
    const QString token = QSettings().value(QStringLiteral("stems/token")).toString();
    if (!token.isEmpty()) {   // (only from outside the home network, a tunnel, say)
        r.setRawHeader("X-Zydek-Token", token.toUtf8());
    }
    r.setTransferTimeout(method == QLatin1String("POST") ? 120000 : 60000);
    if (method == QLatin1String("POST")) {
        r.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
        return m_net.post(r, body);
    }
    if (method == QLatin1String("DELETE")) {
        return m_net.deleteResource(r);
    }
    return m_net.get(r);
}

namespace {
// The stem server's own words for what went wrong, or that it couldn't be reached
QString stemError(QNetworkReply* reply, const QByteArray& body) {
    const QString detail = QJsonDocument::fromJson(body).object().value(QStringLiteral("detail")).toString();
    if (!detail.isEmpty()) {
        return QStringLiteral("Stem server: ") + detail;
    }
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() > 0) {
        return QStringLiteral("Stem server: ") + reply->errorString();
    }
    return QStringLiteral("Can't reach the stem server at %1 (%2)").arg(reply->url().host(), reply->errorString());
}
} // namespace

void Web::stems(Job* job, const QString& path, const QString& preset) {
    if (stemBase(QSettings().value(QStringLiteral("stems/server")).toString()).isEmpty()) {
        fail(job, QStringLiteral("Set the stem server's address first (on the phone: Settings › PC servers)"));
        return;
    }
    if (folder().isEmpty() || !QDir().mkpath(folder() + QStringLiteral("/.incoming"))) {
        fail(job, QStringLiteral("Can't write to the downloads folder (is the network drive connected?)"));
        return;
    }
    auto* file = new QFile(path);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        fail(job, QStringLiteral("Couldn't read %1").arg(path));
        return;
    }
    job->original = path;
    job->message = QStringLiteral("Sending to the PC");
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"), QFileInfo(path).fileName());
    q.addQueryItem(QStringLiteral("preset"), preset);
    QNetworkReply* reply = stemRequest(QStringLiteral("POST"), QStringLiteral("/jobs?") + q.toString(QUrl::FullyEncoded), file);
    file->setParent(reply);
    job->reply = reply;
    connect(reply, &QNetworkReply::uploadProgress, this, [job](qint64 sent, qint64 total) {
        if (total > 0) {
            job->progress = 0.15 * static_cast<double>(sent) / static_cast<double>(total);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, job, reply] {
        reply->deleteLater();
        if (job->cancelled) {
            return;
        }
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            fail(job, stemError(reply, body));
            return;
        }
        job->remoteId = QJsonDocument::fromJson(body).object().value(QStringLiteral("id")).toString();
        if (job->remoteId.isEmpty()) {
            fail(job, QStringLiteral("The stem server didn't take it"));
            return;
        }
        stemsPoll(job);
    });
}

void Web::stemsPoll(Job* job) {
    QNetworkReply* reply = stemRequest(QStringLiteral("GET"), QStringLiteral("/jobs/") + job->remoteId);
    job->reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, job, reply] {
        reply->deleteLater();
        if (job->cancelled) {
            return;
        }
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            fail(job, stemError(reply, body));
            return;
        }
        const QJsonObject r = QJsonDocument::fromJson(body).object();
        const QString st = r.value(QStringLiteral("status")).toString();
        if (st == QLatin1String("error")) {
            fail(job, r.value(QStringLiteral("message")).toString(QStringLiteral("The stem server failed")));
            forget(stemRequest(QStringLiteral("DELETE"), QStringLiteral("/jobs/") + job->remoteId));
            return;
        }
        if (st == QLatin1String("done")) {
            const QString artist = r.value(QStringLiteral("artist")).toString(), title = r.value(QStringLiteral("title")).toString();
            job->outName = safeFileName(!artist.isEmpty() ? artist + QStringLiteral(" - ") + title
                                        : !title.isEmpty() ? title
                                                           : QFileInfo(job->original).completeBaseName());
            job->result = QJsonObject{{"artist", artist}, {"title", title}, {"seconds", r.value(QStringLiteral("seconds"))}};
            stemsFetch(job);
            return;
        }
        const int ahead = r.value(QStringLiteral("ahead")).toInt();
        job->message = st == QLatin1String("queued") ? (ahead ? QStringLiteral("Waiting on the PC (%1 ahead)").arg(ahead)
                                                              : QStringLiteral("Waiting on the PC"))
                : st == QLatin1String("separating") ? QStringLiteral("Separating on the PC")
                                                    : QStringLiteral("Writing the stem file");
        job->progress = 0.15 + 0.7 * r.value(QStringLiteral("progress")).toDouble();
        QTimer::singleShot(1000, this, [this, job] {
            if (m_jobs.contains(job->id) && !job->cancelled) {
                stemsPoll(job);
            }
        });
    });
}

// The .stem.mp4 into .incoming, then into the folder (Mixxx ZyDeck takes it in); the PC keeps nothing after
void Web::stemsFetch(Job* job) {
    job->message = QStringLiteral("Downloading the stems");
    const QString part = QDir(folder()).filePath(QStringLiteral(".incoming/") + job->outName + QStringLiteral(".stem.mp4.part"));
    auto* out = new QFile(part, this);
    if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete out;
        fail(job, QStringLiteral("Can't write to the downloads folder"));
        return;
    }
    QNetworkReply* reply = stemRequest(QStringLiteral("GET"), QStringLiteral("/jobs/%1/file").arg(job->remoteId));
    job->reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [reply, out] { out->write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, this, [job](qint64 got, qint64 total) {
        if (total > 0) {
            job->progress = 0.85 + 0.15 * static_cast<double>(got) / static_cast<double>(total);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, job, reply, out, part] {
        reply->deleteLater();
        out->write(reply->readAll());
        out->close();
        out->deleteLater();
        forget(stemRequest(QStringLiteral("DELETE"), QStringLiteral("/jobs/") + job->remoteId));
        if (job->cancelled || reply->error() != QNetworkReply::NoError) {
            QFile::remove(part);
            if (!job->cancelled) {
                fail(job, stemError(reply, {}));
            }
            return;
        }
        const QString to = freePath(folder(), job->outName, QStringLiteral(".stem.mp4"));
        // a note beside it naming the original (hidden): Mixxx ZyDeck gives the stems its cues, beat grid and the rest
        QFile note(QDir(folder()).filePath(QLatin1Char('.') + QFileInfo(to).fileName() + QStringLiteral(".json")));
        if (note.open(QIODevice::WriteOnly)) {
            note.write(QJsonDocument(QJsonObject{{"original", job->original}}).toJson(QJsonDocument::Compact));
            note.close();
        }
        if (!QFile::rename(part, to)) {
            note.remove();
            QFile::remove(part);
            fail(job, QStringLiteral("Couldn't move the stems into the downloads folder"));
            return;
        }
        QJsonObject r = job->result.toObject();
        r.insert(QStringLiteral("path"), to);
        r.insert(QStringLiteral("original"), job->original);
        finish(job, r);
    });
}

} // namespace zc
