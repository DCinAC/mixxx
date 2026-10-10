#include "zydek/zydekhub.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "control/controlobject.h"
#include "controllers/zydek/zydekcontroller.h"
#include "mixer/playerinfo.h"
#include "moc_zydekhub.cpp"
#include "qml/qmlsoundmanagerproxy.h"
#include "soundio/sounddevice.h"
#include "soundio/soundmanager.h"
#include "soundio/soundmanagerconfig.h"
#include "soundio/soundmanagerutil.h"
#include "track/beats.h"
#include "zydek/zydekpython.h"
#include "track/track.h"
#include "waveform/waveform.h"

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace zydek {

namespace {

// SysEx F0 7D <type> ... F7 (0x7D: non-commercial manufacturer id). Same layout as the desktop bridge,
// see res/controllers/Zydek-Tablet.script.js.
constexpr int kSamplerLength = 0x4D;
constexpr int kDeckLength = 0x4E;
constexpr int kHotcue = 0x4F;
constexpr int kSet = 0x50;
constexpr int kSubscribe = 0x51;
constexpr int kScratch = 0x52;
constexpr int kTempo = 0x53;
constexpr int kSync = 0x54;
constexpr int kValue = 0x58;
constexpr int kPing = 0x5E;   // F0 7D 5E <seq 3x7 bits> F7, echoed back unchanged by the mapping
constexpr int kHello = 0x5F;
// Controller mode, with ZyDeck Link on the computer (the mapping ignores these): text as hex of UTF-8 JSON
constexpr int kLinkHello = 0x60;   // device -> computer: {"v": 1, "name": device}
constexpr int kLinkOffer = 0x61;   // computer -> device: {"name", "hosts": [...], "port", "key"}
constexpr int kNameAsk = 0x62;     // device -> computer: {"s": sampler?, "i": index, "ms": track length}
constexpr int kNameReply = 0x63;   // computer -> device: {"s", "i", "n": {t, a, id} or null}
const QString kRemoteFile = QStringLiteral("zydek-controller-mode.json");

const QString kSettingsFile = QStringLiteral("zydek-controller-settings.json");
const QString kSamplersFile = QStringLiteral("zydek-samplers.json");
const QString kKitsFile = QStringLiteral("zydek-kits.json");
constexpr int kKitSlots = 32;   // the pads ZyDeck shows
const QString kLastSession = QStringLiteral("Last session");
constexpr int kSamplerSlots = 64;
const QString kLatencyFile = QStringLiteral("zydek-latency.log");
constexpr qint64 kLatencyFileMax = 4 * 1024 * 1024;   // then it moves to .1, replacing the previous one

/// round(v * 1e5) + 2^34 as five 7-bit bytes, LSB first (exact for +-171k in 1e-5 steps).
// Control values: round(v * 1e5) + 2^48 as 7 x 7 bits, LSB first (sample positions need the range).
constexpr int kValueBytes = 7;

QList<int> encodeValue(double v) {
    const qint64 n = std::llround(v * 1e5) + (qint64(1) << 48);
    QList<int> out;
    for (int i = 0; i < kValueBytes; ++i) {
        out.append(static_cast<int>((n >> (7 * i)) & 0x7F));
    }
    return out;
}

double decodeValue(const QByteArray& bytes) {
    qint64 n = 0;
    for (int i = 0; i < kValueBytes && i < bytes.size(); ++i) {
        n |= qint64(static_cast<unsigned char>(bytes[i]) & 0x7F) << (7 * i);
    }
    return static_cast<double>(n - (qint64(1) << 48)) / 1e5;
}

QList<int> encodeName(const QString& group, const QString& key) {
    QList<int> out;
    const QString name = group + QLatin1Char(',') + key;
    for (const QChar c : name) {
        out.append(c.unicode() & 0x7F);
    }
    return out;
}

QJsonArray toArray(const int* values, int n) {
    QJsonArray a;
    for (int i = 0; i < n; ++i) {
        a.append(values[i]);
    }
    return a;
}

QByteArray contentType(const QString& name) {
    if (name.endsWith(QLatin1String(".html"))) {
        return "text/html; charset=utf-8";
    }
    if (name.endsWith(QLatin1String(".js"))) {
        return "text/javascript; charset=utf-8";
    }
    if (name.endsWith(QLatin1String(".css"))) {
        return "text/css; charset=utf-8";
    }
    if (name.endsWith(QLatin1String(".webmanifest")) || name.endsWith(QLatin1String(".json"))) {
        return "application/manifest+json";
    }
    if (name.endsWith(QLatin1String(".svg"))) {
        return "image/svg+xml";
    }
    if (name.endsWith(QLatin1String(".png"))) {
        return "image/png";
    }
    return "application/octet-stream";
}

HttpServer::Response json(const QJsonObject& o) {
    return {200, "application/json", QJsonDocument(o).toJson(QJsonDocument::Compact), {}};
}

HttpServer::Response json(const QJsonArray& a) {
    return {200, "application/json", QJsonDocument(a).toJson(QJsonDocument::Compact), {}};
}

/// Actions answer 200 with {ok: true, ...} or 409 with {ok: false, error}.
HttpServer::Response result(const QJsonObject& o) {
    HttpServer::Response r = json(o);
    if (!o.value(QStringLiteral("ok")).toBool(true)) {
        r.status = 409;
    }
    return r;
}

const QByteArray kIndexPage = QByteArrayLiteral(
        "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Zydek</title><style>body{font:18px system-ui,sans-serif;background:#111;color:#ddd;margin:24px}"
        "a{display:block;color:#ff8a1e;margin:14px 0}</style><h1>Zydek</h1>"
        "<a href=/phone>Library (phone)</a><a href=/controller>Controller (tablet)</a><a href=/settings>Settings and debug</a>"
        "<a href=/sizes>Size test</a>");

} // namespace

Hub::Hub(UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_library(pConfig) {
    m_clock.start();
    m_server.setHandler([this](const QString& path, const HttpServer::Query& query) {
        const qint64 start = m_clock.elapsed();
        HttpServer::Response r = handleHttp(path, query);
        const qint64 took = m_clock.elapsed() - start;
        if (took >= 15 && m_slowRequests.size() < 20) {
            m_slowRequests.append(QJsonArray{path, took});
        }
        return r;
    });
    m_lagTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_lagTimer, &QTimer::timeout, this, [this] {
        const qint64 now = m_clock.elapsed();
        if (m_lagLastMs) {
            m_lagMaxMs = std::max(m_lagMaxMs, now - m_lagLastMs - 20);
        }
        m_lagLastMs = now;
    });
    m_lagTimer.start(20);
    connect(&m_server, &HttpServer::wsOpened, this, &Hub::onWsOpened);
    connect(&m_server, &HttpServer::wsText, this, &Hub::onWsText);
    connect(&m_server, &HttpServer::wsClosed, this, &Hub::onWsClosed);
    // Names for the pages straight from Mixxx whenever a deck or sampler gets a track (PlayerInfo lives on
    // the main thread; the lambda runs here, on the hub's thread).
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            [this](const QString& group, TrackPointer, TrackPointer) {
                if (m_controllerMode) {
                    return;   // the pages show the computer's decks
                }
                const int n = QStringView(group).mid(group.indexOf(QRegularExpression(QStringLiteral("\\d")))).chopped(1).toInt();
                if (group.startsWith(QLatin1String("[Channel")) && n >= 1 && n <= kNumDecks) {
                    updateName(false, n - 1, 0);
                } else if (group.startsWith(QLatin1String("[Sampler")) && n >= 1 && n <= kNumSamplers) {
                    updateName(true, n - 1, 0);
                    if (m_lastSessionRestored) {
                        m_lastSessionTimer.start();
                    }
                }
            });

    QFile f(QDir(m_pConfig->getSettingsPath()).filePath(kSettingsFile));
    if (f.open(QIODevice::ReadOnly)) {
        m_ctlSettings = QJsonDocument::fromJson(f.readAll()).object();
    }
    m_remoteTimer.setInterval(1000);
    connect(&m_remoteTimer, &QTimer::timeout, this, &Hub::remoteTick);
}

#ifdef Q_OS_ANDROID
namespace {
Hub* s_pHub = nullptr;   // for ZydekMidi.java's callbacks

void JNICALL jniMidiReceive(JNIEnv* env, jclass, jbyteArray data) {
    const jsize n = env->GetArrayLength(data);
    QByteArray bytes(n, Qt::Uninitialized);
    env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
    if (Hub* pHub = s_pHub) {
        QMetaObject::invokeMethod(pHub, [pHub, bytes] { pHub->usbMidiReceived(bytes); }, Qt::QueuedConnection);
    }
}

void JNICALL jniMidiState(JNIEnv*, jclass, jint state) {
    if (Hub* pHub = s_pHub) {
        QMetaObject::invokeMethod(pHub, [pHub, state] { pHub->usbMidiState(state); }, Qt::QueuedConnection);
    }
}
} // namespace
#endif

void Hub::start() {
#ifdef Q_OS_ANDROID
    s_pHub = this;
    const JNINativeMethod methods[] = {
            {"nativeReceive", "([B)V", reinterpret_cast<void*>(jniMidiReceive)},
            {"nativeState", "(I)V", reinterpret_cast<void*>(jniMidiState)}};
    QJniEnvironment env;
    if (!env.registerNativeMethods("org/mixxx/ZydekMidi", methods, 2)) {
        qWarning() << "Zydek: no USB MIDI for controller mode";
    }
#endif
    loadRemote();
    m_lastSessionTimer.setSingleShot(true);
    m_lastSessionTimer.setInterval(2000);
    connect(&m_lastSessionTimer, &QTimer::timeout, this, [this] { saveKit(kLastSession); });
    QTimer::singleShot(5000, this, [this] { restoreLastSession(0); });
    if (m_server.listen(kPort)) {
        qInfo() << "Zydek: tablet controller at" << lanUrl();
    } else {
        qWarning() << "Zydek: can't listen on port" << kPort;
    }
}

void Hub::setController(ZydekController* pController) {
    m_pController = pController;
    // Tell open pages whether they're talking to Mixxx now.
    m_server.broadcast(QStringLiteral("/ws"), QJsonDocument(snapshot()).toJson(QJsonDocument::Compact));
}

// ---- HTTP -----------------------------------------------------------------------------------------

HttpServer::Response Hub::handleHttp(const QString& path, const HttpServer::Query& query) {
    Q_UNUSED(query);
    if (path == QLatin1String("/")) {
        return {200, "text/html; charset=utf-8", kIndexPage, {}};
    }
    if (path == QLatin1String("/controller") || path == QLatin1String("/settings") ||
            path == QLatin1String("/sizes") || path == QLatin1String("/phone") || path == QLatin1String("/grid") ||
            path == QLatin1String("/sample")) {
        return staticFile(path.mid(1) + QLatin1String(".html"));
    }
    if (path == QLatin1String("/controller.webmanifest")) {
        return staticFile(QStringLiteral("controller.webmanifest"));
    }
    if (path.startsWith(QLatin1String("/waveform/"))) {
        const QByteArray data = waveform(path.mid(10).toInt());
        if (data.isEmpty()) {
            return {404, "text/plain", "not analysed yet\n", {}};
        }
        return {200, "application/octet-stream", data, {{"Cache-Control", "max-age=3600"}}};
    }
    if (path == QLatin1String("/api/latency-log")) {
        return latencyLog(query);
    }
    if (path.startsWith(QLatin1String("/beats/"))) {
        const QByteArray data = beats(path.mid(7).toInt());
        if (data.isEmpty()) {
            return {404, "text/plain", "no beats yet\n", {}};
        }
        return {200, "application/json", data, {}};
    }
    if (path.startsWith(QLatin1String("/api/web/"))) {   // the phone page's Web tab: res/zydek/python/zydek_web.py
        QJsonObject args;
        for (auto it = query.cbegin(); it != query.cend(); ++it) {
            args.insert(it.key(), it.value());
        }
        const QByteArray body = python::call(path.mid(9), QJsonDocument(args).toJson(QJsonDocument::Compact));
        const bool ok = QJsonDocument::fromJson(body).object().value(QStringLiteral("ok")).toBool(true);
        return {ok ? 200 : 409, "application/json", body, {}};
    }
    if (path == QLatin1String("/api/audio")) {
        return json(audioStatus());
    }
    if (path == QLatin1String("/api/audio/set")) {   // ?main=<output name>&headphones=<output name, or empty>
        const QJsonObject result = setAudio(query);
        HttpServer::Response r = json(result);
        if (!result.value(QStringLiteral("ok")).toBool()) {
            r.status = 409;
        }
        return r;
    }
    if (path.startsWith(QLatin1String("/api/"))) {
        return handleApi(path, query);
    }
    if (path == QLatin1String("/lan-url")) {
        return json(QJsonObject{{"url", lanUrl()}});
    }
    if (path == QLatin1String("/lan-urls")) {
        return json(lanUrls());
    }
    if (path == QLatin1String("/status")) {
        return json(QJsonObject{{"live", live()}});
    }
    return {404, "text/plain", "not found\n", {}};
}

HttpServer::Response Hub::handleApi(const QString& path, const HttpServer::Query& query) {
    auto arg = [&query](const char* name) { return query.value(QLatin1String(name)); };
    if (path == QLatin1String("/api/library/views")) {
        return json(m_library.views());
    }
    if (path == QLatin1String("/api/library/tracks")) {
        return json(m_library.tracks(arg("q"),
                arg("view").isEmpty() ? QStringLiteral("all") : arg("view"),
                arg("sort").isEmpty() ? QStringLiteral("added") : arg("sort"),
                arg("limit").isEmpty() ? 200 : arg("limit").toInt(),
                arg("offset").toInt()));
    }
    if (path.startsWith(QLatin1String("/api/library/track/"))) {
        const QJsonObject t = m_library.track(path.section(QLatin1Char('/'), 4).toInt());
        return t.isEmpty() ? HttpServer::Response{404, "text/plain", "no such track\n", {}} : json(t);
    }
    if (path == QLatin1String("/api/decks")) {
        QJsonObject d = m_library.decks();
        d.insert(QStringLiteral("live"), live());
        return json(d);
    }
    if (path == QLatin1String("/api/load")) {   // ?track_id=&target=deck1..4|sampler|samplerN[&sync=1|0 for a sampler]
        const QJsonObject r = m_library.load(arg("track_id").toInt(), arg("target"));
        const QString group = r.value(QStringLiteral("group")).toString();
        if (r.value(QStringLiteral("ok")).toBool() && group.startsWith(QLatin1String("[Sampler"))) {
            Library::setSamplerOptions(group, arg("sync") != QLatin1String("0"), false);
            setSamplerMode(group.mid(8, group.size() - 9).toInt(), QStringLiteral("oneshot"));   // a whole track: plays once
        }
        return result(r);
    }
    if (path == QLatin1String("/api/load-path")) {   // ?path=&target=
        return result(m_library.loadLocation(arg("path"), arg("target")));
    }
    if (path == QLatin1String("/api/preview")) {
        return result(m_library.preview(arg("track_id").toInt()));
    }
    if (path == QLatin1String("/api/preview/stop")) {
        m_library.stopPreview();
        return json(QJsonObject{{"ok", true}});
    }
    if (path == QLatin1String("/api/folders")) {
        return json(m_library.folders());
    }
    if (path == QLatin1String("/api/folders/add")) {
        return result(m_library.addFolder(arg("path")));
    }
    if (path == QLatin1String("/api/folders/remove")) {   // ?path=
        return result(m_library.removeFolder(arg("path")));
    }
    if (path == QLatin1String("/api/library/add")) {   // ?path=  (a Web tab download)
        return result(m_library.addTrack(arg("path")));
    }
    if (path == QLatin1String("/api/crates/create")) {   // ?name=
        return result(m_library.createCrate(arg("name")));
    }
    if (path == QLatin1String("/api/crates/set")) {   // ?crate=&track=&on=1|0
        return result(m_library.setCrateTrack(arg("crate").toInt(), arg("track").toInt(), arg("on") != QLatin1String("0")));
    }
    if (path == QLatin1String("/api/crates/of")) {   // ?track=  -> crate ids holding the track
        return json(m_library.cratesOf(arg("track").toInt()));
    }
    if (path == QLatin1String("/api/fs")) {
        return json(m_library.listDirectory(arg("path")));
    }
    if (path == QLatin1String("/api/scan")) {
        m_library.startScan();
        return json(QJsonObject{{"ok", true}});
    }
    if (path == QLatin1String("/api/folders/excluded")) {
        return json(m_library.excludedFolders());
    }
    if (path == QLatin1String("/api/folders/exclude")) {   // ?path=&on=1|0
        return result(m_library.setExcluded(arg("path"), arg("on") != QLatin1String("0")));
    }
    if (path == QLatin1String("/api/controller")) {   // controller mode: ?on=1|0, ?link=1 (go wireless),
        // ?link=0 (not now / back to the cable), ?forget=1 (forget the computer)
        if (query.contains(QStringLiteral("on"))) {
            setControllerMode(arg("on") == QLatin1String("1"));
        }
        if (arg("link") == QLatin1String("1") && m_controllerMode && !m_linkOffer.isEmpty()) {
            m_linkHost = 0;
            linkConnect(m_linkOffer);
        } else if (arg("link") == QLatin1String("0")) {
            m_offerDeclined = true;
            linkDrop(false);
            m_linkSaved = {};   // stay on the cable: don't come back over Wi-Fi by itself
            saveRemote();
        }
        if (arg("forget") == QLatin1String("1")) {
            m_linkSaved = {};
            saveRemote();
            emitRemote();
        }
        return json(remoteStatus());
    }
    if (path == QLatin1String("/api/analyze/track")) {   // ?id=&fresh=1 (start over: new beat grid and key)
        return result(m_library.analyzeTrack(arg("id").toInt(), arg("fresh") == QLatin1String("1")));
    }
    if (path == QLatin1String("/api/grid/save")) {   // ?group=: the beat grid editor is done
        return result(m_library.saveLoaded(arg("group")));
    }
    if (path == QLatin1String("/api/analyze")) {   // ?dry=1: only count the tracks without BPM or key
        return result(m_library.analyzeAll(arg("dry") == QLatin1String("1")));
    }
    if (path == QLatin1String("/api/sampler/capture")) {   // ?deck=&slot=&sync=1|0
        const QJsonObject r = m_library.captureSample(arg("deck").toInt(), arg("slot").toInt(), arg("sync") != QLatin1String("0"));
        if (r.value(QStringLiteral("ok")).toBool()) {
            setSamplerMode(arg("slot").toInt(), QStringLiteral("loop"));   // a captured loop loops
        }
        return result(r);
    }
    if (path == QLatin1String("/api/grid/bpm") || path == QLatin1String("/api/grid/lock")) {   // ?group=&bpm= · ?group=&on=1|0
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(arg("group"));
        if (!pTrack) {
            return result({{"ok", false}, {"error", "Nothing loaded there"}});
        }
        if (path.endsWith(QLatin1String("lock"))) {
            pTrack->setBpmLocked(arg("on") != QLatin1String("0"));
            return result({{"ok", true}, {"locked", pTrack->isBpmLocked()}});
        }
        const double bpm = arg("bpm").toDouble();
        if (pTrack->isBpmLocked()) {
            return result({{"ok", false}, {"error", "The grid is locked"}});
        }
        if (bpm < 30 || bpm > 300 || !pTrack->trySetBpm(bpm)) {
            return result({{"ok", false}, {"error", "Couldn't set that BPM"}});
        }
        return result({{"ok", true}, {"bpm", pTrack->getBpm()}});
    }
    if (path == QLatin1String("/api/kits")) {
        return json(kits());
    }
    if (path == QLatin1String("/api/kits/save")) {   // ?name=
        return result(saveKit(arg("name")));
    }
    if (path == QLatin1String("/api/kits/load")) {   // ?name=
        return result(loadKit(arg("name")));
    }
    if (path == QLatin1String("/api/kits/delete")) {   // ?name=
        return result(deleteKit(arg("name")));
    }
    if (path == QLatin1String("/api/sampler/cut")) {   // ?group=&start=&end= (s)&stems=<mask>&slot=&sync=&mode=
        const int slot = arg("slot").toInt();
        const QJsonObject r = m_library.cutSample(arg("group"), arg("start").toDouble(), arg("end").toDouble(),
                arg("stems").toUInt(), slot, arg("sync") != QLatin1String("0"));
        if (r.value(QStringLiteral("ok")).toBool()) {
            setSamplerMode(slot, arg("mode").isEmpty() ? QStringLiteral("loop") : arg("mode"));
        }
        return result(r);
    }
    if (path == QLatin1String("/api/sampler/modes")) {
        return json(samplerModes());
    }
    if (path == QLatin1String("/api/sampler/mode")) {   // ?slot=1..64&mode=oneshot|hold|loop
        return result(setSamplerMode(arg("slot").toInt(), arg("mode")));
    }
    if (path == QLatin1String("/api/analyze/status")) {
        return json(m_library.analysisStatus());
    }
    if (path == QLatin1String("/api/analyze/stop")) {
        m_library.stopAnalysis();
        return json(QJsonObject{{"ok", true}});
    }
    if (path == QLatin1String("/api/scan/status")) {
        return json(QJsonObject{{"scanning", m_library.scanning()}});
    }
    return {404, "text/plain", "no such API\n", {}};
}

HttpServer::Response Hub::staticFile(const QString& name) const {
    QFile f(QDir(m_pConfig->getResourcePath()).filePath(QStringLiteral("zydek/web/") + name));
    if (!f.open(QIODevice::ReadOnly)) {
        return {404, "text/plain", "missing " + name.toUtf8() + "\n", {}};
    }
    return {200, contentType(name), f.readAll(), {{"Cache-Control", "no-store"}}};
}

/// "MXWF", visual rate (float32 LE), point count (uint32 LE), then all/low/mid/high per point
/// (the louder of left and right) — the format the controller page draws on the platter strip.
QByteArray Hub::waveform(int trackId) const {
    const auto tracks = PlayerInfo::instance().getLoadedTracks();
    for (const TrackPointer& pTrack : tracks) {
        if (!pTrack || pTrack->getId().toVariant().toInt() != trackId) {
            continue;
        }
        const ConstWaveformPointer pWaveform = pTrack->getWaveform();
        if (!pWaveform || pWaveform->getDataSize() < 2 ||
                pWaveform->getCompletion() < pWaveform->getDataSize()) {
            return {};   // still being analysed
        }
        const int points = pWaveform->getDataSize() / 2;
        const WaveformData* pData = pWaveform->data();
        QByteArray out(12 + points * 4, Qt::Uninitialized);
        memcpy(out.data(), "MXWF", 4);
        // Points per second of audio (441 for Mixxx's analysis; the rate itself isn't public).
        const double duration = pTrack->getDuration();
        const float rate = static_cast<float>(duration > 0 ? points / duration : 441.0);
        qToLittleEndian(rate, out.data() + 4);
        qToLittleEndian(static_cast<quint32>(points), out.data() + 8);
        auto* p = reinterpret_cast<unsigned char*>(out.data() + 12);
        for (int i = 0; i < points; ++i) {
            const WaveformFilteredData& l = pData[2 * i].filtered;
            const WaveformFilteredData& r = pData[2 * i + 1].filtered;
            p[4 * i] = std::max(l.all, r.all);
            p[4 * i + 1] = std::max(l.low, r.low);
            p[4 * i + 2] = std::max(l.mid, r.mid);
            p[4 * i + 3] = std::max(l.high, r.high);
        }
        return out;
    }
    return {};
}

QString Hub::lanUrl() const {
    // Prefer WiFi; the tablet joins the phone's network or the phone's hotspot.
    QString fallback;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) || (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            const QString url = QStringLiteral("http://%1:%2").arg(ip.toString()).arg(kPort);
            if (iface.name().startsWith(QLatin1String("wlan")) || iface.name().startsWith(QLatin1String("ap"))) {
                return url;
            }
            if (fallback.isEmpty()) {
                fallback = url;
            }
        }
    }
    return fallback.isEmpty() ? QStringLiteral("http://127.0.0.1:%1").arg(kPort) : fallback;
}

QJsonArray Hub::lanUrls() const {
    // Android names the Wi-Fi it joins wlan0; its own hotspot gets another wlan, ap or swlan interface.
    QJsonArray wifi, hotspot;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) || (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const QString name = iface.name();
        const bool isWifi = name == QLatin1String("wlan0");
        const bool isHotspot = !isWifi &&
                (name.startsWith(QLatin1String("wlan")) || name.startsWith(QLatin1String("ap")) ||
                        name.startsWith(QLatin1String("swlan")));
        if (!isWifi && !isHotspot) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            const QJsonObject item{{"label", isWifi ? "Wi-Fi" : "Hotspot"},
                    {"url", QStringLiteral("http://%1:%2").arg(entry.ip().toString()).arg(kPort)}};
            (isWifi ? wifi : hotspot).append(item);
        }
    }
    for (const QJsonValue& v : std::as_const(hotspot)) {
        wifi.append(v);
    }
    return wifi;
}

// ---- WebSockets -----------------------------------------------------------------------------------

void Hub::onWsOpened(QTcpSocket* pClient, const QString& path, const HttpServer::Query& query) {
    if (path == QLatin1String("/ws")) {
        m_server.sendText(pClient, QJsonDocument(snapshot()).toJson(QJsonDocument::Compact));
    } else if (path == QLatin1String("/ctl")) {
        m_ctlRoles.insert(pClient, query.value(QStringLiteral("role"), QStringLiteral("controller")));
        sendCtl(pClient, {{"type", "settings"}, {"settings", m_ctlSettings}});
        broadcastPresence();
    }
}

void Hub::onWsClosed(QTcpSocket* pClient, const QString& path) {
    for (auto it = m_pings.begin(); it != m_pings.end();) {
        it = it->pClient == pClient ? m_pings.erase(it) : std::next(it);
    }
    if (path == QLatin1String("/ctl")) {
        m_ctlRoles.remove(pClient);
        broadcastPresence();
    }
}

void Hub::onWsText(QTcpSocket* pClient, const QString& path, const QByteArray& text) {
    if (path == QLatin1String("/ws")) {
        if (text.startsWith("{\"ping\"")) {
            ping(pClient, QJsonDocument::fromJson(text).object().value(QStringLiteral("ping")).toArray());
            return;
        }
        handlePageMessage(text);
        return;
    }
    if (path != QLatin1String("/ctl")) {
        return;
    }
    const QJsonObject m = QJsonDocument::fromJson(text).object();
    const QString type = m.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("settings") && m.value(QStringLiteral("settings")).isObject()) {
        const QJsonObject changed = m.value(QStringLiteral("settings")).toObject();
        for (auto it = changed.begin(); it != changed.end(); ++it) {
            m_ctlSettings.insert(it.key(), it.value());
        }
        saveSettings();
        for (auto it = m_ctlRoles.cbegin(); it != m_ctlRoles.cend(); ++it) {
            if (it.key() != pClient) {
                sendCtl(it.key(), {{"type", "settings"}, {"settings", m_ctlSettings}});
            }
        }
    } else if (type == QLatin1String("latency")) {
        logLatency(m);
    } else if (type == QLatin1String("telemetry")) {
        for (auto it = m_ctlRoles.cbegin(); it != m_ctlRoles.cend(); ++it) {
            if (it.value() == QLatin1String("debug")) {
                m_server.sendText(it.key(), text);
            }
        }
    }
}

void Hub::sendCtl(QTcpSocket* pClient, const QJsonObject& message) {
    m_server.sendText(pClient, QJsonDocument(message).toJson(QJsonDocument::Compact));
}

void Hub::broadcastPresence() {
    int controllers = 0;
    for (const QString& role : std::as_const(m_ctlRoles)) {
        controllers += role == QLatin1String("controller");
    }
    for (auto it = m_ctlRoles.cbegin(); it != m_ctlRoles.cend(); ++it) {
        if (it.value() == QLatin1String("debug")) {
            sendCtl(it.key(), {{"type", "presence"}, {"controllers", controllers}});
        }
    }
}

void Hub::saveSettings() const {
    QFile f(QDir(m_pConfig->getSettingsPath()).filePath(kSettingsFile));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(m_ctlSettings).toJson());
    }
}

/// The track's beats, {"beats": [seconds...], "bar": k}: beat i starts a bar when (i + k) % 4 == 0 (Mixxx
/// counts 4/4 bars from the first beat marker). Empty while the track hasn't been analysed.
QByteArray Hub::beats(int trackId) const {
    const auto tracks = PlayerInfo::instance().getLoadedTracks();
    for (const TrackPointer& pTrack : tracks) {
        if (!pTrack || pTrack->getId().toVariant().toInt() != trackId) {
            continue;
        }
        const mixxx::BeatsPointer pBeats = pTrack->getBeats();
        const double rate = pTrack->getSampleRate().toDouble();
        if (!pBeats || rate <= 0) {
            return {};
        }
        const double endFrame = pTrack->getDuration() * rate;
        auto it = pBeats->iteratorFrom(mixxx::audio::kStartFramePos);
        const int firstIndex = static_cast<int>(it - pBeats->cfirstmarker());
        QJsonArray times;
        for (int n = 0; n < 20000 && it->value() < endFrame; ++n, ++it) {
            times.append(std::round(it->value() / rate * 10000.0) / 10000.0);
        }
        return QJsonDocument(QJsonObject{{"beats", times}, {"bar", ((firstIndex % 4) + 4) % 4}, {"locked", pTrack->isBpmLocked()}})
                .toJson(QJsonDocument::Compact);
    }
    return {};
}

// ---- sampler pad modes ---------------------------------------------------------------------------

QJsonObject Hub::samplerModes() {
    if (m_samplerModes.isEmpty()) {
        QFile f(QDir(m_pConfig->getSettingsPath()).filePath(kSamplersFile));
        const QJsonArray saved = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("modes")).toArray()
                                                            : QJsonArray();
        for (int i = 0; i < kSamplerSlots; ++i) {
            m_samplerModes.append(saved.at(i).toString(QStringLiteral("oneshot")));
        }
    }
    QJsonArray names;   // what's on the 32 pads ZyDeck shows ("" = empty)
    for (int i = 0; i < kKitSlots; ++i) {
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(QStringLiteral("[Sampler%1]").arg(i + 1));
        names.append(pTrack ? (pTrack->getTitle().isEmpty() ? QFileInfo(pTrack->getLocation()).completeBaseName() : pTrack->getTitle()) : QString());
    }
    return {{"modes", QJsonArray::fromStringList(m_samplerModes)}, {"names", names}};
}

QJsonObject Hub::setSamplerMode(int slot, const QString& mode) {
    if (slot < 1 || slot > kSamplerSlots || !QStringList{QStringLiteral("oneshot"), QStringLiteral("hold"), QStringLiteral("loop")}.contains(mode)) {
        return {{"ok", false}, {"error", "No such sampler or mode"}};
    }
    samplerModes();
    m_samplerModes[slot - 1] = mode;
    if (m_lastSessionRestored) {
        m_lastSessionTimer.start();
    }
    QSaveFile f(QDir(m_pConfig->getSettingsPath()).filePath(kSamplersFile));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{"modes", QJsonArray::fromStringList(m_samplerModes)}}).toJson(QJsonDocument::Compact));
        f.commit();
    }
    // a looping pad repeats its clip; the others play it once
    setControl(QStringLiteral("[Sampler%1]").arg(slot), QStringLiteral("repeat"), mode == QLatin1String("loop") ? 1 : 0);
    emitJson({{"t", "smode"}, {"i", slot - 1}, {"v", mode}});   // every page shows it
    return {{"ok", true}, {"slot", slot}, {"mode", mode}};
}

// ---- Last session --------------------------------------------------------------------------------

void Hub::restoreLastSession(int tries) {
    // wait for Mixxx's samplers (the controller mapping asks for 64 of them when it starts)
    if (ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_samplers"))) < kKitSlots && tries < 12) {
        QTimer::singleShot(2500, this, [this, tries] { restoreLastSession(tries + 1); });
        return;
    }
    bool anyLoaded = false;
    for (int i = 0; i < kKitSlots && !anyLoaded; ++i) {
        anyLoaded = static_cast<bool>(PlayerInfo::instance().getTrackInfo(QStringLiteral("[Sampler%1]").arg(i + 1)));
    }
    if (!anyLoaded) {
        loadKit(kLastSession);   // nothing if there's no such kit yet
    }
    QTimer::singleShot(4000, this, [this] { m_lastSessionRestored = true; });   // after the loads have landed
}

// ---- sampler kits ----------------------------------------------------------------------------------

namespace {

QJsonObject readKits(const QString& settingsPath) {
    QFile f(QDir(settingsPath).filePath(kKitsFile));
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

void writeKits(const QString& settingsPath, const QJsonObject& kits) {
    QSaveFile f(QDir(settingsPath).filePath(kKitsFile));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(kits).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

} // namespace

QJsonObject Hub::kits() const {
    const QJsonObject all = readKits(m_pConfig->getSettingsPath());
    QJsonArray out;
    for (auto it = all.begin(); it != all.end(); ++it) {
        int pads = 0, missing = 0;
        for (const QJsonValue& slot : it.value().toObject().value(QStringLiteral("slots")).toArray()) {
            const QString path = slot.toObject().value(QStringLiteral("path")).toString();
            if (!path.isEmpty()) {
                ++pads;
                missing += !QFileInfo::exists(path);
            }
        }
        out.append(QJsonObject{{"name", it.key()}, {"pads", pads}, {"missing", missing},
                {"saved", it.value().toObject().value(QStringLiteral("saved"))}});
    }
    return {{"kits", out}};
}

QJsonObject Hub::saveKit(const QString& name) {
    const QString kit = name.trimmed();
    if (kit.isEmpty()) {
        return {{"ok", false}, {"error", "Give the kit a name"}};
    }
    samplerModes();
    QJsonArray padList;
    int pads = 0;
    for (int i = 0; i < kKitSlots; ++i) {
        const QString group = QStringLiteral("[Sampler%1]").arg(i + 1);
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
        if (!pTrack) {
            padList.append(QJsonValue());
            continue;
        }
        ++pads;
        padList.append(QJsonObject{{"path", pTrack->getLocation()},
                {"mode", m_samplerModes.value(i, QStringLiteral("oneshot"))},
                {"sync", ControlObject::get(ConfigKey(group, QStringLiteral("sync_enabled"))) > 0},
                {"keylock", ControlObject::get(ConfigKey(group, QStringLiteral("keylock"))) > 0}});
    }
    QJsonObject all = readKits(m_pConfig->getSettingsPath());
    all.insert(kit, QJsonObject{{"slots", padList}, {"saved", QDateTime::currentDateTime().toString(Qt::ISODate)}});
    writeKits(m_pConfig->getSettingsPath(), all);
    return {{"ok", true}, {"name", kit}, {"pads", pads}};
}

QJsonObject Hub::loadKit(const QString& name) {
    const QJsonObject kit = readKits(m_pConfig->getSettingsPath()).value(name).toObject();
    if (kit.isEmpty()) {
        return {{"ok", false}, {"error", "No such kit"}};
    }
    const QJsonArray padList = kit.value(QStringLiteral("slots")).toArray();
    int loaded = 0, missing = 0;
    for (int i = 0; i < kKitSlots; ++i) {
        const QString group = QStringLiteral("[Sampler%1]").arg(i + 1);
        const QJsonObject slot = padList.at(i).toObject();
        const QString path = slot.value(QStringLiteral("path")).toString();
        if (path.isEmpty() || !QFileInfo::exists(path)) {   // empty in the kit (or the file's gone): empty here
            missing += !path.isEmpty();
            if (PlayerInfo::instance().getTrackInfo(group)) {
                ControlObject::set(ConfigKey(group, QStringLiteral("eject")), 1);
                ControlObject::set(ConfigKey(group, QStringLiteral("eject")), 0);
            }
            continue;
        }
        const QString mode = slot.value(QStringLiteral("mode")).toString(QStringLiteral("oneshot"));
        if (m_library.loadLocation(path, QStringLiteral("sampler%1").arg(i + 1)).value(QStringLiteral("ok")).toBool()) {
            ++loaded;
            Library::setSamplerOptions(group, slot.value(QStringLiteral("sync")).toBool(), mode == QLatin1String("loop"),
                    slot.value(QStringLiteral("keylock")).toBool(true));   // kits from before had it on
            setSamplerMode(i + 1, mode);
        }
    }
    return {{"ok", true}, {"name", name}, {"pads", loaded}, {"missing", missing}};
}

QJsonObject Hub::deleteKit(const QString& name) {
    QJsonObject all = readKits(m_pConfig->getSettingsPath());
    if (!all.contains(name)) {
        return {{"ok", false}, {"error", "No such kit"}};
    }
    all.remove(name);
    writeKits(m_pConfig->getSettingsPath(), all);
    return {{"ok", true}};
}

// ---- audio outputs ---------------------------------------------------------------------------------
// Mixxx's SoundManager belongs to the main thread: these wait for it there (both are quick).

QJsonObject Hub::audioStatus() const {
    const std::shared_ptr<SoundManager> pManager = mixxx::qml::QmlSoundManagerProxy::registeredManager();
    if (!pManager) {
        return {{"ok", false}, {"error", "Mixxx isn't ready"}};
    }
    QJsonObject out;
    QMetaObject::invokeMethod(
            pManager.get(),
            [&out, &pManager] {
                const SoundManagerConfig config = pManager->getConfig();
                const QList<SoundDevicePointer> devices = pManager->getDeviceList(config.getAPI(), true, false);
                QJsonArray list;
                QString main, headphones, mainChannels, headphonesChannels;
                // "base,count": 0-based first channel and 1 or 2 channels ("0,2" = 1-2, "1,1" = 2 alone)
                const auto channels = [](const AudioOutput& out) {
                    const ChannelGroup group = out.getChannelGroup();
                    return QStringLiteral("%1,%2").arg(static_cast<int>(group.getChannelBase())).arg(static_cast<int>(group.getChannelCount()));
                };
                for (const SoundDevicePointer& pDevice : devices) {
                    list.append(QJsonObject{{"name", pDevice->getDisplayName()},
                            {"channels", static_cast<int>(pDevice->getNumOutputChannels())}});
                }
                const auto outputs = config.getOutputs();
                for (auto it = outputs.cbegin(); it != outputs.cend(); ++it) {
                    for (const SoundDevicePointer& pDevice : devices) {
                        if (pDevice->getDeviceId() != it.key()) {
                            continue;
                        }
                        if (it.value().getType() == AudioPathType::Main) {
                            main = pDevice->getDisplayName();
                            mainChannels = channels(it.value());
                        } else if (it.value().getType() == AudioPathType::Headphones) {
                            headphones = pDevice->getDisplayName();
                            headphonesChannels = channels(it.value());
                        }
                    }
                }
                out = {{"ok", true},
                        {"api", config.getAPI()},
                        {"devices", list},
                        {"main", main},
                        {"mainChannels", mainChannels},
                        {"headphones", headphones},
                        {"headphonesChannels", headphonesChannels}};
            },
            Qt::BlockingQueuedConnection);
    out.insert(QStringLiteral("latencyMs"),
            ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("output_latency_ms"))));
    return out;
}

QJsonObject Hub::setAudio(const HttpServer::Query& query) {
    const std::shared_ptr<SoundManager> pManager = mixxx::qml::QmlSoundManagerProxy::registeredManager();
    if (!pManager) {
        return {{"ok", false}, {"error", "Mixxx isn't ready"}};
    }
    const QString mainName = query.value(QStringLiteral("main"));
    const QString headphonesName = query.value(QStringLiteral("headphones"));
    // "base,count" as audioStatus() gives them; main defaults to 1-2, the headphones to 3-4 on the main's device
    const auto parseChannels = [](const QString& text, int base, int count) {
        const QStringList parts = text.split(QLatin1Char(','));
        if (parts.size() == 2) {
            base = parts[0].toInt();
            count = parts[1].toInt() == 1 ? 1 : 2;
        }
        return std::pair<int, int>(qBound(0, base, 31), count);
    };
    const std::pair<int, int> mainChannels = parseChannels(query.value(QStringLiteral("mainChannels")), 0, 2);
    const int mainBase = mainChannels.first;
    const int mainCount = mainChannels.second;
    const QString headphonesChannels = query.value(QStringLiteral("headphonesChannels"));
    QJsonObject out;
    QMetaObject::invokeMethod(
            pManager.get(),
            [&] {
                SoundManagerConfig config = pManager->getConfig();
                const QList<SoundDevicePointer> devices = pManager->getDeviceList(config.getAPI(), true, false);
                const auto find = [&devices](const QString& name) {
                    for (const SoundDevicePointer& pDevice : devices) {
                        if (pDevice->getDisplayName() == name) {
                            return pDevice;
                        }
                    }
                    return SoundDevicePointer();
                };
                const SoundDevicePointer pMain = find(mainName);
                const SoundDevicePointer pHeadphones = headphonesName.isEmpty() ? SoundDevicePointer() : find(headphonesName);
                if (!pMain || (!headphonesName.isEmpty() && !pHeadphones)) {
                    out = {{"ok", false}, {"error", QStringLiteral("That output isn't connected any more")}};
                    return;
                }
#ifdef Q_OS_ANDROID
                // Android plays an app's media to one output at a time: two outputs (speaker and Bluetooth, say)
                // take turns, each one cutting the other off many times a second.
                if (pHeadphones && pHeadphones != pMain) {
                    out = {{"ok", false},
                            {"error", QStringLiteral("Android plays to one device at a time: put the headphones on the main's "
                                                     "device, on other channels (a USB interface's 3-4, or main on 1 "
                                                     "left and headphones on 2 right)")}};
                    return;
                }
#endif
                const std::pair<int, int> phonesChannels = parseChannels(headphonesChannels,
                        pHeadphones == pMain && pMain->getNumOutputChannels() >= 4 ? 2 : 0, 2);
                const int phonesBase = phonesChannels.first;
                const int phonesCount = phonesChannels.second;
                const auto fits = [](const SoundDevicePointer& pDevice, int base, int count) {
                    return base + count <= static_cast<int>(pDevice->getNumOutputChannels());
                };
                if (!fits(pMain, mainBase, mainCount) || (pHeadphones && !fits(pHeadphones, phonesBase, phonesCount))) {
                    out = {{"ok", false}, {"error", QStringLiteral("That output doesn't have those channels")}};
                    return;
                }
                // Main and headphones may share channels: Mixxx then mixes the cue into the main mix there
                // (the page warns about it first).
                const bool shared = pHeadphones == pMain &&
                        mainBase < phonesBase + phonesCount && phonesBase < mainBase + mainCount;
                QMultiHash<SoundDeviceId, AudioOutput>& outputs = config.getOutputsRef();
                for (auto it = outputs.begin(); it != outputs.end();) {
                    const AudioPathType type = it.value().getType();
                    it = type == AudioPathType::Main || type == AudioPathType::Headphones ? outputs.erase(it) : std::next(it);
                }
                config.addOutput(pMain->getDeviceId(),
                        AudioOutput(AudioPathType::Main,
                                static_cast<unsigned char>(mainBase),
                                mixxx::audio::ChannelCount(mainCount),
                                0));
                if (pHeadphones) {
                    config.addOutput(pHeadphones->getDeviceId(),
                            AudioOutput(AudioPathType::Headphones,
                                    static_cast<unsigned char>(phonesBase),
                                    mixxx::audio::ChannelCount(phonesCount),
                                    0));
                }
                const SoundDeviceStatus status = pManager->setConfig(config);
                out = status == SoundDeviceStatus::Ok
                        ? QJsonObject{{"ok", true}, {"shared", shared}}
                        : QJsonObject{{"ok", false}, {"error", pManager->getLastErrorMessage(status)}};
            },
            Qt::BlockingQueuedConnection);
    return out;
}

// ---- latency log ---------------------------------------------------------------------------------

void Hub::ping(QTcpSocket* pClient, const QJsonArray& ping) {   // [seq, page's performance.now()]
    const int seq = ping.at(0).toInt() & 0x1FFFFF;
    m_pings.insert(seq, Ping{pClient, ping.at(1).toDouble(), m_clock.elapsed()});
    if (!canSend()) {
        pong(seq, false);
        return;
    }
    // Through the mapping, the way the page's controls go (it answers synchronously, on this thread)
    sysex({kPing, seq & 0x7F, (seq >> 7) & 0x7F, (seq >> 14) & 0x7F});
    if (m_pings.contains(seq)) {   // the mapping didn't echo (an older mapping): answer directly
        pong(seq, false);
    }
}

void Hub::pong(int seq, bool viaMapping) {
    const auto it = m_pings.constFind(seq);
    if (it == m_pings.constEnd()) {
        return;
    }
    // c: the page's send time · h: hub clock when it arrived (ms) · j: time in the mapping (ms)
    const qint64 now = m_clock.elapsed();
    m_server.sendText(it->pClient,
            QJsonDocument(QJsonObject{{"t", "pong"},
                                  {"s", seq},
                                  {"c", it->clientMs},
                                  {"h", static_cast<double>(it->hubMs)},
                                  {"j", static_cast<double>(now - it->hubMs)},
                                  {"m", viaMapping}})
                    .toJson(QJsonDocument::Compact));
    m_pings.erase(it);
}

void Hub::logLatency(const QJsonObject& summary) {
    QJsonObject line = summary;
    line.remove(QStringLiteral("type"));
    line.insert(QStringLiteral("at"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")));
    line.insert(QStringLiteral("hub"),
            QJsonObject{{"lagMax", static_cast<double>(m_lagMaxMs)},
                    {"slow", m_slowRequests},
                    {"pending", m_pings.size()},
                    {"audioMs", ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("output_latency_ms")))},
                    {"audioLoad", ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("audio_latency_usage")))},
                    {"xruns", ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("audio_latency_overload_count")))}});
    m_lagMaxMs = 0;
    m_slowRequests = QJsonArray();

    const QString path = QDir(m_pConfig->getSettingsPath()).filePath(kLatencyFile);
    if (QFileInfo(path).size() > kLatencyFileMax) {
        QFile::remove(path + QStringLiteral(".1"));
        QFile::rename(path, path + QStringLiteral(".1"));
    }
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        f.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
    }
}

HttpServer::Response Hub::latencyLog(const HttpServer::Query& query) const {
    const QString path = QDir(m_pConfig->getSettingsPath()).filePath(kLatencyFile);
    if (query.contains(QStringLiteral("clear"))) {
        QFile::remove(path);
        QFile::remove(path + QStringLiteral(".1"));
        return {200, "text/plain", "cleared\n", {}};
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {200, "text/plain", "", {}};
    }
    const int lines = query.value(QStringLiteral("lines"), QStringLiteral("300")).toInt();
    QList<QByteArray> all = f.readAll().split('\n');
    if (!all.isEmpty() && all.last().isEmpty()) {
        all.removeLast();
    }
    const QList<QByteArray> tail = all.mid(std::max<qsizetype>(0, all.size() - lines));
    return {200, "application/x-ndjson", tail.join('\n') + '\n', {}};
}

// ---- page -> Mixxx --------------------------------------------------------------------------------

void Hub::handlePageMessage(const QByteArray& text) {
    if (!canSend()) {
        return;
    }
    if (!text.startsWith('{')) {   // "d|u <channel> <note>": a pad press
        const QList<QByteArray> parts = text.split(' ');
        if (parts.size() == 3) {
            press(parts[1].toInt(), parts[2].toInt(), parts[0] == "d");
        }
        return;
    }
    const QJsonObject m = QJsonDocument::fromJson(text).object();
    if (m.contains(QStringLiteral("set"))) {   // [group, key, value, param?]
        const QJsonArray a = m.value(QStringLiteral("set")).toArray();
        QList<int> parts{kSet, a.size() > 3 && (a[3].toBool() || a[3].toInt()) ? 1 : 0};
        parts += encodeName(a[0].toString(), a[1].toString());
        parts.append(0);
        parts += encodeValue(a[2].toDouble());
        sysex(parts);
    } else if (m.contains(QStringLiteral("sub"))) {   // [[group, key, kind], ...]
        const QJsonArray subs = m.value(QStringLiteral("sub")).toArray();
        for (const QJsonValue& v : subs) {
            const QJsonArray s = v.toArray();
            const QString group = s[0].toString();
            const QString key = s[1].toString();
            const int kind = s[2].toInt();
            m_subscriptions.insert(group + QLatin1Char(',') + key, kind);
            sysex(QList<int>{kSubscribe, kind} + encodeName(group, key));
        }
    } else if (m.contains(QStringLiteral("scratch"))) {   // [deck 0-3, op, ticks]
        const QJsonArray a = m.value(QStringLiteral("scratch")).toArray();
        const int n = a[2].toInt() + (1 << 20);
        sysex({kScratch, a[0].toInt(), a[1].toInt(), n & 0x7F, (n >> 7) & 0x7F, (n >> 14) & 0x7F});
    } else if (m.contains(QStringLiteral("tempo"))) {   // [deck 0-3, overlimit, percent]
        const QJsonArray a = m.value(QStringLiteral("tempo")).toArray();
        sysex(QList<int>{kTempo, a[0].toInt(), a[1].toBool() || a[1].toInt() ? 1 : 0} +
                encodeValue(a[2].toDouble()));
    } else if (m.contains(QStringLiteral("sync"))) {   // [deck 0-3, mode]
        const QJsonArray a = m.value(QStringLiteral("sync")).toArray();
        sysex({kSync, a[0].toInt(), a[1].toInt()});
    }
}

void Hub::sysex(const QList<int>& parts) {
    QByteArray data;
    data.reserve(parts.size() + 3);
    data.append(static_cast<char>(0xF0));
    data.append(static_cast<char>(0x7D));
    for (int p : parts) {
        data.append(static_cast<char>(p & 0x7F));
    }
    data.append(static_cast<char>(0xF7));
    sendMidi(data);
}

void Hub::press(int channel, int note, bool down) {
    // channel 0 = samplers, 1-4 = decks 1-4
    if (channel >= 0 && channel <= kNumDecks && note >= 0 && note < 128) {
        const char msg[3] = {static_cast<char>(0x90 | channel), static_cast<char>(note), static_cast<char>(down ? 127 : 0)};
        sendMidi(QByteArray(msg, 3));
    }
}

/// To the mapping: here (the Zydek controller in this Mixxx), or on the computer in controller mode.
void Hub::sendMidi(const QByteArray& data) {
    if (data.isEmpty()) {
        return;
    }
    if (!m_controllerMode) {
        if (!m_pController) {
            return;
        }
        if (static_cast<unsigned char>(data[0]) == 0xF0) {
            m_pController->injectSysex(data);
        } else if (data.size() == 3) {
            m_pController->injectShortMessage(static_cast<unsigned char>(data[0]),
                    static_cast<unsigned char>(data[1]),
                    static_cast<unsigned char>(data[2]));
        }
        return;
    }
    if (m_linkUp && m_pLink) {
        m_pLink->write("M " + data.toHex() + '\n');
        return;
    }
#ifdef Q_OS_ANDROID
    if (m_usbState == 2) {
        QJniEnvironment env;
        jbyteArray array = env->NewByteArray(data.size());
        env->SetByteArrayRegion(array, 0, data.size(), reinterpret_cast<const jbyte*>(data.constData()));
        QJniObject::callStaticMethod<jboolean>("org/mixxx/ZydekMidi", "send", "([B)Z", array);
        env->DeleteLocalRef(array);
    }
#endif
}

void Hub::setControl(const QString& group, const QString& key, double value) {
    if (!m_controllerMode) {
        ControlObject::set(ConfigKey(group, key), value);
        return;
    }
    QList<int> parts{kSet, 0};
    parts += encodeName(group, key);
    parts.append(0);
    parts += encodeValue(value);
    sysex(parts);
}

// ---- controller mode ------------------------------------------------------------------------------

template<typename F>
void Hub::MidiParser::feed(const QByteArray& data, F&& onMessage) {
    for (const char c : data) {
        const auto b = static_cast<unsigned char>(c);
        if (b >= 0xF8) {
            continue;   // real-time (clock...): not ours
        }
        if (b == 0xF0) {
            sysex = true;
            message = QByteArray(1, c);
            continue;
        }
        if (sysex) {
            if (b == 0xF7) {
                message.append(c);
                sysex = false;
                onMessage(message);
                message.clear();
            } else if (b & 0x80) {
                sysex = false;   // a broken SysEx: drop it, start over with this status byte
                message.clear();
            } else {
                message.append(c);
                continue;
            }
            if (!(b & 0x80) || b == 0xF7) {
                continue;
            }
        }
        if (b & 0x80) {
            status = b < 0xF0 ? b : 0;
            message = QByteArray(1, c);
            continue;
        }
        if (!status) {
            continue;
        }
        if (message.isEmpty()) {
            message = QByteArray(1, static_cast<char>(status));   // running status
        }
        message.append(c);
        const int need = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
        if (message.size() == need) {
            onMessage(message);
            message.clear();
        }
    }
}

void Hub::usbMidiReceived(const QByteArray& data) {
    m_usbParser.feed(data, [this](const QByteArray& msg) { remoteMessage(msg, true); });
}

void Hub::usbMidiState(int state) {
    if (state == m_usbState) {
        return;
    }
    const bool wasReady = m_usbState == 2;
    m_usbState = state;
    if (state != 2) {
        m_usbParser = MidiParser();
        if (!m_linkUp) {
            m_remoteLive = false;
        }
        if (state == 0) {
            m_linkOffer = {};   // a new cable, a new offer
            m_offerDeclined = false;
        }
    } else if (!wasReady && !m_linkUp) {
        m_helloMs = -100000;   // say hello now
        resendSubscriptions();
    }
    emitRemote();
    remoteTick();
}

void Hub::setControllerMode(bool on) {
    if (on == m_controllerMode) {
        return;
    }
    m_controllerMode = on;
    saveRemote();
#ifdef Q_OS_ANDROID
    QJniObject::callStaticMethod<void>("org/mixxx/ZydekMidi", "setEnabled", "(Z)V", static_cast<jboolean>(on));
#endif
    if (on) {
        m_remoteTimer.start();
    } else {
        m_remoteTimer.stop();
        linkDrop(false);
        m_usbState = 0;
        m_linkOffer = {};
    }
    m_remoteLive = false;
    resetMixxxState();
    if (on) {   // each deck's and pad's track length: ZyDeck Link finds its name from it
        for (int i = 1; i <= kNumDecks; ++i) {
            m_subscriptions.insert(QStringLiteral("[Channel%1],duration").arg(i), 0);
        }
        for (int i = 1; i <= kKitSlots; ++i) {
            m_subscriptions.insert(QStringLiteral("[Sampler%1],duration").arg(i), 0);
        }
    }
    resendSubscriptions();
    if (!on) {   // back to this Mixxx: the pages get its names again
        for (int i = 0; i < kNumDecks; ++i) {
            updateName(false, i, 0);
        }
        for (int i = 0; i < kNumSamplers; ++i) {
            updateName(true, i, 0);
        }
    }
    m_server.broadcast(QStringLiteral("/ws"), QJsonDocument(snapshot()).toJson(QJsonDocument::Compact));
}

/// Forget what the pages were shown (the other Mixxx's decks): the mapping fills it in again.
void Hub::resetMixxxState() {
    std::fill(std::begin(m_samplers), std::end(m_samplers), 0);
    for (QJsonValue& n : m_samplerNames) {
        n = QJsonValue();
    }
    for (Deck& d : m_decks) {
        d = Deck();
    }
    for (auto& unit : m_fx) {
        std::fill(std::begin(unit), std::end(unit), 0);
    }
    m_controlValues = {};
}

void Hub::resendSubscriptions() {
    for (auto it = m_subscriptions.cbegin(); it != m_subscriptions.cend(); ++it) {
        const int comma = it.key().indexOf(QLatin1Char(','));
        sysex(QList<int>{kSubscribe, it.value()} + encodeName(it.key().left(comma), it.key().mid(comma + 1)));
    }
}

bool Hub::canSend() const {
    return m_controllerMode ? (m_linkUp || m_usbState == 2) : m_pController != nullptr;
}

bool Hub::live() const {
    return m_controllerMode ? m_remoteLive && canSend() : m_pController != nullptr;
}

QJsonObject Hub::remoteStatus() const {
    QJsonObject out{{"on", m_controllerMode},
            {"usb", m_usbState},
            {"via", !m_controllerMode ? "" : m_linkUp ? "wifi" : m_usbState == 2 ? "usb" : ""},
            {"live", live()},
            {"linking", m_pLink != nullptr && !m_linkUp},
            {"saved", m_linkSaved.value(QStringLiteral("name"))}};
    const QJsonObject& computer = m_linkUp ? m_linkTarget : m_linkOffer;
    if (!computer.isEmpty()) {
        const QJsonArray hosts = computer.value(QStringLiteral("hosts")).toArray();
        const QString host = m_linkUp ? m_linkTarget.value(QStringLiteral("host")).toString()
                                      : hosts.isEmpty() ? QString() : hosts.first().toString();
        out.insert(QStringLiteral("computer"), computer.value(QStringLiteral("name")));
        // waveforms and beat grids come straight from ZyDeck Link (pages add ?k=<key>)
        if (!host.isEmpty()) {
            out.insert(QStringLiteral("http"),
                    QStringLiteral("http://%1:%2").arg(host).arg(computer.value(QStringLiteral("port")).toInt()));
            out.insert(QStringLiteral("key"), computer.value(QStringLiteral("key")));
        }
    }
    // ask the pages to offer the wireless link
    out.insert(QStringLiteral("offer"), !m_linkUp && !m_offerDeclined && !m_linkOffer.isEmpty() && !m_pLink);
    return out;
}

void Hub::emitRemote() {
    QJsonObject m = remoteStatus();
    m.insert(QStringLiteral("t"), QStringLiteral("remote"));
    emitJson(m);
}

void Hub::sendSysexText(int type, const QByteArray& text) {
    QList<int> parts{type};
    for (const char c : text.toHex()) {
        parts.append(c);
    }
    sysex(parts);
}

/// From the computer: the mapping's MIDI/SysEx, or ZyDeck Link's answers.
void Hub::remoteMessage(const QByteArray& msg, bool viaUsb) {
    if (!m_controllerMode) {
        return;
    }
    const auto byte = [&msg](int i) { return static_cast<unsigned char>(msg[i]); };
    if (msg.size() >= 4 && byte(0) == 0xF0 && byte(1) == 0x7D && (byte(2) == kLinkOffer || byte(2) == kNameReply)) {
        const QJsonObject j = QJsonDocument::fromJson(QByteArray::fromHex(msg.mid(3, msg.size() - 4))).object();
        if (byte(2) == kLinkOffer) {
            const bool fresh = m_linkOffer.value(QStringLiteral("key")) != j.value(QStringLiteral("key"));
            m_linkOffer = j;
            if (fresh) {
                emitRemote();
                // ZyDeck Link is there now: it can name what's loaded
                for (auto it = m_controlValues.constBegin(); it != m_controlValues.constEnd(); ++it) {
                    static const QRegularExpression slot(QStringLiteral(R"(^\[(Channel|Sampler)(\d+)\],duration$)"));
                    const QRegularExpressionMatch m = slot.match(it.key());
                    if (m.hasMatch()) {
                        updateName(m.captured(1) == QLatin1String("Sampler"), m.captured(2).toInt() - 1,
                                static_cast<int>(it.value().toDouble() * 1000));
                    }
                }
            }
        } else {
            applyName(j.value(QStringLiteral("s")).toBool(), j.value(QStringLiteral("i")).toInt(), j.value(QStringLiteral("n")));
        }
        return;
    }
    if (viaUsb && m_linkUp) {
        return;   // the same mapping's messages arrive over Wi-Fi now
    }
    m_remoteRxMs = m_clock.elapsed();
    if (!m_remoteLive) {
        m_remoteLive = true;
        emitRemote();
        m_server.broadcast(QStringLiteral("/ws"), QJsonDocument(snapshot()).toJson(QJsonDocument::Compact));
    }
    handleMixxx(msg);
}

/// Once a second in controller mode: hello over the cable until ZyDeck Link answers, check that the
/// mapping still answers, and try the computer from last time over Wi-Fi when there's no cable.
void Hub::remoteTick() {
    if (!m_controllerMode) {
        return;
    }
    const qint64 now = m_clock.elapsed();
    if (m_usbState == 2 && !m_linkUp && m_linkOffer.isEmpty() && now - m_helloMs > 3000) {
        m_helloMs = now;
        sendSysexText(kLinkHello,
                QJsonDocument(QJsonObject{{"v", 1}, {"name", deviceName()}}).toJson(QJsonDocument::Compact));
    }
    if (canSend() && now - m_remoteRxMs > 4000) {
        sysex({kPing, 0x7F, 0x7F, 0x7F});   // the mapping echoes it: still there?
        if (m_remoteLive && now - m_remoteRxMs > 10000) {
            m_remoteLive = false;   // Mixxx closed, or the mapping isn't on for this port
            emitRemote();
            m_server.broadcast(QStringLiteral("/ws"), QJsonDocument(snapshot()).toJson(QJsonDocument::Compact));
        }
    }
    if (!m_pLink && m_usbState != 2 && !m_linkSaved.isEmpty() && now - m_helloMs > 5000) {
        m_helloMs = now;
        linkConnect(m_linkSaved);
    }
}

void Hub::linkConnect(const QJsonObject& computer) {
    linkDrop(false);
    const QJsonArray hosts = computer.value(QStringLiteral("hosts")).toArray();
    if (hosts.isEmpty()) {
        return;
    }
    m_linkTarget = computer;
    m_linkHost = std::clamp(m_linkHost, 0, static_cast<int>(hosts.size()) - 1);
    const QString host = hosts.at(m_linkHost).toString();
    m_linkTarget.insert(QStringLiteral("host"), host);
    m_pLink = new QTcpSocket(this);
    m_linkBuffer.clear();
    m_linkParser = MidiParser();
    QTcpSocket* pLink = m_pLink;
    connect(pLink, &QTcpSocket::connected, this, [this, pLink] {
        pLink->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        pLink->write("ZYDECK " + m_linkTarget.value(QStringLiteral("key")).toString().toLatin1() + ' ' +
                deviceName().toUtf8().toHex() + '\n');
    });
    connect(pLink, &QTcpSocket::readyRead, this, [this, pLink] {
        if (pLink != m_pLink) {
            return;
        }
        m_linkBuffer += pLink->readAll();
        for (int nl; (nl = m_linkBuffer.indexOf('\n')) >= 0;) {
            const QByteArray line = m_linkBuffer.left(nl);
            m_linkBuffer.remove(0, nl + 1);
            linkLine(line);
            if (pLink != m_pLink) {
                return;
            }
        }
    });
    connect(pLink, &QTcpSocket::errorOccurred, this, [this, pLink] {
        if (pLink == m_pLink) {
            m_linkHost++;   // the next address next time
            linkDrop(true);
        }
    });
    connect(pLink, &QTcpSocket::disconnected, this, [this, pLink] {
        if (pLink == m_pLink) {
            linkDrop(true);
        }
    });
    pLink->connectToHost(host, static_cast<quint16>(computer.value(QStringLiteral("port")).toInt()));
    QTimer::singleShot(4000, pLink, [this, pLink] {
        if (pLink == m_pLink && !m_linkUp) {
            m_linkHost++;
            linkDrop(true);
        }
    });
    emitRemote();
}

void Hub::linkLine(const QByteArray& line) {
    if (line.startsWith("M ")) {
        m_linkParser.feed(QByteArray::fromHex(line.mid(2)), [this](const QByteArray& msg) { remoteMessage(msg, false); });
    } else if (line.startsWith("J ")) {
        const QJsonObject j = QJsonDocument::fromJson(line.mid(2)).object();
        if (j.contains(QStringLiteral("n"))) {
            applyName(j.value(QStringLiteral("s")).toBool(), j.value(QStringLiteral("i")).toInt(), j.value(QStringLiteral("n")));
        }
    } else if (line.startsWith("OK")) {
        m_linkUp = true;
        m_linkHost = 0;
        m_linkSaved = m_linkTarget;
        m_linkSaved.remove(QStringLiteral("host"));
        saveRemote();
        m_remoteLive = false;
        m_remoteRxMs = m_clock.elapsed();
        resendSubscriptions();   // to the mapping on ZyDeck Link's port (the durations bring the names)
        emitRemote();
    } else if (line.startsWith("NO")) {   // a wrong key: that computer forgot this device
        m_linkSaved = {};
        saveRemote();
        linkDrop(false);
    }
}

void Hub::linkDrop(bool retry) {
    const bool wasUp = m_linkUp;
    m_linkUp = false;
    if (m_pLink) {
        QTcpSocket* pLink = m_pLink;
        m_pLink = nullptr;
        pLink->disconnect(this);
        pLink->abort();
        pLink->deleteLater();
    }
    if (wasUp) {
        m_remoteLive = false;
        if (m_usbState == 2) {
            resendSubscriptions();   // back on the cable
        }
    }
    if (retry) {
        m_helloMs = m_clock.elapsed();   // remoteTick tries again in a few seconds
    }
    if (m_controllerMode) {
        emitRemote();
    }
}

QString Hub::deviceName() const {
#ifdef Q_OS_ANDROID
    const QString model = QJniObject::getStaticObjectField("android/os/Build", "MODEL", "Ljava/lang/String;").toString();
    if (!model.isEmpty()) {
        return model;
    }
#endif
    return QStringLiteral("ZyDeck");
}

void Hub::loadRemote() {
    QFile f(QDir(m_pConfig->getSettingsPath()).filePath(kRemoteFile));
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject j = QJsonDocument::fromJson(f.readAll()).object();
    m_linkSaved = j.value(QStringLiteral("computer")).toObject();
    if (j.value(QStringLiteral("on")).toBool()) {
        QTimer::singleShot(0, this, [this] { setControllerMode(true); });
    }
}

void Hub::saveRemote() const {
    QSaveFile f(QDir(m_pConfig->getSettingsPath()).filePath(kRemoteFile));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{"on", m_controllerMode}, {"computer", m_linkSaved}}).toJson());
        f.commit();
    }
}

// ---- Mixxx -> pages -------------------------------------------------------------------------------

void Hub::fromMixxx(const QByteArray& msg) {
    if (!m_controllerMode) {   // in controller mode the pages show the computer's Mixxx, not this one
        handleMixxx(msg);
    }
}

void Hub::handleMixxx(const QByteArray& msg) {
    auto byte = [&msg](int i) { return static_cast<unsigned char>(msg[i]); };
    if (msg.size() == 7 && byte(0) == 0xF0 && byte(1) == 0x7D && byte(2) == kPing) {
        pong(byte(3) | byte(4) << 7 | byte(5) << 14, true);
        return;
    }
    if (msg.size() >= 4 && byte(0) == 0xF0 && byte(1) == 0x7D &&
            (byte(2) == kValue || byte(2) == kHello)) {
        if (byte(2) == kHello) {   // the mapping (re)started: subscriptions must be made again
            resendSubscriptions();
            return;
        }
        const int end = msg.indexOf('\0', 3);
        if (end < 0) {
            return;
        }
        const QString name = QString::fromLatin1(msg.mid(3, end - 3));
        const double value = decodeValue(msg.mid(end + 1, kValueBytes));
        m_controlValues.insert(name, value);
        if (m_controllerMode && name.endsWith(QLatin1String("],duration"))) {
            static const QRegularExpression slot(QStringLiteral(R"(^\[(Channel|Sampler)(\d+)\],)"));
            const QRegularExpressionMatch m = slot.match(name);
            if (m.hasMatch()) {
                updateName(m.captured(1) == QLatin1String("Sampler"), m.captured(2).toInt() - 1, static_cast<int>(value * 1000));
            }
        }
        // ts: the hub's clock (ms) when Mixxx reported it, so pages can time positions without network jitter
        emitJson({{"t", "cv"}, {"k", name}, {"v", value}, {"ts", static_cast<double>(m_clock.elapsed())}});
        return;
    }
    if (msg.size() == 10 && byte(0) == 0xF0 && byte(1) == 0x7D) {
        const int kind = byte(2), idx = byte(3), sub = byte(4);
        const int v = byte(5) | byte(6) << 7 | byte(7) << 14 | byte(8) << 21;
        if (kind == kSamplerLength && idx < kNumSamplers) {
            updateName(true, idx, v);
        } else if (kind == kDeckLength && idx < kNumDecks) {
            updateName(false, idx, v);
        } else if (kind == kHotcue && idx < kNumDecks && sub < 8 && m_decks[idx].cues[sub] != v) {
            m_decks[idx].cues[sub] = v;   // 0 = no cue, otherwise RGB colour + 1
            emitJson({{"t", "cue"}, {"d", idx}, {"k", sub}, {"v", v}});
        }
        return;
    }
    if (msg.size() != 3 || (byte(0) & 0xF0) != 0x90) {
        return;
    }
    const int ch = byte(0) & 0x0F, note = byte(1), value = byte(2);
    if (ch == 0 && note < kNumSamplers) {
        if (m_samplers[note] != value) {
            m_samplers[note] = value;
            emitJson({{"t", "s"}, {"i", note}, {"v", value}});
        }
    } else if (ch == 2 && note < kNumDecks * 8) {
        int& roll = m_decks[note / 8].rolls[note % 8];
        if (roll != value) {
            roll = value;
            emitJson({{"t", "roll"}, {"d", note / 8}, {"k", note % 8}, {"v", value}});
        }
    } else if (ch == 3 && note < kNumDecks) {
        setDeckField(note, m_decks[note].v, value, "dv");
    } else if (ch == 4 && note < kNumDecks * 8 && note % 8 < 4) {
        int& stem = m_decks[note / 8].stems[note % 8];
        if (stem != value) {
            stem = value;
            emitJson({{"t", "stem"}, {"d", note / 8}, {"k", note % 8}, {"v", value}});
        }
    } else if (ch == 5 && note < kNumDecks) {
        setDeckField(note, m_decks[note].stemCount, value, "sc");
    } else if (ch == 7 && note < kNumDecks) {
        setDeckField(note, m_decks[note].pitch, value - 64, "p");
    } else if (ch == 6 && note < 32 && note % 8 < 7) {
        if (m_fx[note / 8][note % 8] != value) {
            m_fx[note / 8][note % 8] = value;
            emitJson({{"t", "fx"}, {"u", note / 8}, {"j", note % 8}, {"v", value}});
        }
    }
}

void Hub::setDeckField(int deck, int& field, int value, const char* kind) {
    if (field != value) {
        field = value;
        emitJson({{"t", QLatin1String(kind)}, {"d", deck}, {"v", value}});
    }
}

void Hub::updateName(bool sampler, int index, int lengthMs) {
    if (m_controllerMode) {
        // Only the computer knows what's loaded: ZyDeck Link looks the length up in its Mixxx library (with
        // no ZyDeck Link there, the pages show no names)
        if (lengthMs <= 0) {
            applyName(sampler, index, QJsonValue());
            return;
        }
        const QByteArray ask = QJsonDocument(QJsonObject{{"s", sampler}, {"i", index}, {"ms", lengthMs}})
                                       .toJson(QJsonDocument::Compact);
        if (m_linkUp && m_pLink) {
            m_pLink->write("J " + ask + '\n');
        } else {
            sendSysexText(kNameAsk, ask);
        }
        return;
    }
    // The mapping reports a new track length when a deck or sampler loads or ejects a track; Mixxx knows
    // exactly what's loaded, so the page gets its title, artist and library id (for /waveform/<id>).
    const QString group = sampler ? QStringLiteral("[Sampler%1]").arg(index + 1)
                                  : QStringLiteral("[Channel%1]").arg(index + 1);
    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
    QJsonValue name;   // null
    if (pTrack) {
        QString title = pTrack->getTitle();
        if (title.isEmpty()) {
            title = QFileInfo(pTrack->getLocation()).completeBaseName();
        }
        name = QJsonObject{{"t", title}, {"a", pTrack->getArtist()}, {"id", pTrack->getId().toVariant().toInt()}};
    }
    applyName(sampler, index, name);
}

void Hub::applyName(bool sampler, int index, const QJsonValue& name) {
    if (index < 0 || index >= (sampler ? kNumSamplers : kNumDecks)) {
        return;
    }
    QJsonValue& current = sampler ? m_samplerNames[index] : m_decks[index].name;
    if (current == name) {
        return;
    }
    current = name;
    if (sampler) {
        emitJson({{"t", "sn"}, {"i", index}, {"n", name}});
    } else {
        emitJson({{"t", "dn"}, {"d", index}, {"n", name}});
    }
}

void Hub::emitJson(const QJsonObject& message) {
    m_server.broadcast(QStringLiteral("/ws"), QJsonDocument(message).toJson(QJsonDocument::Compact));
}

QJsonObject Hub::deckJson(int i) const {
    const Deck& d = m_decks[i];
    return {{"v", d.v},
            {"n", d.name},
            {"cues", toArray(d.cues, 8)},
            {"rolls", toArray(d.rolls, 8)},
            {"stems", toArray(d.stems, 4)},
            {"sc", d.stemCount},
            {"p", d.pitch}};
}

QJsonObject Hub::snapshot() const {
    QJsonArray samplerNames, decks, fx;
    for (const QJsonValue& n : m_samplerNames) {
        samplerNames.append(n);
    }
    for (int i = 0; i < kNumDecks; ++i) {
        decks.append(deckJson(i));
    }
    for (const auto& unit : m_fx) {
        fx.append(toArray(unit, 7));
    }
    return {{"init",
            QJsonObject{{"s", toArray(m_samplers, kNumSamplers)},
                    {"sn", samplerNames},
                    {"decks", decks},
                    {"fx", fx},
                    {"live", live()},
                    {"remote", remoteStatus()},
                    {"cv", m_controlValues}}}};
}

} // namespace zydek
