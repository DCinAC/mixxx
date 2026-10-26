#include "zydek/zydekhub.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSslSocket>
#include <QTcpSocket>
#include <QThreadPool>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#ifdef __BROADCAST__
#include "broadcast/defs_broadcast.h"
#include "engine/sidechain/enginenetworkstream.h"
#include "engine/sidechain/shoutconnection.h"
#include "recording/defs_recording.h"
#endif
#include "controllers/zydek/zydekcontroller.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "qml/qmllibraryproxy.h"
#include "zydek/zydekdiscordrelay.h"
#include "zydek/zydekmain.h"
#include "moc_zydekhub.cpp"
#include "effects/backends/effectmanifest.h"
#include "effects/backends/effectmanifestparameter.h"
#include "effects/effectchain.h"
#include "effects/effectparameterslotbase.h"
#include "effects/backends/effectsbackendmanager.h"
#include "effects/effectsmanager.h"
#include "effects/effectslot.h"
#include "effects/visibleeffectslist.h"
#include "qml/qmleffectsmanagerproxy.h"
#include "qml/qmlsoundmanagerproxy.h"
#include "soundio/sounddevice.h"
#include "soundio/soundmanager.h"
#include "soundio/soundmanagerconfig.h"
#include "soundio/soundmanagerutil.h"
#include "track/beats.h"
#include "track/cue.h"
#include "zydek/zydekpython.h"
#include "zydek/zydekstems.h"
#include "track/track.h"
#include "waveform/isynctimeprovider.h"
#include "waveform/visualplayposition.h"
#ifdef PA_USE_OBOE
#include <pa_oboe.h>   // PaOboe_GetOutputPresentationDelay (tools/zydeck-deps/portaudio)
#endif
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
const QString kAudioPrefsFile = QStringLiteral("zydek-audio.json");

const QString kSettingsFile = QStringLiteral("zydek-controller-settings.json");
const QString kPairedFile = QStringLiteral("zydeck-paired.json");   // (kept out of backups: AndroidManifest's rules)
const QString kSamplersFile = QStringLiteral("zydek-samplers.json");
const QString kKitsFile = QStringLiteral("zydek-kits.json");
constexpr int kKitSlots = 32;   // the pads ZyDeck shows
const QString kLastSession = QStringLiteral("Last session");
const QString kDeckSessionFile = QStringLiteral("zydek-decks.json");
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
            [this](const QString& group, TrackPointer pNew, TrackPointer) {
                if (m_controllerMode) {
                    return;   // the pages show the computer's decks
                }
                const int n = QStringView(group).mid(group.indexOf(QRegularExpression(QStringLiteral("\\d")))).chopped(1).toInt();
                if (group.startsWith(QLatin1String("[Channel")) && n >= 1 && n <= kNumDecks) {
                    updateName(false, n - 1, 0);
                    deckTrackLoaded(n - 1, pNew);
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
    QFile paired(QDir(m_pConfig->getSettingsPath()).filePath(kPairedFile));
    if (paired.open(QIODevice::ReadOnly)) {
        m_paired = QJsonDocument::fromJson(paired.readAll()).object();
    }
    m_server.setGate([this](const QString& path, const HttpServer::Query& query) { return allowedFrom(path, query); });
    m_remoteTimer.setInterval(1000);
    connect(&m_remoteTimer, &QTimer::timeout, this, &Hub::remoteTick);
    m_recordTimer.setInterval(1000);
    connect(&m_recordTimer, &QTimer::timeout, this, &Hub::recordTick);
    connect(&m_recordTimer, &QTimer::timeout, this, &Hub::checkFxMeta);
    connect(&m_recordTimer, &QTimer::timeout, this, &Hub::checkAudioOutput);
    m_recordTimer.start();
    m_slipTimer.setInterval(40);
    connect(&m_slipTimer, &QTimer::timeout, this, &Hub::slipTick);
    m_slipTimer.start();
    m_streamDelayTimer.setInterval(1000);
    connect(&m_streamDelayTimer, &QTimer::timeout, this, &Hub::streamDelayTick);
    m_streamDelayTimer.start();
    m_visualTimer.setTimerType(Qt::PreciseTimer);
    m_visualTimer.setInterval(16);
    connect(&m_visualTimer, &QTimer::timeout, this, &Hub::visualTick);
    m_visualTimer.start();
}

#ifdef Q_OS_ANDROID
namespace {
// cleared by ~Hub: Java keeps calling in until the app is gone
std::atomic<Hub*> s_pHub{nullptr};   // for ZydekMidi.java's and ZydekAudio.java's callbacks

void JNICALL jniMidiReceive(JNIEnv* env, jclass, jbyteArray data) {
    const jsize n = env->GetArrayLength(data);
    QByteArray bytes(n, Qt::Uninitialized);
    env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
    if (Hub* pHub = s_pHub) {
        QMetaObject::invokeMethod(pHub, [pHub, bytes] { pHub->usbMidiReceived(bytes); }, Qt::QueuedConnection);
    }
}

void JNICALL jniLatency(JNIEnv* env, jclass, jint ms, jstring route) {
    const char* chars = env->GetStringUTFChars(route, nullptr);
    const QString text = QString::fromUtf8(chars);
    env->ReleaseStringUTFChars(route, chars);
    if (Hub* pHub = s_pHub) {
        QMetaObject::invokeMethod(pHub, [pHub, ms, text] { pHub->outputLatencyMeasured(ms, text); }, Qt::QueuedConnection);
    }
}

void JNICALL jniMidiState(JNIEnv*, jclass, jint state) {
    if (Hub* pHub = s_pHub) {
        QMetaObject::invokeMethod(pHub, [pHub, state] { pHub->usbMidiState(state); }, Qt::QueuedConnection);
    }
}
} // namespace
#endif

Hub::~Hub() {
#ifdef Q_OS_ANDROID
    s_pHub = nullptr;
    if (m_controllerMode) {
        QJniObject::callStaticMethod<void>("org/mixxx/ZydekMidi", "setEnabled", "(Z)V", static_cast<jboolean>(false));
    }
#endif
    // its worker first, while everything it reports to is still here
    delete m_pLiveStems;
    m_pLiveStems = nullptr;
    // its own sockets (the link to ZyDeck Connect, the bot's status) close after its members are gone, and say so
    // (disconnected): not to here then
    for (QAbstractSocket* pSocket : findChildren<QAbstractSocket*>(Qt::FindDirectChildrenOnly)) {
        pSocket->disconnect(this);
    }
}

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
    const JNINativeMethod audioMethods[] = {
            {"nativeLatency", "(ILjava/lang/String;)V", reinterpret_cast<void*>(jniLatency)}};
    if (env.registerNativeMethods("org/mixxx/ZydekAudio", audioMethods, 1)) {
        QJniObject::callStaticMethod<void>("org/mixxx/ZydekAudio", "measureNow", "()V");
    }
#endif
    {
        QFile f(QDir(m_pConfig->getSettingsPath()).filePath(kAudioPrefsFile));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject prefs = QJsonDocument::fromJson(f.readAll()).object();
            const QJsonObject offsets = prefs.value(QStringLiteral("offsets")).toObject();
            for (auto it = offsets.begin(); it != offsets.end(); ++it) {
                m_latencyOffsets.insert(it.key(), it.value().toInt());
            }
            if (offsets.isEmpty() && prefs.value(QStringLiteral("offsetMs")).toInt()) {   // (one for all, before: the speaker's)
                m_latencyOffsets.insert(QStringLiteral("speaker"), prefs.value(QStringLiteral("offsetMs")).toInt());
            }
            const QJsonObject queues = prefs.value(QStringLiteral("queueBursts")).toObject();
            for (auto it = queues.begin(); it != queues.end(); ++it) {
                m_queueBursts.insert(it.key(), std::clamp(it.value().toInt(), 1, 16));
            }
            m_liveStemsAuto = prefs.value(QStringLiteral("liveStems")).toBool(true);
            m_recordPreFx = prefs.value(QStringLiteral("recordPreFx")).toBool(false);
            m_discordHost = prefs.value(QStringLiteral("discordHost")).toString();
            m_discordKey = prefs.value(QStringLiteral("discordKey")).toString();
            const QJsonArray defaults = prefs.value(QStringLiteral("deckDefaults")).toArray();
            for (int i = 0; i < 3 && i < defaults.size(); ++i) {
                m_deckDefaults[i] = std::clamp(defaults[i].toInt(-1), -1, 1);
            }
        }
    }
    loadRemote();
    ControlObject::set(ConfigKey(QStringLiteral("[Recording]"), QStringLiteral("zydek_pre_fx")), m_recordPreFx ? 1 : 0);
    // Mixxx clones a playing deck (or a held sampler) when a deck is loaded twice within half a second, a
    // "double tap" on a controller's LOAD button. Live stems loads a track's stems right after the track, and
    // a quick second tap on the pages' deck buttons is easy: both got some other player's audio. Off in ZyDeck.
    m_pConfig->setValue(ConfigKey(QStringLiteral("[Controls]"), QStringLiteral("CloneDeckOnLoadDoubleTap")), false);
    if (LiveStems::available()) {
        m_pLiveStems = new LiveStems(m_pConfig, this);
        connect(m_pLiveStems, &LiveStems::progress, this, &Hub::liveStemsProgress);
        connect(m_pLiveStems, &LiveStems::where, this, [this](int trackId, bool onPc) {
            for (int i = 0; i < kNumDecks; ++i) {
                if (m_deckStems[i].trackId == trackId && m_deckStems[i].onPc != onPc) {
                    m_deckStems[i].onPc = onPc;
                    emitDeckStems(i);
                }
            }
        });
        connect(m_pLiveStems, &LiveStems::finished, this, &Hub::liveStemsFinished);
        m_stemsCrateTimer.setInterval(15000);
        connect(&m_stemsCrateTimer, &QTimer::timeout, this, &Hub::checkStemsCrate);
        QTimer::singleShot(25000, this, [this] { checkStemsCrate(); m_stemsCrateTimer.start(); });
        // stems made before they copied their original's loudness (once Mixxx's library is up)
        QTimer::singleShot(20000, this, [this] {
            if (m_pLiveStems) {
                m_pLiveStems->repairLoudness();
            }
        });
    }
    m_lastSessionTimer.setSingleShot(true);
    m_lastSessionTimer.setInterval(2000);
    connect(&m_lastSessionTimer, &QTimer::timeout, this, [this] { saveKit(kLastSession); });
    QTimer::singleShot(5000, this, [this] { restoreLastSession(0); });
    QTimer::singleShot(6000, this, [this] { restoreDeckSession(0); });
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
    if (path == QLatin1String("/calibrate")) {   // lining the waveform up by ear
        return staticFile(QStringLiteral("calibrate.html"));
    }
    if (path == QLatin1String("/controller") || path == QLatin1String("/settings") ||
            path == QLatin1String("/sizes") || path == QLatin1String("/phone") || path == QLatin1String("/grid") ||
            path == QLatin1String("/sample")) {
        return staticFile(path.mid(1) + QLatin1String(".html"));
    }
    if (path == QLatin1String("/controller.webmanifest")) {
        return staticFile(QStringLiteral("controller.webmanifest"));
    }
    if (path == QLatin1String("/zydeck-key.js")) {   // a paired device's key on its requests here (pairApi)
        return staticFile(path.mid(1));
    }
    if (path == QLatin1String("/zydeck-position.js")) {   // where a playing track is drawn (every page that draws one)
        return staticFile(path.mid(1));
    }
    if (path == QLatin1String("/zydeck-knob.js")) {   // touch and knobs (the deck page, the phone page's mixer)
        return staticFile(path.mid(1));
    }
    if (path == QLatin1String("/zydeck-menu.js") || path == QLatin1String("/zydeck-menu.css")) {   // the settings menu, both pages
        return staticFile(path.mid(1));
    }
    if (path == QLatin1String("/zydeck-skin-d.js") || path == QLatin1String("/zydeck-skin-d.css")) {   // the deck page's Hardware look
        return staticFile(path.mid(1));
    }
    // 200: the data; 202: on a deck but still being analysed (a "wf" push says when it's done); 404: not on a deck
    if (path.startsWith(QLatin1String("/waveform/"))) {
        bool loaded = false;
        const QByteArray data = waveform(path.mid(10).toInt(), query.value(QStringLiteral("stems")) == QLatin1String("1"), &loaded);
        if (data.isEmpty()) {
            return loaded ? HttpServer::Response{202, "text/plain", "being analysed\n", {}}
                          : HttpServer::Response{404, "text/plain", "not on a deck\n", {}};
        }
        return {200, "application/octet-stream", data, {{"Cache-Control", "max-age=3600"}}};
    }
    if (path == QLatin1String("/api/latency-log")) {
        return latencyLog(query);
    }
    if (path.startsWith(QLatin1String("/beats/"))) {
        bool loaded = false;
        const QByteArray data = beats(path.mid(7).toInt(), &loaded);
        if (data.isEmpty()) {
            return loaded ? HttpServer::Response{202, "text/plain", "being analysed\n", {}}
                          : HttpServer::Response{404, "text/plain", "not on a deck\n", {}};
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
    if (path == QLatin1String("/api/audio/buffer")) {   // ?index=3..7
        return result(setAudioBuffer(query.value(QStringLiteral("index")).toInt()));
    }
    if (path == QLatin1String("/api/record")) {   // ?on=1|0 · ?prefx=1|0 (before the effect units) · nothing: the status
        if (query.contains(QStringLiteral("prefx"))) {
            m_recordPreFx = query.value(QStringLiteral("prefx")) == QLatin1String("1");
            ControlObject::set(ConfigKey(QStringLiteral("[Recording]"), QStringLiteral("zydek_pre_fx")), m_recordPreFx ? 1 : 0);
            saveAudioPrefs();
        }
        if (query.contains(QStringLiteral("on"))) {
            return result(setRecording(query.value(QStringLiteral("on")) == QLatin1String("1")));
        }
        return json(recordStatus());
    }
    if (path == QLatin1String("/api/discord")) {   // ?host=&key= (kept) · ?on=1|0 · nothing: the status
        if (query.contains(QStringLiteral("host")) || query.contains(QStringLiteral("key"))) {
            if (query.contains(QStringLiteral("host"))) {
                m_discordHost = query.value(QStringLiteral("host")).trimmed();
            }
            if (query.contains(QStringLiteral("key"))) {
                m_discordKey = query.value(QStringLiteral("key")).trimmed();
            }
            saveAudioPrefs();
            if (m_pRelay && m_pRelay->state() > 0) {   // live: to the new address now
                m_pRelay->start(m_discordHost, m_discordKey);
                m_pRelay->reconnect();
            }
        }
        if (query.contains(QStringLiteral("on"))) {
            return result(setDiscord(query.value(QStringLiteral("on")) == QLatin1String("1")));
        }
        return json(discordStatus());
    }
    if (path == QLatin1String("/api/deckdefaults")) {   // ?keylock=&quantize=&slip= (1 on, 0 off, -1 as they were)
        static const char* const kNames[3] = {"keylock", "quantize", "slip"};
        bool changed = false;
        for (int i = 0; i < 3; ++i) {
            if (query.contains(QLatin1String(kNames[i]))) {
                m_deckDefaults[i] = std::clamp(query.value(QLatin1String(kNames[i])).toInt(), -1, 1);
                changed = true;
            }
        }
        if (changed) {
            saveAudioPrefs();
            emitJson({{"t", "ddef"}, {"v", deckDefaultsJson()}});
        }
        return json(QJsonObject{{"ok", true}, {"defaults", deckDefaultsJson()}});
    }
    if (path == QLatin1String("/api/tracklist")) {   // this session's
        return json(m_library.tracklist());
    }
    if (path == QLatin1String("/api/audio/optimize")) {   // start · ?status=1 · ?cancel=1
        if (query.contains(QStringLiteral("cancel"))) {
            if (m_optimize.running) {
                finishOptimize(QStringLiteral("Cancelled"));
            }
            return json(optimizeStatus());
        }
        if (query.contains(QStringLiteral("status"))) {
            return json(optimizeStatus());
        }
        return result(startOptimize());
    }
    if (path == QLatin1String("/api/audio/latency")) {   // ?offset=ms (fine-tune) · ?measure=1
        if (query.contains(QStringLiteral("offset"))) {
            // (for the kind of output it plays on now; Bluetooth can need a lot)
            m_latencyOffsets.insert(routeKind(), std::clamp(query.value(QStringLiteral("offset")).toInt(), -200, 800));
            saveAudioPrefs();
            emitLatency();
        }
#ifdef Q_OS_ANDROID
        if (query.value(QStringLiteral("measure")) == QLatin1String("1")) {
            QJniObject::callStaticMethod<void>("org/mixxx/ZydekAudio", "measureNow", "()V");
        }
#endif
        return json(audioStatus());
    }
    if (path == QLatin1String("/api/audio/set")) {   // ?main=<output name>&headphones=<output name, or empty>
        const QJsonObject result = setAudio(query);
#ifdef Q_OS_ANDROID
        QJniObject::callStaticMethod<void>("org/mixxx/ZydekAudio", "measureNow", "()V");   // a new route
#endif
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

// ---- pairing: who else may use this ZyDeck --------------------------------------------------------------

bool Hub::allowedFrom(const QString& path, const HttpServer::Query& query) const {
    static const QRegularExpression outside(QStringLiteral(
            R"(^/(|controller|settings|sizes|phone|grid|sample|calibrate|controller\.webmanifest|zydeck-[a-z0-9-]+\.(js|css)|api/(whoami|offer|pair))$)"));
    if (outside.match(path).hasMatch()) {
        return true;
    }
    const QString key = query.value(QStringLiteral("k"));
    return !key.isEmpty() && m_paired.contains(key);
}

/// GET /api/pair. Another device: ?id=<its question>&name=<its name> asks (the pages here show the question; it asks
/// again with the same id until there's an answer: {state: asked | yes (with its key, once) | no | busy}); ?k=<key>:
/// is that key still allowed ({state: yes | no}). This device: ?answer=<id>&yes=0|1, ?list=1 (the allowed devices'
/// names), ?forget=1 (none allowed any more).
HttpServer::Response Hub::pairApi(const HttpServer::Query& query) {
    const auto arg = [&query](const char* name) { return query.value(QLatin1String(name)); };
    const bool local = arg("_local") == QLatin1String("1");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_pairAsks.begin(); it != m_pairAsks.end();) {   // (a question lasts two minutes)
        it = now - it->at > 120000 ? m_pairAsks.erase(it) : std::next(it);
    }
    if (query.contains(QStringLiteral("k"))) {
        return json(QJsonObject{{"state", m_paired.contains(arg("k")) ? "yes" : "no"}});
    }
    if (local && query.contains(QStringLiteral("answer"))) {
        const auto it = m_pairAsks.find(arg("answer"));
        if (it == m_pairAsks.end() || it->state != QLatin1String("asked")) {
            return result({{"ok", false}, {"error", "That question has gone (it waits two minutes)"}});
        }
        if (arg("yes") == QLatin1String("1")) {
            QByteArray raw(24, Qt::Uninitialized);
            QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(raw.data()), raw.size() / 4);
            it->key = QString::fromLatin1(raw.toHex());
            it->state = QStringLiteral("yes");
            m_paired.insert(it->key, QJsonObject{{"name", it->name}, {"at", QDateTime::currentDateTime().toString(Qt::ISODate)}});
            savePaired();
        } else {
            it->state = QStringLiteral("no");
        }
        return result({{"ok", true}});
    }
    if (local && query.contains(QStringLiteral("list"))) {
        QJsonArray names;
        for (const QJsonValue& v : std::as_const(m_paired)) {
            names.append(v.toObject().value(QStringLiteral("name")));
        }
        return json(QJsonObject{{"devices", names}});
    }
    if (local && query.contains(QStringLiteral("forget"))) {
        m_paired = {};
        savePaired();
        return result({{"ok", true}});
    }
    const QString id = arg("id").left(64);
    if (id.isEmpty()) {
        return {400, "text/plain", "id missing\n", {}};
    }
    const auto it = m_pairAsks.find(id);
    if (it == m_pairAsks.end()) {
        for (const PairAsk& a : std::as_const(m_pairAsks)) {
            if (a.state == QLatin1String("asked")) {
                return json(QJsonObject{{"state", "busy"}});   // (one question at a time)
            }
        }
        const QString name = arg("name").left(60).trimmed().isEmpty() ? QStringLiteral("A ZyDeck") : arg("name").left(60).trimmed();
        m_pairAsks.insert(id, {name, arg("_peer"), QStringLiteral("asked"), {}, now});
        emitJson({{"t", "pairask"}, {"id", id}, {"name", name}, {"peer", arg("_peer")}});
        return json(QJsonObject{{"state", "asked"}});
    }
    const QJsonObject out{{"state", it->state}};
    if (it->state == QLatin1String("yes")) {   // (the key goes out once)
        const QJsonObject withKey{{"state", "yes"}, {"key", it->key}};
        m_pairAsks.erase(it);
        return json(withKey);
    }
    return json(out);
}

void Hub::savePaired() const {
    QSaveFile f(QDir(m_pConfig->getSettingsPath()).filePath(kPairedFile));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(m_paired).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

HttpServer::Response Hub::handleApi(const QString& path, const HttpServer::Query& query) {
    auto arg = [&query](const char* name) { return query.value(QLatin1String(name)); };
    if (path == QLatin1String("/api/pair")) {
        return pairApi(query);
    }
    if (path == QLatin1String("/api/offer")) {   // ?offer=<JSON>: ZyDeck Connect on a computer, which found this device
        // on the network, asks it to link ({name, hosts, port, key}): the pages ask the user (nothing changes without)
        const QJsonObject offer = QJsonDocument::fromJson(arg("offer").toUtf8()).object();
        if (offer.value(QStringLiteral("hosts")).toArray().isEmpty() || offer.value(QStringLiteral("key")).toString().isEmpty()) {
            return result({{"ok", false}, {"error", "Not an offer"}});
        }
        if (m_linkUp && m_linkTarget.value(QStringLiteral("key")) == offer.value(QStringLiteral("key"))) {
            return result({{"ok", true}, {"linked", true}});   // (linked to it already)
        }
        emitJson({{"t", "offer"}, {"o", offer}});
        return result({{"ok", true}, {"asked", !m_server.clients(QStringLiteral("/ws")).isEmpty()}});
    }
    if (path == QLatin1String("/api/whoami")) {   // ZydekLink.java's network scan: "a ZyDeck, called ..."
        QString name = deviceName();
#ifdef Q_OS_ANDROID
        const QString shown = QJniObject::callStaticObjectMethod("org/mixxx/ZydekLink", "displayName", "()Ljava/lang/String;").toString();
        if (!shown.isEmpty()) {
            name = shown;
        }
#endif
        return json(QJsonObject{{"zydeck", true}, {"name", name}});
    }
    if (path == QLatin1String("/api/library/views")) {
        return json(m_library.views());
    }
    if (path == QLatin1String("/api/library/tracks")) {
        QString view = arg("view").isEmpty() ? QStringLiteral("all") : arg("view");
        static const QRegularExpression fitsDeck(QStringLiteral(R"(^fits:([1-4])$)"));
        if (const QRegularExpressionMatch m = fitsDeck.match(view); m.hasMatch()) {
            // what fits deck N now: its current key (key shift included) and tempo
            const QString group = QStringLiteral("[Channel%1]").arg(m.captured(1));
            const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
            view = QStringLiteral("fits:%1:%2:%3")
                           .arg(std::lround(ControlObject::get(ConfigKey(group, QStringLiteral("key")))))
                           .arg(ControlObject::get(ConfigKey(group, QStringLiteral("bpm"))), 0, 'f', 2)
                           .arg(pTrack ? pTrack->getId().toVariant().toInt() : 0);
        }
        return json(m_library.tracks(arg("q"),
                view,
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
            Library::setSamplerOptions(group, arg("sync") != QLatin1String("0"), false, arg("keylock") != QLatin1String("0"));
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
        // ?link=0 (not now / back to the cable), ?forget=1 (forget the computer), ?offer=<JSON> (a computer's
        // name, hosts, port and key, from ZyDeck Connect's QR code)
        if (query.contains(QStringLiteral("layout"))) {   // zydeck | standard
            m_standardMidi = arg("layout") == QLatin1String("standard");
            saveRemote();
            m_remoteLive = false;
            resendSubscriptions();
            emitRemote();
            m_server.broadcast(QStringLiteral("/ws"), QJsonDocument(snapshot()).toJson(QJsonDocument::Compact));
        }
        if (query.contains(QStringLiteral("on"))) {
            setControllerMode(arg("on") == QLatin1String("1"));
        }
        if (query.contains(QStringLiteral("offer"))) {   // ZyDeck Connect's offer from its QR code (as over the cable)
            const QJsonObject j = QJsonDocument::fromJson(arg("offer").toUtf8()).object();
            if (!j.value(QStringLiteral("hosts")).toArray().isEmpty() && !j.value(QStringLiteral("key")).toString().isEmpty()) {
                m_linkOffer = j;
                m_offerDeclined = false;
                emitRemote();
            }
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
    if (path == QLatin1String("/api/debug/pos")) {   // ?group=
        return json(debugPos(arg("group")));
    }
    if (path == QLatin1String("/api/grid/save")) {   // ?group=: the beat grid editor is done
        m_gridStart.remove(arg("group"));
        return result(m_library.saveLoaded(arg("group")));
    }
    if (path == QLatin1String("/api/grid/begin") || path == QLatin1String("/api/grid/revert")) {   // ?group=
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(arg("group"));
        if (!pTrack) {
            return result({{"ok", false}, {"error", "Nothing loaded there"}});
        }
        if (path.endsWith(QLatin1String("begin"))) {   // the grid as the editor opens on it
            m_gridStart.insert(arg("group"), {pTrack->getLocation(), pTrack->getBeats(), pTrack->isBpmLocked()});
            return result({{"ok", true}, {"id", pTrack->getId().toVariant().toInt()}});   // (which track that was)
        }
        const GridStart start = m_gridStart.take(arg("group"));   // thrown away: back as it was
        if (start.location != pTrack->getLocation()) {
            return result({{"ok", false}, {"error", "The track has changed"}});
        }
        pTrack->setBpmLocked(false);
        const bool ok = !start.beats || pTrack->trySetBeats(start.beats);
        pTrack->setBpmLocked(start.locked);
        return result({{"ok", ok}, {"bpm", pTrack->getBpm()}});
    }
    if (path == QLatin1String("/api/analyze")) {   // ?dry=1: only count the tracks without BPM or key
        return result(m_library.analyzeAll(arg("dry") == QLatin1String("1")));
    }
    if (path == QLatin1String("/api/sampler/capture")) {   // ?deck=&slot=&sync=1|0
        const QJsonObject r = m_library.captureSample(arg("deck").toInt(), arg("slot").toInt(), arg("sync") != QLatin1String("0"),
                arg("keylock") != QLatin1String("0"));
        if (r.value(QStringLiteral("ok")).toBool()) {
            setSamplerMode(arg("slot").toInt(), QStringLiteral("loop"));   // a captured loop loops
        }
        return result(r);
    }
    if (path == QLatin1String("/api/grid/range")) {   // ?group=&start=&end= (s): a steady grid from that stretch · ?status=1 · ?cancel=1
        GridRange& job = m_gridRange;
        if (query.contains(QStringLiteral("cancel")) && job.cancel) {
            job.cancel->store(true);
        }
        if (query.contains(QStringLiteral("status")) || query.contains(QStringLiteral("cancel"))) {
            QJsonObject out = job.result;
            out.insert(QStringLiteral("running"), job.running);
            out.insert(QStringLiteral("serial"), job.serial);
            return json(out);
        }
        if (job.running) {
            return result({{"ok", false}, {"error", "Already analysing a stretch"}});
        }
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(arg("group"));
        if (!pTrack) {
            return result({{"ok", false}, {"error", "Nothing loaded there"}});
        }
        // decoding it takes a while: on a worker (this thread handles the tablet's input)
        job.running = true;
        job.result = {};
        job.cancel = std::make_shared<std::atomic<bool>>(false);
        const int serial = ++job.serial;
        const double start = arg("start").toDouble(), end = arg("end").toDouble();
        QThreadPool::globalInstance()->start([this, pTrack, start, end, serial, cancel = job.cancel] {
            const QJsonObject out = Library::analyzeRange(pTrack, start, end, cancel.get());
            QMetaObject::invokeMethod(
                    this,
                    [this, out, serial] {
                        if (m_gridRange.serial == serial) {
                            m_gridRange.running = false;
                            m_gridRange.result = out;
                        }
                    },
                    Qt::QueuedConnection);
        });
        return result({{"ok", true}, {"serial", serial}});
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
    if (path == QLatin1String("/api/fx")) {
        return json(fxStatus());
    }
    if (path == QLatin1String("/api/stemfx")) {   // ?deck=1..4&fx=0..3
        return result(stemFx(arg("deck").toInt() - 1, arg("fx").toInt()));
    }
    if (path == QLatin1String("/api/fx/beats")) {   // ?unit=1..4&slot=1..3 (0: every slot)&beats=0.5
        return result(setFxBeats(arg("unit").toInt(), arg("slot").toInt(), arg("beats").toDouble()));
    }
    if (path == QLatin1String("/api/livestems")) {   // ?auto=0|1: make stems whenever a deck loads a track
        if (query.contains(QStringLiteral("auto"))) {
            m_liveStemsAuto = arg("auto") != QLatin1String("0");
            saveAudioPrefs();
            if (!m_liveStemsAuto) {   // off: decks on live stems go back to their originals, where they are
                for (int i = 0; i < kNumDecks; ++i) {
                    swapToOriginal(i);   // (not where the stems are in use)
                }
            }
        }
        return json(liveStemsStatus());
    }
    if (path == QLatin1String("/api/livestems/make")) {   // ?deck=1..4: now, for what's loaded
        const int deck = arg("deck").toInt() - 1;
        const TrackPointer pTrack = deck >= 0 && deck < kNumDecks ? PlayerInfo::instance().getTrackInfo(QStringLiteral("[Channel%1]").arg(deck + 1)) : TrackPointer();
        if (!m_pLiveStems || !pTrack) {
            return result({{"ok", false}, {"error", m_pLiveStems ? "Nothing loaded there" : "This build can't make stems on the phone"}});
        }
        m_deckStems[deck] = {pTrack->getId().toVariant().toInt(), QStringLiteral("making"), 0, {}};
        m_pLiveStems->request(pTrack);
        emitDeckStems(deck);
        return result({{"ok", true}});
    }
    if (path == QLatin1String("/api/stemscrate")) {   // ?track=[&on=1|0]: in the Stems crate (made if needed)
        const int track = arg("track").toInt();
        if (query.contains(QStringLiteral("on")) && track > 0) {
            int crate = 0;
            m_library.crateTrackIds(QStringLiteral("Stems"), &crate);
            if (!crate) {
                crate = m_library.createCrate(QStringLiteral("Stems")).value(QStringLiteral("id")).toInt();
            }
            const QJsonObject r = crate ? m_library.setCrateTrack(crate, track, arg("on") != QLatin1String("0"))
                                        : QJsonObject{{"ok", false}, {"error", "Couldn't make the Stems crate"}};
            if (!r.value(QStringLiteral("ok")).toBool()) {
                return result(r);
            }
            checkStemsCrate();
        }
        return json(stemsCrateStatus(track));
    }
    if (path == QLatin1String("/api/livestems/adopt")) {   // ?id=&path=: the PC made this track's stems
        if (!m_pLiveStems || arg("id").toInt() <= 0 || !QFileInfo::exists(arg("path"))) {
            return result({{"ok", false}, {"error", "No such track or file"}});
        }
        m_pLiveStems->adopt(arg("id").toInt(), arg("path"), true);
        return result({{"ok", true}});
    }
    if (path == QLatin1String("/api/livestems/swap")) {   // ?deck=1..4: load its stems, where it is
        return result(swapToStems(arg("deck").toInt() - 1, true));
    }
    if (path == QLatin1String("/api/calibrate")) {   // ?on=1: the test pattern in the headphones · ?on=0: done · ?state=1
        if (arg("state") == QLatin1String("1")) {   // the test pattern's track, once it's loaded (for its waveform)
            const TrackPointer pTrack = m_calib.group.isEmpty() ? TrackPointer() : PlayerInfo::instance().getTrackInfo(m_calib.group);
            const bool ours = pTrack && pTrack->getLocation() == calibrationFile();
            return json({{"ok", true}, {"group", m_calib.group}, {"id", ours ? pTrack->getId().toVariant().toInt() : 0}});
        }
        return result(calibrate(arg("on") == QLatin1String("1")));
    }
    if (path == QLatin1String("/api/library/remove")) {   // ?id=&delete=1: the file too
        const bool deleteFile = arg("delete") == QLatin1String("1");
        const QJsonObject r = m_library.removeTrack(arg("id").toInt(), deleteFile);
        if (r.value(QStringLiteral("ok")).toBool() && deleteFile && m_pLiveStems) {
            m_pLiveStems->forget(arg("id").toInt());
        }
        return result(r);
    }
    if (path == QLatin1String("/api/library/rename")) {   // ?id=&title=
        const QJsonObject r = m_library.renameTrack(arg("id").toInt(), arg("title"));
        for (int i = 0; i < kNumSamplers && r.value(QStringLiteral("ok")).toBool(); ++i) {
            updateName(true, i, 0);   // a pad holding it shows the new name
        }
        for (int i = 0; i < kNumDecks && r.value(QStringLiteral("ok")).toBool(); ++i) {
            updateName(false, i, 0);
        }
        return result(r);
    }
    if (path == QLatin1String("/api/library/cues")) {   // ?id=: the track's hot cues
        return result(m_library.trackCues(arg("id").toInt()));
    }
    if (path == QLatin1String("/api/library/cues/edit")) {   // ?id=&n=&label= | &color=%23rrggbb | &clear=1
        QJsonObject change;
        if (query.contains(QStringLiteral("label"))) {
            change.insert(QStringLiteral("label"), arg("label"));
        }
        if (query.contains(QStringLiteral("color"))) {
            change.insert(QStringLiteral("color"), arg("color"));
        }
        if (arg("clear") == QLatin1String("1")) {
            change.insert(QStringLiteral("clear"), true);
        }
        const QJsonObject r = m_library.editTrackCue(arg("id").toInt(), arg("n").toInt(), change);
        if (r.value(QStringLiteral("ok")).toBool()) {   // every page with this track on a deck reads its names again
            emitJson({{"t", "cuelbl"}, {"id", arg("id").toInt()}});
        }
        return result(r);
    }
    if (path == QLatin1String("/api/cues")) {   // ?group=[ChannelN] -> {"labels": [8 hot cue names]}
        QJsonArray labels;
        for (int i = 0; i < 8; ++i) {
            labels.append(QString());
        }
        if (const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(arg("group"))) {
            for (const CuePointer& pCue : pTrack->getCuePoints()) {
                const int n = pCue->getHotCue();
                if (n >= 0 && n < 8) {
                    labels[n] = pCue->getLabel();
                }
            }
        }
        return json(QJsonObject{{"labels", labels}});
    }
    if (path == QLatin1String("/api/cues/label")) {   // ?group=&n=1..8&label=
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(arg("group"));
        const int n = arg("n").toInt() - 1;
        if (pTrack) {
            for (const CuePointer& pCue : pTrack->getCuePoints()) {
                if (pCue->getHotCue() == n) {
                    pCue->setLabel(arg("label").trimmed().left(60));
                    emitJson({{"t", "cuelbl"}, {"id", pTrack->getId().toVariant().toInt()}});
                    return result({{"ok", true}});
                }
            }
        }
        return result({{"ok", false}, {"error", "No hot cue there"}});
    }
    if (path == QLatin1String("/api/sampler/rename")) {   // ?slot=1..64&name=: the sample's title in the library
        const int slot = arg("slot").toInt();
        const QString name = arg("name").trimmed().left(120);
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(QStringLiteral("[Sampler%1]").arg(slot));
        if (!pTrack || name.isEmpty()) {
            return result({{"ok", false}, {"error", pTrack ? "Give it a name" : "Nothing on that pad"}});
        }
        pTrack->setTitle(name);
        m_library.saveLoaded(QStringLiteral("[Sampler%1]").arg(slot));
        updateName(true, slot - 1, 0);
        return result({{"ok", true}});
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
                arg("stems").toUInt(), slot, arg("sync") != QLatin1String("0"), arg("keylock") != QLatin1String("0"));
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
/// stems (?stems=1), and the track has them: "MXWS", the same, then each stem's level per point too
/// (Mixxx's order: drums, bass, other, vocals), 8 bytes a point, so the page can draw each stem on its own.
QByteArray Hub::waveform(int trackId, bool stems, bool* pLoaded) const {
    const auto tracks = PlayerInfo::instance().getLoadedTracks();
    for (const TrackPointer& pTrack : tracks) {
        if (!pTrack || pTrack->getId().toVariant().toInt() != trackId) {
            continue;
        }
        if (pLoaded) {
            *pLoaded = true;
        }
        const ConstWaveformPointer pWaveform = pTrack->getWaveform();
        if (!pWaveform || pWaveform->getDataSize() < 2 ||
                pWaveform->getCompletion() < pWaveform->getDataSize()) {
            return {};   // still being analysed
        }
        const int points = pWaveform->getDataSize() / 2;
        const WaveformData* pData = pWaveform->data();
        const int stemCount = stems ? std::min(pWaveform->getStemCount(), mixxx::kMaxSupportedStems) : 0;
        const int stride = stemCount > 0 ? 8 : 4;
        QByteArray out(12 + points * stride, Qt::Uninitialized);
        memcpy(out.data(), stemCount > 0 ? "MXWS" : "MXWF", 4);
        // Points per second of audio (441 for Mixxx's analysis; the rate itself isn't public).
        const double duration = pTrack->getDuration();
        const float rate = static_cast<float>(duration > 0 ? points / duration : 441.0);
        qToLittleEndian(rate, out.data() + 4);
        qToLittleEndian(static_cast<quint32>(points), out.data() + 8);
        auto* p = reinterpret_cast<unsigned char*>(out.data() + 12);
        for (int i = 0; i < points; ++i) {
            const WaveformFilteredData& l = pData[2 * i].filtered;
            const WaveformFilteredData& r = pData[2 * i + 1].filtered;
            unsigned char* q = p + stride * i;
            q[0] = std::max(l.all, r.all);
            q[1] = std::max(l.low, r.low);
            q[2] = std::max(l.mid, r.mid);
            q[3] = std::max(l.high, r.high);
            for (int s = 0; s < 4 && stemCount > 0; ++s) {
                q[4 + s] = s < stemCount ? std::max(pData[2 * i].stems[s], pData[2 * i + 1].stems[s]) : 0;
            }
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
        if (text.startsWith("{\"clk\"")) {   // a page matching its clock to the hub's: answered at once, to the ms fraction
            const double c = QJsonDocument::fromJson(text).object().value(QStringLiteral("clk")).toDouble();
            m_server.sendText(pClient, QJsonDocument(QJsonObject{{"t", "clk"}, {"c", c},
                    {"h", static_cast<double>(m_clock.nsecsElapsed()) / 1e6}}).toJson(QJsonDocument::Compact));
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
QByteArray Hub::beats(int trackId, bool* pLoaded) const {
    const auto tracks = PlayerInfo::instance().getLoadedTracks();
    for (const TrackPointer& pTrack : tracks) {
        if (!pTrack || pTrack->getId().toVariant().toInt() != trackId) {
            continue;
        }
        if (pLoaded) {
            *pLoaded = true;
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

// ---- the decks' session ----------------------------------------------------------------------------

void Hub::notice(const QString& text) {
    m_notice = text;
    m_noticeAt = QDateTime::currentMSecsSinceEpoch();
    emitJson({{"t", "notice"}, {"m", text}});
}

void Hub::saveDeckSession() {
    if (!m_deckSessionReady || m_optimize.running) {
        return;
    }
    QJsonArray decks;
    QString key;
    bool playing = false;
    for (int i = 0; i < kNumDecks; ++i) {
        const QString group = QStringLiteral("[Channel%1]").arg(i + 1);
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
        if (!pTrack) {
            decks.append(QJsonValue());
            key += QLatin1Char('|');
            continue;
        }
        const auto get = [&group](const char* name) {
            return ControlObject::get(ConfigKey(group, QLatin1String(name)));
        };
        const bool play = get("play") > 0;
        playing = playing || play;
        decks.append(QJsonObject{{"location", pTrack->getLocation()}, {"position", get("playposition")},
                {"rate", get("rate")}, {"pitchAdjust", get("pitch_adjust")}, {"keylock", get("keylock") > 0},
                {"quantize", get("quantize") > 0}, {"playing", play}});
        key += pTrack->getLocation() + (play ? QStringLiteral("|>") : QStringLiteral("|"));
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // a new track or play/pause: now; while playing, the positions every 5 s
    if (key == m_deckSessionKey && !(playing && now - m_deckSessionAt >= 5000)) {
        return;
    }
    m_deckSessionKey = key;
    m_deckSessionAt = now;
    QSaveFile f(QDir(m_pConfig->getSettingsPath()).filePath(kDeckSessionFile));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{"decks", decks}, {"playing", playing}, {"at", now}}).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void Hub::restoreDeckSession(int tries) {
    if (ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_decks"))) < 2 && tries < 12) {
        QTimer::singleShot(2500, this, [this, tries] { restoreDeckSession(tries + 1); });
        return;
    }
    QFile f(QDir(m_pConfig->getSettingsPath()).filePath(kDeckSessionFile));
    const QJsonObject session = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
    QString reason;
#ifdef Q_OS_ANDROID
    reason = QJniObject::callStaticObjectMethod<jstring>("org/mixxx/ZydekRunningService", "lastExitReason").toString();
#endif
    const bool crashed = reason == QLatin1String("crash") || reason == QLatin1String("anr") ||
            reason == QLatin1String("low memory");
    const bool wasPlaying = session.value(QStringLiteral("playing")).toBool();
    int restored = 0;
    if (wasPlaying || crashed) {
        const QJsonArray decks = session.value(QStringLiteral("decks")).toArray();
        for (int i = 0; i < kNumDecks && i < decks.size(); ++i) {
            const QJsonObject d = decks[i].toObject();
            const QString location = d.value(QStringLiteral("location")).toString();
            const QString group = QStringLiteral("[Channel%1]").arg(i + 1);
            if (location.isEmpty() || !QFileInfo::exists(location) || PlayerInfo::instance().getTrackInfo(group)) {
                continue;   // gone, or the deck has something already
            }
            // where it was, paused (deckTrackLoaded seeks there; nor do the new-track defaults apply)
            m_pendingSeek.insert(group, {d.value(QStringLiteral("position")).toDouble(), false, location});
            if (!m_library.loadLocation(location, QStringLiteral("deck%1").arg(i + 1), true).value(QStringLiteral("ok")).toBool()) {
                m_pendingSeek.remove(group);
                continue;
            }
            ++restored;
            // the deck's settings once the track is in (Mixxx may reset tempo and key on a load)
            QTimer::singleShot(2500, this, [group, d] {
                ControlObject::set(ConfigKey(group, QStringLiteral("rate")), d.value(QStringLiteral("rate")).toDouble());
                ControlObject::set(ConfigKey(group, QStringLiteral("pitch_adjust")), d.value(QStringLiteral("pitchAdjust")).toDouble());
                ControlObject::set(ConfigKey(group, QStringLiteral("keylock")), d.value(QStringLiteral("keylock")).toBool() ? 1 : 0);
                ControlObject::set(ConfigKey(group, QStringLiteral("quantize")), d.value(QStringLiteral("quantize")).toBool() ? 1 : 0);
            });
        }
    }
    if (restored) {
        qInfo() << "Zydek: restored" << restored << "decks (last run:" << (crashed ? reason : QStringLiteral("stopped while playing")) << ")";
        notice(QStringLiteral("The decks are back where they were: ZyDeck %1 last time")
                        .arg(crashed ? QStringLiteral("crashed") : QStringLiteral("stopped while playing")));
    }
    QTimer::singleShot(4000, this, [this] { m_deckSessionReady = true; });   // after the loads have landed
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
    runOnMain(
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
                        {"buffer", static_cast<int>(config.getAudioBufferSizeIndex())},
                        {"api", config.getAPI()},
                        {"devices", list},
                        {"main", main},
                        {"mainChannels", mainChannels},
                        {"headphones", headphones},
                        {"headphonesChannels", headphonesChannels}};
            });
    out.insert(QStringLiteral("latencyMs"),
            ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("output_latency_ms"))));
    out.insert(QStringLiteral("measuredMs"), m_measuredLatencyMs);
    out.insert(QStringLiteral("streamMs"), m_streamDelayMs);   // Mixxx's own stream, as Android times it (-1: not yet)
    out.insert(QStringLiteral("route"), m_latencyRoute);
    out.insert(QStringLiteral("kind"), routeKind());
    out.insert(QStringLiteral("path"), outputPath());
    out.insert(QStringLiteral("offsetMs"), latencyOffsetMs());
    out.insert(QStringLiteral("totalMs"), std::round(latencySeconds() * 1000));
    out.insert(QStringLiteral("dropouts"),
            ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("audio_latency_overload_count"))));
    return out;
}

/// Mixxx's audio buffer: 3 = 5.3 ms, 4 = 10.7, 5 = 21.3, 6 = 42.7, 7 = 85.3 (at 48 kHz). Smaller answers sooner
/// but drops out if the phone can't keep up.
QJsonObject Hub::setAudioBuffer(int index) {
    const std::shared_ptr<SoundManager> pManager = mixxx::qml::QmlSoundManagerProxy::registeredManager();
    if (!pManager || index < 1 || index > 7) {
        return {{"ok", false}, {"error", "No such buffer size"}};
    }
    QJsonObject out{{"ok", false}, {"error", "Mixxx is busy: try again"}};
    runOnMain(
            pManager.get(),
            [&] {
                SoundManagerConfig config = pManager->getConfig();
                config.setAudioBufferSizeIndex(static_cast<unsigned int>(index));
                const SoundDeviceStatus status = pManager->setConfig(config);
                out = status == SoundDeviceStatus::Ok
                        ? QJsonObject{{"ok", true}}
                        : QJsonObject{{"ok", false}, {"error", pManager->getLastErrorMessage(status)}};
            });
#ifdef Q_OS_ANDROID
    QJniObject::callStaticMethod<void>("org/mixxx/ZydekAudio", "measureNow", "()V");
#endif
    emitLatency();
    return out;
}

// ---- recording the set ---------------------------------------------------------------------------------

namespace {
const ConfigKey kRecordStatus(QStringLiteral("[Recording]"), QStringLiteral("status"));   // 0 off, 1 starting, 2 on
QString recordingsFolder() {
    const QString shared = QStringLiteral("/storage/emulated/0/Music");
    const QString music = QFileInfo(shared).isWritable() ? shared : QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return music + QStringLiteral("/ZyDeck Recordings");
}
} // namespace

QJsonObject Hub::recordStatus() const {
    const int status = static_cast<int>(ControlObject::get(kRecordStatus));
    return {{"ok", true}, {"on", status != 0}, {"status", status},
            {"seconds", status == 2 && m_recordClock.isValid() ? m_recordClock.elapsed() / 1000.0 : 0.0},
            {"file", status ? m_pConfig->getValueString(ConfigKey(QStringLiteral("[Recording]"), QStringLiteral("Path"))) : QString()},
            {"folder", recordingsFolder()}, {"preFx", m_recordPreFx}};
}

QJsonObject Hub::setRecording(bool on) {
    if (m_controllerMode) {
        return {{"ok", false}, {"error", "In controller mode, record on the computer"}};
    }
    if ((ControlObject::get(kRecordStatus) != 0) == on) {
        return recordStatus();
    }
    if (on) {   // FLAC (lossless, half a WAV), one file however long, a .cue tracklist, in ZyDeck Recordings
        const QString dir = recordingsFolder();
        QDir().mkpath(dir);
        const auto set = [this](const char* key, const QString& value) {
            m_pConfig->setValue(ConfigKey(QStringLiteral("[Recording]"), QLatin1String(key)), value);
        };
        set("Directory", dir);
        set("Encoding", QStringLiteral("FLAC"));
        set("FileSize", QStringLiteral("No Splitting"));
        set("CueEnabled", QStringLiteral("1"));
        set("tracklist_as_comment", QStringLiteral("1"));
    }
    // Mixxx's RECORD button: a press toggles it
    ControlObject::set(ConfigKey(QStringLiteral("[Recording]"), QStringLiteral("toggle_recording")), 1);
    ControlObject::set(ConfigKey(QStringLiteral("[Recording]"), QStringLiteral("toggle_recording")), 0);
    QTimer::singleShot(300, this, [this] { recordTick(); });
    return {{"ok", true}, {"on", on}};
}

// The Web tab's settings (zydek_web.py keeps them in zydek-web.json): read here, so Python needn't start for it
bool Hub::stemServerSet() const {
    QFile f(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("zydek-web.json")));
    return f.open(QIODevice::ReadOnly) &&
            !QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("stemServer")).toString().trimmed().isEmpty();
}

void Hub::recordTick() {
    saveDeckSession();
    if (m_stemServerTick++ % 5 == 0) {   // the stem server set or not (every 5 s): what STEM pad 5 says
        const bool server = stemServerSet();
        if (server != m_stemServer) {
            m_stemServer = server;
            emitJson({{"t", "ssrv"}, {"on", server}});
        }
    }
    const int discord = discordStatus().value(QStringLiteral("status")).toInt();
    if (discord != m_discordStatus) {   // 0 off · 1 connecting · 2 live · 3 failed · 4 (some failed)
        m_discordStatus = discord;
        m_botTick = 0;
        emitJson({{"t", "dc"}, {"d", discordStatus()}});
    }
    if (discord == 2 && m_botTick++ % 10 == 1) {
        checkBot();
    }
    const int status = static_cast<int>(ControlObject::get(kRecordStatus));
    if (status == 2 && m_recordStatus != 2) {
        m_recordClock.start();
    }
    if (status != m_recordStatus || status == 2) {   // every second while recording (the pages' clock)
        m_recordStatus = status;
        emitJson({{"t", "rec"}, {"r", recordStatus()}});
    }
}

// ---- streaming to the ZyDeck Discord bot ----------------------------------------------------------------

QJsonObject Hub::discordStatus() const {
    const bool enabled = ControlObject::get(ConfigKey(QStringLiteral("[Shoutcast]"), QStringLiteral("enabled"))) > 0;
    // Mixxx's connection is to the relay on this device; the relay's to the bot is what counts
    int status = static_cast<int>(ControlObject::get(ConfigKey(QStringLiteral("[Shoutcast]"), QStringLiteral("status"))));
    const int relay = enabled && m_pRelay ? m_pRelay->state() : 0;
    if (relay > 0) {
        status = relay;
    } else if (enabled && status == 2) {
        status = 1;   // Mixxx is connected to the relay, which hasn't reached the bot yet
    }
    QJsonObject out{{"ok", true}, {"host", m_discordHost}, {"hasKey", !m_discordKey.isEmpty()}, {"on", enabled},
            {"status", status}};
    if (!m_bot.isEmpty()) {
        out.insert(QStringLiteral("bot"), m_bot);   // the bot's last /status: its channel, listeners, kicked
    }
#ifdef __BROADCAST__
    const std::shared_ptr<SoundManager> pManager = mixxx::qml::QmlSoundManagerProxy::registeredManager();
    if (relay == 3) {
        out.insert(QStringLiteral("error"), m_pRelay->error());
    } else if (status == 3 && pManager) {   // failed: why (written by the connection before it gave up)
        QString error;
        runOnMain(
                qApp,
                [&] {
                    for (const NetworkOutputStreamWorkerPtr& pWorker : pManager->getNetworkStream()->outputWorkers()) {
                        if (const ShoutConnectionPtr pConnection = qSharedPointerCast<ShoutConnection>(pWorker);
                                pConnection && pConnection->profile()->getEnabled()) {
                            error = pConnection->lastError();
                        }
                    }
                },
                2000);
        out.insert(QStringLiteral("error"), error);
    }
#else
    out.insert(QStringLiteral("error"), QStringLiteral("This build has no Live Broadcasting"));
#endif
    return out;
}

void Hub::checkBot() {
    const DiscordRelay::Target target = DiscordRelay::parseTarget(m_discordHost);
    if (m_pBotCheck || !target.valid()) {
        return;
    }
    QTcpSocket* pSocket = target.tls ? new QSslSocket(this) : new QTcpSocket(this);
    m_pBotCheck = pSocket;
    auto pReply = std::make_shared<QByteArray>();
    const QByteArray request = "GET /status HTTP/1.0\r\nHost: " + target.host.toUtf8() + "\r\nAuthorization: Bearer " +
            m_discordKey.toUtf8() + "\r\nUser-Agent: ZyDeck\r\n\r\n";
    const auto send = [pSocket, request] {
        pSocket->write(request);
    };
    if (target.tls) {
        connect(static_cast<QSslSocket*>(pSocket), &QSslSocket::encrypted, pSocket, send);
    } else {
        connect(pSocket, &QTcpSocket::connected, pSocket, send);
    }
    connect(pSocket, &QTcpSocket::readyRead, pSocket, [pSocket, pReply] {
        pReply->append(pSocket->readAll());
        if (pReply->size() > 65536) {
            pSocket->abort();
        }
    });
    connect(pSocket, &QTcpSocket::disconnected, this, [this, pSocket, pReply] {
        const int body = pReply->indexOf("\r\n\r\n");
        const QJsonObject status = body < 0 ? QJsonObject() : QJsonDocument::fromJson(pReply->mid(body + 4)).object();
        pSocket->deleteLater();
        if (!status.isEmpty()) {
            onBotStatus(status);
        }
    });
    connect(pSocket, &QTcpSocket::errorOccurred, pSocket, [pSocket](QAbstractSocket::SocketError error) {
        if (error != QAbstractSocket::RemoteHostClosedError) {
            pSocket->deleteLater();
        }
    });
    QTimer::singleShot(8000, pSocket, [pSocket] { pSocket->abort(); pSocket->deleteLater(); });
    if (target.tls) {
        static_cast<QSslSocket*>(pSocket)->connectToHostEncrypted(target.host, target.port);
    } else {
        pSocket->connectToHost(target.host, target.port);
    }
}

void Hub::onBotStatus(const QJsonObject& status) {
    const QJsonObject kicked = status.value(QStringLiteral("kicked")).toObject();
    const double at = kicked.value(QStringLiteral("at")).toDouble();
    if (at > 0 && at != m_botKickedAt) {
        m_botKickedAt = at;
        // a kick from the last 10 minutes is news (an older one was told before, or happened offline)
        if (QDateTime::currentMSecsSinceEpoch() - at < 10 * 60 * 1000) {
            const QString channel = kicked.value(QStringLiteral("channel")).toString();
            const QString text = QStringLiteral("The Discord bot was kicked from %1. It stays out until /zydeck join.")
                                         .arg(channel.isEmpty() ? QStringLiteral("its voice channel") : channel);
            notice(text);
#ifdef Q_OS_ANDROID
            QJniObject::callStaticMethod<void>("org/mixxx/ZydekRunningService",
                    "notice",
                    "(Ljava/lang/String;Ljava/lang/String;)V",
                    QJniObject::fromString(QStringLiteral("Discord bot kicked")).object<jstring>(),
                    QJniObject::fromString(text).object<jstring>());
#endif
        }
    }
    if (status != m_bot) {
        m_bot = status;
        emitJson({{"t", "dc"}, {"d", discordStatus()}});
    }
}

QJsonObject Hub::setDiscord(bool on) {
#ifdef __BROADCAST__
    if (on && (m_discordHost.isEmpty() || m_discordKey.isEmpty())) {
        return {{"ok", false}, {"error", "Set the bot's address and stream key first"}};
    }
    const std::shared_ptr<SoundManager> pManager = mixxx::qml::QmlSoundManagerProxy::registeredManager();
    if (!pManager) {
        return {{"ok", false}, {"error", "Mixxx isn't ready"}};
    }
    if (!m_pRelay) {
        m_pRelay = new DiscordRelay(this);
    }
    if (on && !DiscordRelay::parseTarget(m_discordHost).valid()) {
        return {{"ok", false}, {"error", "That bot address doesn't look right"}};
    }
    if (on && !m_pRelay->start(m_discordHost, m_discordKey)) {
        return {{"ok", false}, {"error", m_pRelay->error()}};
    }
    const quint16 relayPort = m_pRelay->port();
    const QString relayPassword = m_pRelay->password();
    QString error;
    const bool ran = runOnMain(
            qApp,
            [&] {
                // Mixxx's Live Broadcasting connections (one per profile; Mixxx makes a default one)
                QList<BroadcastProfilePtr> profiles;
                for (const NetworkOutputStreamWorkerPtr& pWorker : pManager->getNetworkStream()->outputWorkers()) {
                    if (const ShoutConnectionPtr pConnection = qSharedPointerCast<ShoutConnection>(pWorker)) {
                        profiles.append(pConnection->profile());
                    }
                }
                if (profiles.isEmpty()) {
                    error = QStringLiteral("Mixxx's Live Broadcasting isn't set up");
                    return;
                }
                // through a proxy: the broadcast manager hears it (connects / disconnects)
                ControlProxy enabled(ConfigKey(QStringLiteral(BROADCAST_PREF_KEY), QStringLiteral("enabled")));
                if (!on) {
                    enabled.set(0);
                    return;
                }
                if (enabled.toBool() && profiles.first()->getPort() != relayPort) {
                    enabled.set(0);   // on, but to an earlier relay (it restarted): connect again
                }
                if (enabled.toBool()) {
                    return;   // already on (off and on again to change the address)
                }
                for (int i = 1; i < profiles.size(); ++i) {
                    profiles[i]->setEnabled(false);
                }
                // To the relay on this device (an Icecast source); its settings are read when it connects
                const BroadcastProfilePtr& pProfile = profiles.first();
                pProfile->setHost(QStringLiteral("127.0.0.1"));
                pProfile->setPort(relayPort);
                pProfile->setServertype(QStringLiteral(BROADCAST_SERVER_ICECAST2));
                pProfile->setLogin(QStringLiteral("source"));
                pProfile->setPassword(relayPassword);
                pProfile->setMountPoint(QStringLiteral("/zydeck"));
                pProfile->setFormat(QStringLiteral(ENCODING_OPUS));   // 48 kHz, 20 ms packets: as Discord takes them
                pProfile->setBitrate(128);
                pProfile->setChannels(2);
                pProfile->setStreamName(QStringLiteral("ZyDeck"));
                pProfile->setStreamPublic(false);
                pProfile->setEnableMetadata(false);
                pProfile->setOggDynamicUpdate(false);   // one Ogg stream for the whole set
                pProfile->setEnableReconnect(true);     // the bot restarting, Wi-Fi dropping: keep trying
                pProfile->setNoDelayFirstReconnect(true);
                pProfile->setReconnectPeriod(5);
                pProfile->setLimitReconnects(false);
                pProfile->setEnabled(true);
                enabled.set(1);
            },
            5000);
    if (!on) {
        m_pRelay->stop();
    }
    if (!ran) {
        return {{"ok", false}, {"error", "Mixxx is busy: try again"}};
    }
    if (!error.isEmpty()) {
        return {{"ok", false}, {"error", error}};
    }
    QJsonObject out = discordStatus();
    out.insert(QStringLiteral("ok"), true);
    return out;
#else
    Q_UNUSED(on);
    return {{"ok", false}, {"error", "This build has no Live Broadcasting"}};
#endif
}

void Hub::checkFxMeta() {
    QList<QString> changed;
    for (int u = 0; u < 2; ++u) {
        for (int j = 0; j < 3; ++j) {
            const QString group = QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]").arg(u + 1).arg(j + 1);
            const double loaded = ControlObject::get(ConfigKey(group, QStringLiteral("loaded_effect")));
            if (loaded != m_loadedFx[u][j]) {
                m_loadedFx[u][j] = loaded;
                changed.append(group);
            }
        }
    }
    const std::shared_ptr<EffectsManager> pEffects = mixxx::qml::QmlEffectsManagerProxy::registeredManager();
    if (changed.isEmpty() || !pEffects) {
        return;
    }
    runOnMain(
            qApp,
            [&] {
                for (int u = 0; u < 2; ++u) {
                    const EffectChainPointer pChain = pEffects->getStandardEffectChain(u);
                    if (!pChain) {
                        continue;
                    }
                    for (const EffectSlotPointer& pSlot : pChain->getEffectSlots()) {
                        const EffectManifestPointer pManifest = pSlot->getManifest();
                        if (!pManifest || !changed.contains(pSlot->getGroup()) ||
                                (pManifest->id() != QLatin1String("org.mixxx.effects.echo") &&
                                        pManifest->id() != QLatin1String("org.mixxx.effects.reverb"))) {
                            continue;
                        }
                        const ConfigKey meta(pSlot->getGroup(), QStringLiteral("meta"));
                        if (ControlObject::get(meta) <= 0) {
                            // through a proxy, so the slot hears it and moves the linked Send (-3 dB, Echo's own default)
                            ControlProxy(meta).set(0.7);
                        }
                    }
                }
            },
            2000);
}

namespace {
// Mixxx's waveforms ask where the deck will be at their next frame; the hub asks where it is now
class NowSync : public VSyncTimeProvider {
  public:
    std::chrono::microseconds fromTimerToNextSync(const PerformanceTimer& timer) override {
        return std::chrono::microseconds(timer.elapsed().toIntegerMicros());
    }
    std::chrono::microseconds getSyncInterval() const override {
        return std::chrono::microseconds(16667);
    }
};
} // namespace

/// Where Mixxx's own waveform would draw the deck now (its engine position and when that was, run on to now, plus
/// the time to the output), and the playposition the pages get, with the hub's clock: to see how far the pages'
/// drawing is from it (/api/debug/pos?group=)
QSharedPointer<VisualPlayPosition> Hub::visualPos(const QString& group) {
    QSharedPointer<VisualPlayPosition> pPos = m_visualPosOf.value(group);
    if (!pPos) {   // (made on Mixxx's main thread)
        runOnMain(qApp, [&pPos, &group] { pPos = VisualPlayPosition::getVisualPlayPosition(group); }, 1000);
        if (pPos) {
            m_visualPosOf.insert(group, pPos);
        }
    }
    return pPos;
}

/// {"t":"vp","ts":<hub ms>,"p":{"[Channel1]":[position 0-1, speed in track per second],...}}: each deck that plays
/// (and, a few times, one that just stopped), where it's heard now as far as Mixxx's audio timing knows. What Mixxx's
/// timing doesn't know (visualExtraSeconds: the phone's own output path, the user's offset) is in init's and lat's
/// "vlat", for the page to take off.
void Hub::visualTick() {
    if (m_controllerMode || m_server.clients(QStringLiteral("/ws")).isEmpty()) {
        return;   // (in controller mode the decks are the computer's)
    }
    static NowSync now;
    static const QStringList groups{QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]"), QStringLiteral("[Channel3]"),
            QStringLiteral("[Channel4]"), QStringLiteral("[PreviewDeck1]")};
    QJsonObject positions;
    for (const QString& group : groups) {
        if (ControlObject::get(ConfigKey(group, QStringLiteral("track_loaded"))) <= 0) {
            m_visualQuiet.remove(group);
            continue;
        }
        const QSharedPointer<VisualPlayPosition> pPos = visualPos(group);
        double play = 0, slip = 0, rate = 0;
        if (!pPos || !pPos->getPlaySlipAtNextVSync(&now, &play, &slip) || !pPos->getRateFractionPerSecond(&rate)) {
            continue;
        }
        int& quiet = m_visualQuiet[group];
        quiet = rate == 0 ? quiet + 1 : 0;
        if (quiet > 6) {
            continue;   // still: once it's said so a few times, the page's own (stopped) model holds it
        }
        positions.insert(group, QJsonArray{play, rate});
    }
    if (!positions.isEmpty()) {
        emitJson({{"t", "vp"}, {"ts", static_cast<double>(m_clock.nsecsElapsed()) / 1e6}, {"p", positions}});
    }
}

double Hub::visualExtraSeconds() const {
    if (m_controllerMode) {
        return 0;
    }
    // Mixxx's timing counts its own buffer (the Oboe host says a buffer reaches the speaker one buffer later); the
    // rest of Android's path and the user's offset it doesn't
    if (m_streamDelayMs >= 0) {
        const double buffer = ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("output_latency_ms")));
        return std::max(0.0, m_streamDelayMs - buffer + latencyOffsetMs()) / 1000.0;
    }
    return std::max(0.0, static_cast<double>(std::max(0, m_measuredLatencyMs) + latencyOffsetMs())) / 1000.0;
}

/// Once a second: when Mixxx's output stream is heard (the Oboe host measures it at every callback from Android's
/// timestamps of that stream, following the route), the median of the last 5; the pages hear of a change of 2 ms or more
void Hub::streamDelayTick() {
#ifdef PA_USE_OBOE
    // where the queue settled on this kind of output: the next stream there starts at that (not while Optimize probes,
    // nor in the app's first 20 s: starting up stalls the sound now and then, and grows it for nothing; then it's put
    // back to what was learned; nor in controller mode, where this device's own sound isn't heard)
    PaOboe_OutputStatus out{};
    PaOboe_GetOutputStatus(&out);
    const QString kind = routeKind();
    static bool settled = false;
    if (!settled && m_clock.elapsed() > 20000) {
        settled = true;
        applyQueue();
        return;
    }
    if (settled && out.open && out.framesPerBurst > 0 && !kind.isEmpty() && !m_optimize.running && !m_controllerMode) {
        const int bursts = (out.bufferSizeFrames + out.framesPerBurst - 1) / out.framesPerBurst;
        if (bursts > m_queueBursts.value(kind, 0)) {
            qInfo() << "Zydek: the output queue on" << kind << "settled at" << bursts << "bursts of" << out.framesPerBurst << "frames";
            m_queueBursts.insert(kind, bursts);
            saveAudioPrefs();
        }
    }
    static QList<int> recent;
    const double d = PaOboe_GetOutputPresentationDelay();
    if (d <= 0) {
        return;
    }
    recent.append(static_cast<int>(std::lround(d * 1000)));
    while (recent.size() > 5) {
        recent.removeFirst();
    }
    QList<int> sorted = recent;
    std::sort(sorted.begin(), sorted.end());
    const int ms = sorted.at(sorted.size() / 2);
    if (m_streamDelayMs < 0 || std::abs(ms - m_streamDelayMs) >= 2) {
        if (m_streamDelayMs < 0) {
            qInfo() << "Zydek: Mixxx's output is heard" << ms << "ms after its callback (Android's timestamps of the stream)";
        }
        m_streamDelayMs = ms;
        emitLatency();
    }
#endif
}

QJsonObject Hub::debugPos(const QString& group) {
    static NowSync now;
    const QSharedPointer<VisualPlayPosition> pPos = visualPos(group);
    double play = -1, slip = -1;
    const bool ok = pPos && pPos->getPlaySlipAtNextVSync(&now, &play, &slip);
    return {{"ok", ok}, {"ts", static_cast<double>(m_clock.elapsed())}, {"visual", play},
            {"playposition", ControlObject::get(ConfigKey(group, QStringLiteral("playposition")))},
            {"duration", ControlObject::get(ConfigKey(group, QStringLiteral("duration")))},
            {"latMs", latencySeconds() * 1000}};
}

void Hub::slipTick() {
    static NowSync now;
    for (int i = 0; i < kNumDecks; ++i) {
        const QString group = QStringLiteral("[Channel%1]").arg(i + 1);
        double play = 0, slip = 0;
        bool show = false;
        if (ControlObject::get(ConfigKey(group, QStringLiteral("slip_enabled"))) > 0) {
            if (!m_visualPos[i]) {   // the engine's, made on Mixxx's main thread
                QSharedPointer<VisualPlayPosition> pPos;
                runOnMain(qApp, [&pPos, &group] { pPos = VisualPlayPosition::getVisualPlayPosition(group); }, 1000);
                m_visualPos[i] = pPos;
            }
            // as Mixxx's waveforms: slipping while the two positions differ
            show = m_visualPos[i] && m_visualPos[i]->getPlaySlipAtNextVSync(&now, &play, &slip) &&
                    std::abs(slip - play) > 1e-9;
        }
        if (show) {
            emitJson({{"t", "slip"}, {"d", i}, {"p", slip}});   // p: the slip position, 0-1 of the track
        } else if (m_slipShown[i]) {
            emitJson({{"t", "slip"}, {"d", i}, {"p", QJsonValue()}});
        }
        m_slipShown[i] = show;
    }
}

// ---- one-tap audio optimisation ------------------------------------------------------------------------

namespace {
const int kOptimizeBuffers[] = {3, 4, 5};   // Mixxx's block: 5.3, 10.7, 21.3 ms at 48 kHz
constexpr int kOptimizeSeconds = 60;        // per block: a size on the edge stalls now and then (30 s missed some)
constexpr double kOptimizeMaxQueueMs = 25;  // a block whose queue had to grow past this isn't worth it
/// The output's underruns since its stream opened (PortAudio's Oboe host counts AAudio's), or Mixxx's overloads
int outputGlitches() {
#ifdef PA_USE_OBOE
    PaOboe_OutputStatus s{};
    PaOboe_GetOutputStatus(&s);
    return s.xruns;
#else
    return static_cast<int>(ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("audio_latency_overload_count"))));
#endif
}
ConfigKey deckKey(int deck, const char* key) {
    return ConfigKey(QStringLiteral("[Channel%1]").arg(deck), QLatin1String(key));
}
} // namespace

QJsonObject Hub::optimizeStatus() const {
    return {{"running", m_optimize.running}, {"state", m_optimize.state}, {"results", m_optimize.results},
            {"chosen", m_optimize.chosen}, {"error", m_optimize.error},
            {"seconds", kOptimizeSeconds}};
}

QJsonObject Hub::startOptimize() {
    if (m_optimize.running) {
        return {{"ok", false}, {"error", "Already testing"}};
    }
    if (m_controllerMode) {
        return {{"ok", false}, {"error", "In controller mode the sound comes from the computer"}};
    }
    for (int deck = 1; deck <= kNumDecks; ++deck) {
        if (ControlObject::get(deckKey(deck, "track_loaded")) > 0) {
            return {{"ok", false}, {"error", "Empty the decks first: the test plays tracks on all four (silently)"}};
        }
    }
    // the load: stems tracks first (the heaviest), then the most played tracks
    QStringList stems = m_pLiveStems ? m_pLiveStems->files() : QStringList();
    QList<int> tracks;
    for (const QJsonValue& t : m_library.tracks(QString(), QStringLiteral("all"), QStringLiteral("played"), 40, 0)) {
        const QJsonObject o = t.toObject();
        if (!o.value(QStringLiteral("missing")).toBool() && o.value(QStringLiteral("duration")).toDouble() > 90) {
            tracks.append(o.value(QStringLiteral("id")).toInt());
        }
    }
    if (stems.size() + tracks.size() < kNumDecks) {
        return {{"ok", false}, {"error", "The test needs four tracks of at least 1.5 minutes in the library"}};
    }
    Optimize& o = m_optimize;
    const int serial = o.serial + 1;
    o = Optimize();
    o.serial = serial;
    o.running = true;
    o.mainGain = ControlObject::get(ConfigKey(QStringLiteral("[Master]"), QStringLiteral("gain")));
    ControlObject::set(ConfigKey(QStringLiteral("[Master]"), QStringLiteral("gain")), 0);   // silent from here
    for (int deck = 1; deck <= kNumDecks; ++deck) {
        const QString target = QStringLiteral("deck%1").arg(deck);
        if (deck <= 2 && !stems.isEmpty()) {
            m_library.loadLocation(stems.takeFirst(), target, true);
        } else {
            m_library.load(tracks.takeFirst(), target);
        }
    }
    o.state = QStringLiteral("Loading four tracks…");
    emitJson({{"t", "opt"}, {"o", optimizeStatus()}});
    QTimer::singleShot(6000, this, [this, serial] {
        if (m_optimize.serial != serial || !m_optimize.running) {
            return;
        }
        for (int deck = 1; deck <= kNumDecks; ++deck) {   // key lock and +4 %: the time-stretcher is the heavy part
            ControlObject::set(deckKey(deck, "keylock"), 1);
            ControlObject::set(deckKey(deck, "rate"), 0.5 * ControlObject::get(deckKey(deck, "rate_dir")));
            ControlObject::set(deckKey(deck, "playposition"), 0.2);
            ControlObject::set(deckKey(deck, "play"), 1);
        }
        optimizeStep(serial);
    });
    return {{"ok", true}, {"started", true}};
}

/// One block size: from the smallest queue (2 bursts) up, as the underruns grow it; after a moment to start, half the
/// time to settle, and the other half that must have none
void Hub::optimizeStep(int serial) {
    Optimize& o = m_optimize;
    if (o.serial != serial || !o.running) {
        return;
    }
    const int buffer = kOptimizeBuffers[o.step];
#ifdef PA_USE_OBOE
    PaOboe_SetOutputQueueBursts(2);   // (the stream the new block opens starts there)
#endif
    const QJsonObject set = setAudioBuffer(buffer);
    if (!set.value(QStringLiteral("ok")).toBool(true)) {
        finishOptimize(set.value(QStringLiteral("error")).toString());
        return;
    }
    o.state = QStringLiteral("Trying a %1 ms block…").arg(QString::number((32 << buffer) * 1000.0 / 48000, 'f', 1));
    emitJson({{"t", "opt"}, {"o", optimizeStatus()}});
    QTimer::singleShot(4000, this, [this, serial] {   // after the sound has restarted with the new block
        if (m_optimize.serial != serial || !m_optimize.running) {
            return;
        }
        m_optimize.xrunsAt = outputGlitches();
        QTimer::singleShot(kOptimizeSeconds * 500, this, [this, serial] {
            if (m_optimize.serial != serial || !m_optimize.running) {
                return;
            }
            m_optimize.xrunsMid = outputGlitches();
            QTimer::singleShot(kOptimizeSeconds * 500, this, [this, serial] { optimizeMeasure(serial); });
        });
    });
}

void Hub::optimizeMeasure(int serial) {
    Optimize& o = m_optimize;
    if (o.serial != serial || !o.running) {
        return;
    }
    const int buffer = kOptimizeBuffers[o.step];
    const int glitches = outputGlitches();
    const QJsonObject path = outputPath();
    const double queueMs = path.value(QStringLiteral("queueMs")).toDouble();
    const int burst = path.value(QStringLiteral("burst")).toInt();
    const int bursts = burst > 0 ? (path.value(QStringLiteral("queue")).toInt() + burst - 1) / burst : kDefaultQueueBursts;
    const int late = glitches - o.xrunsMid;   // after it had settled
    o.results.append(QJsonObject{{"buffer", buffer}, {"glitches", glitches - o.xrunsAt}, {"late", late},
            {"queueMs", queueMs}, {"bursts", bursts}});
    const int last = static_cast<int>(std::size(kOptimizeBuffers)) - 1;
    if ((late == 0 && queueMs <= kOptimizeMaxQueueMs) || o.step == last) {
        o.chosen = buffer;
        o.chosenBursts = bursts;
        finishOptimize(QString());
        return;
    }
    ++o.step;
    optimizeStep(serial);
}

void Hub::finishOptimize(const QString& error) {
    Optimize& o = m_optimize;
    o.running = false;
    ++o.serial;   // its timers stop
    o.error = error;
    for (int deck = 1; deck <= kNumDecks; ++deck) {
        ControlObject::set(deckKey(deck, "play"), 0);
        ControlObject::set(deckKey(deck, "keylock"), 0);
        ControlObject::set(deckKey(deck, "rate"), 0);
        if (ControlObject::get(deckKey(deck, "track_loaded")) > 0) {   // (eject on an empty deck reloads the last track)
            ControlObject::set(deckKey(deck, "eject"), 1);
            ControlObject::set(deckKey(deck, "eject"), 0);
        }
    }
    if (o.chosen && !routeKind().isEmpty()) {   // where the queue settled with that block: the start from now on
        m_queueBursts.insert(routeKind(), o.chosenBursts);
        saveAudioPrefs();
    }
    applyQueue();
    if (o.chosen) {
        setAudioBuffer(o.chosen);   // (it measures the output again)
    }
    // the volume comes back once the decks have stopped
    const double gain = o.mainGain;
    QTimer::singleShot(500, this, [gain] {
        ControlObject::set(ConfigKey(QStringLiteral("[Master]"), QStringLiteral("gain")), gain);
    });
    o.state = error.isEmpty() ? QStringLiteral("Done") : error;
    emitJson({{"t", "opt"}, {"o", optimizeStatus()}});
}

void Hub::outputLatencyMeasured(int ms, const QString& route) {
    m_measuredLatencyMs = ms;
    m_latencyRoute = route;
    if (!m_optimize.running) {
        applyQueue();
    }
    emitLatency();
}

int Hub::latencyOffsetMs() const {
    return m_latencyOffsets.value(routeKind(), 0);
}

QString Hub::routeKind() const {
    return m_latencyRoute.isEmpty() ? QString() : m_latencyRoute.section(QStringLiteral(" · "), -1);
}

void Hub::applyQueue() {
#ifdef PA_USE_OBOE
    PaOboe_SetOutputQueueBursts(m_queueBursts.value(routeKind(), kDefaultQueueBursts));
#endif
}

QJsonObject Hub::outputPath() const {
#ifdef PA_USE_OBOE
    PaOboe_OutputStatus s{};
    PaOboe_GetOutputStatus(&s);
    if (!s.open || s.sampleRate <= 0) {
        return {};
    }
    const auto ms = [&s](int frames) { return std::round(frames * 10000.0 / s.sampleRate) / 10; };
    return {{"mmap", s.mmap}, {"exclusive", s.exclusive == 1}, {"aaudio", s.aaudio == 1}, {"rate", s.sampleRate},
            {"burst", s.framesPerBurst}, {"burstMs", ms(s.framesPerBurst)}, {"callback", s.framesPerCallback},
            {"queue", s.bufferSizeFrames}, {"queueMs", ms(s.bufferSizeFrames)}, {"capacity", s.bufferCapacityFrames},
            {"glitches", s.xruns}};
#else
    return {};
#endif
}

/// How far behind Mixxx's reported play positions the sound is: Mixxx's buffer, plus the output path after
/// it as measured (ZydekAudio.java; Bluetooth is most of it), plus the user's offset. Nothing in controller
/// mode: the sound comes from the computer.
double Hub::latencySeconds() const {
    if (m_controllerMode) {
        return 0;
    }
    const double buffer = ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("output_latency_ms")));
    if (m_streamDelayMs >= 0) {   // the stream itself, as Android times it
        return std::max(0, m_streamDelayMs + latencyOffsetMs()) / 1000.0;
    }
    return std::max(0.0, buffer + std::max(0, m_measuredLatencyMs) + latencyOffsetMs()) / 1000.0;
}

/// The sound's delay as measured (Android's timing of Mixxx's stream, or the test sound), without the user's offset:
/// what another device's screen draws by, with its own calibration on top (that was measured against this sound, so
/// the host screen's fine-tune isn't stacked on it)
double Hub::audioSeconds() const {
    if (m_controllerMode) {
        return 0;
    }
    if (m_streamDelayMs >= 0) {
        return m_streamDelayMs / 1000.0;
    }
    const double buffer = ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("output_latency_ms")));
    return std::max(0.0, buffer + std::max(0, m_measuredLatencyMs)) / 1000.0;
}

void Hub::emitLatency() {
    emitJson({{"t", "lat"}, {"s", latencySeconds()}, {"v", visualExtraSeconds()}, {"a", audioSeconds()},
            {"o", latencyOffsetMs()}, {"bt", routeKind() == QLatin1String("Bluetooth")}});
}

void Hub::saveAudioPrefs() const {
    QSaveFile f(QDir(m_pConfig->getSettingsPath()).filePath(kAudioPrefsFile));
    if (f.open(QIODevice::WriteOnly)) {
        QJsonObject queues;
        for (auto it = m_queueBursts.cbegin(); it != m_queueBursts.cend(); ++it) {
            queues.insert(it.key(), it.value());
        }
        QJsonObject offsets;
        for (auto it = m_latencyOffsets.cbegin(); it != m_latencyOffsets.cend(); ++it) {
            offsets.insert(it.key(), it.value());
        }
        f.write(QJsonDocument(QJsonObject{{"offsets", offsets}, {"queueBursts", queues}, {"liveStems", m_liveStemsAuto},
                                       {"recordPreFx", m_recordPreFx}, {"discordHost", m_discordHost},
                                       {"discordKey", m_discordKey}, {"deckDefaults", deckDefaultsJson()}})
                        .toJson());
        f.commit();
    }
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
    QJsonObject out{{"ok", false}, {"error", "Mixxx is busy: try again"}};
    runOnMain(
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
                // No main output: only the headphones play (the cue, with as much of the main mix as CUE MIX
                // lets in), and the main mix goes only where it's recorded or streamed (Discord). Mixxx keeps
                // making it for those (EngineMixer::onOutputDisconnected), the headphones' device keeping time.
                const SoundDevicePointer pMain = mainName.isEmpty() ? SoundDevicePointer() : find(mainName);
                const SoundDevicePointer pHeadphones = headphonesName.isEmpty() ? SoundDevicePointer() : find(headphonesName);
                if ((!mainName.isEmpty() && !pMain) || (!headphonesName.isEmpty() && !pHeadphones)) {
                    out = {{"ok", false}, {"error", QStringLiteral("That output isn't connected any more")}};
                    return;
                }
                if (!pMain && !pHeadphones) {
                    out = {{"ok", false}, {"error", QStringLiteral("Pick the headphones' output: with neither, nothing plays")}};
                    return;
                }
#ifdef Q_OS_ANDROID
                // Android plays an app's media to one output at a time: two outputs (speaker and Bluetooth, say)
                // take turns, each one cutting the other off many times a second.
                if (pMain && pHeadphones && pHeadphones != pMain) {
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
                if ((pMain && !fits(pMain, mainBase, mainCount)) || (pHeadphones && !fits(pHeadphones, phonesBase, phonesCount))) {
                    out = {{"ok", false}, {"error", QStringLiteral("That output doesn't have those channels")}};
                    return;
                }
                // Main and headphones may share channels: Mixxx then mixes the cue into the main mix there
                // (the page warns about it first).
                const bool shared = pMain && pHeadphones == pMain &&
                        mainBase < phonesBase + phonesCount && phonesBase < mainBase + mainCount;
                QMultiHash<SoundDeviceId, AudioOutput>& outputs = config.getOutputsRef();
                for (auto it = outputs.begin(); it != outputs.end();) {
                    const AudioPathType type = it.value().getType();
                    it = type == AudioPathType::Main || type == AudioPathType::Headphones ? outputs.erase(it) : std::next(it);
                }
                if (pMain) {
                    config.addOutput(pMain->getDeviceId(),
                            AudioOutput(AudioPathType::Main,
                                    static_cast<unsigned char>(mainBase),
                                    mixxx::audio::ChannelCount(mainCount),
                                    0));
                }
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
            });
    return out;
}

void Hub::checkAudioOutput() {
#ifdef Q_OS_ANDROID
    // Every 2 s: is the main or the headphones on a device that's there? (Mixxx's own status can't say: with
    // nothing found it runs on the "Network stream" device's clock and calls that connected.) Two checks in a
    // row without: the system output becomes the main.
    if (m_optimize.running || ++m_audioTick % 2) {
        return;
    }
    const std::shared_ptr<SoundManager> pManager = mixxx::qml::QmlSoundManagerProxy::registeredManager();
    if (!pManager) {
        return;
    }
    QString now;
    runOnMain(
            pManager.get(),
            [&] {
                SoundManagerConfig config = pManager->getConfig();
                const QList<SoundDevicePointer> devices = pManager->getDeviceList(config.getAPI(), true, false);
                QSet<SoundDeviceId> present;
                for (const SoundDevicePointer& pDevice : devices) {
                    present.insert(pDevice->getDeviceId());
                }
                QMultiHash<SoundDeviceId, AudioOutput>& outputs = config.getOutputsRef();
                bool playing = false;
                for (auto it = outputs.cbegin(); it != outputs.cend(); ++it) {
                    const AudioPathType type = it.value().getType();
                    playing = playing || ((type == AudioPathType::Main || type == AudioPathType::Headphones) && present.contains(it.key()));
                }
                m_audioDownSeconds = playing ? 0 : m_audioDownSeconds + 2;
                if (playing || m_audioDownSeconds < 4 || devices.isEmpty()) {
                    return;
                }
                m_audioDownSeconds = 0;
                // the first output is the one that follows Android's routing (PortAudio's Oboe enumerator)
                const SoundDevicePointer pSystem = devices.first();
                for (auto it = outputs.begin(); it != outputs.end();) {
                    const AudioPathType type = it.value().getType();
                    it = type == AudioPathType::Main || type == AudioPathType::Headphones ? outputs.erase(it) : std::next(it);
                }
                config.addOutput(pSystem->getDeviceId(), AudioOutput(AudioPathType::Main, 0, mixxx::audio::ChannelCount(2), 0));
                if (pManager->setConfig(config) == SoundDeviceStatus::Ok) {
                    now = pSystem->getDisplayName();
                }
            },
            2000);
    if (!now.isEmpty()) {
        qInfo() << "Zydek: no audio output was open; playing to" << now;
    }
#endif
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
            if (standardMidi()) {
                standardPad(parts[1].toInt(), parts[2].toInt(), parts[0] == "d");
            } else {
                press(parts[1].toInt(), parts[2].toInt(), parts[0] == "d");
            }
        }
        return;
    }
    const QJsonObject m = QJsonDocument::fromJson(text).object();
    if (standardMidi()) {
        if (m.contains(QStringLiteral("sub"))) {   // remembered for the ZyDeck layout; nothing to ask here
            for (const QJsonValue& v : m.value(QStringLiteral("sub")).toArray()) {
                const QJsonArray s = v.toArray();
                m_subscriptions.insert(s[0].toString() + QLatin1Char(',') + s[1].toString(), s[2].toInt());
            }
            return;
        }
        standardFromPage(m);
        return;
    }
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
    if (!m_viaCable && !m_forceCable && m_linkUp && m_pLink) {
        m_pLink->write("M " + data.toHex() + '\n');
        if (m_pLink->bytesToWrite() > 4 * 1024 * 1024) {
            // the computer stopped taking it (gone from the Wi-Fi without closing): start over, later
            QTimer::singleShot(0, this, [this] { linkDrop(true); });
        }
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

// ---- FX panel ----------------------------------------------------------------------------------------
// Mixxx's effects belong to the main thread: these wait for it there.

namespace {
/// The effect's knob measured in beats: its slot number (1-based) and range, or 0.
int beatsKnob(const EffectSlotPointer& pSlot, double* pMin, double* pMax, double* pValue) {
    if (!pSlot || !pSlot->getManifest()) {
        return 0;
    }
    const unsigned int knobs = pSlot->numParameters(EffectParameterType::Knob);
    for (unsigned int k = 0; k < knobs; ++k) {
        const EffectParameterSlotBasePointer pParam = pSlot->getEffectParameterSlot(EffectParameterType::Knob, k);
        const EffectManifestParameterPointer pManifest = pParam ? pParam->getManifest() : EffectManifestParameterPointer();
        if (pManifest && pManifest->unitsHint() == EffectManifestParameter::UnitsHint::Beats) {
            *pMin = pManifest->getMinimum();
            *pMax = pManifest->getMaximum();
            *pValue = ControlObject::get(ConfigKey(pSlot->getGroup(), QStringLiteral("parameter%1").arg(k + 1)));
            return static_cast<int>(k) + 1;
        }
    }
    // Flanger's speed is in beats too (1/4 to 32 a sweep, with a grid), but Mixxx doesn't say so: its first knob
    if (pSlot->getManifest()->id() == QLatin1String("org.mixxx.effects.flanger") && knobs > 0) {
        const EffectParameterSlotBasePointer pParam = pSlot->getEffectParameterSlot(EffectParameterType::Knob, 0);
        const EffectManifestParameterPointer pManifest = pParam ? pParam->getManifest() : EffectManifestParameterPointer();
        if (pManifest) {
            *pMin = pManifest->getMinimum();
            *pMax = pManifest->getMaximum();
            *pValue = ControlObject::get(ConfigKey(pSlot->getGroup(), QStringLiteral("parameter1")));
            return 1;
        }
    }
    return 0;
}
} // namespace

QJsonObject Hub::fxStatus() const {
    const std::shared_ptr<EffectsManager> pEffects = mixxx::qml::QmlEffectsManagerProxy::registeredManager();
    if (!pEffects) {
        return {{"ok", false}, {"error", "Mixxx isn't ready"}};
    }
    QJsonObject out{{"ok", false}, {"error", "Mixxx is busy: try again"}};
    runOnMain(
            qApp,
            [&] {
                QJsonArray names;
                for (const EffectManifestPointer& pManifest : pEffects->getVisibleEffectsList()->getList()) {
                    names.append(pManifest->displayName().isEmpty() ? pManifest->name() : pManifest->displayName());
                }
                QJsonArray units;
                for (int u = 0; u < 4; ++u) {
                    const EffectChainPointer pChain = pEffects->getStandardEffectChain(u);
                    QJsonArray slotList;
                    if (pChain) {
                        for (const EffectSlotPointer& pSlot : pChain->getEffectSlots()) {
                            double min = 0, max = 0, value = 0;
                            const int k = beatsKnob(pSlot, &min, &max, &value);
                            const EffectManifestPointer pManifest = pSlot->getManifest();
                            slotList.append(QJsonObject{
                                    {"name", pManifest ? (pManifest->displayName().isEmpty() ? pManifest->name() : pManifest->displayName()) : QString()},
                                    {"beats", k ? QJsonValue(QJsonObject{{"min", min}, {"max", max}, {"value", value}}) : QJsonValue()}});
                        }
                    }
                    units.append(QJsonObject{{"unit", u + 1}, {"slots", slotList}});
                }
                out = {{"ok", true}, {"effects", names}, {"units", units}};
            });
    return out;
}

QJsonObject Hub::setFxBeats(int unit, int slot, double beats) {
    const std::shared_ptr<EffectsManager> pEffects = mixxx::qml::QmlEffectsManagerProxy::registeredManager();
    if (!pEffects || unit < 1 || unit > 4 || beats <= 0) {
        return {{"ok", false}, {"error", "No such effect unit"}};
    }
    int done = 0;
    runOnMain(
            qApp,
            [&] {
                const EffectChainPointer pChain = pEffects->getStandardEffectChain(unit - 1);
                if (!pChain) {
                    return;
                }
                const QList<EffectSlotPointer>& slotList = pChain->getEffectSlots();
                for (int i = 0; i < slotList.size(); ++i) {
                    if (slot > 0 && i != slot - 1) {
                        continue;
                    }
                    double min = 0, max = 0, value = 0;
                    if (const int k = beatsKnob(slotList[i], &min, &max, &value)) {
                        ControlObject::set(ConfigKey(slotList[i]->getGroup(), QStringLiteral("parameter%1").arg(k)),
                                std::clamp(beats, min, max));
                        ++done;
                    }
                }
            });
    return done ? QJsonObject{{"ok", true}, {"set", done}}
                : QJsonObject{{"ok", false}, {"error", "This effect has no beat length"}};
}

// ---- stem FX -----------------------------------------------------------------------------------------
// Mixxx applies a stem's volume and mute before its effects: muted, an echo on it rings on. A stem is
// [ChannelN_StemS]: 1 drums, 2 bass, 3 other, 4 vocals. The unit's slot 1 holds Echo, slot 2 Pitch Shift.

namespace {
const QList<int> kStemFxStems[4] = {{4}, {1, 2, 3}, {1, 2, 3}, {1}};
QString stemGroup(int deck, int stem) {
    return QStringLiteral("[Channel%1_Stem%2]").arg(deck + 1).arg(stem);
}
QString unitSlot(int unit, int slot) {
    return QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]").arg(unit).arg(slot);
}
/// The slot's knob for the effect's parameter id: its parameterK number, or 0.
int knobFor(const EffectSlotPointer& pSlot, const QString& id) {
    const unsigned int knobs = pSlot ? pSlot->numParameters(EffectParameterType::Knob) : 0;
    for (unsigned int k = 0; k < knobs; ++k) {
        const EffectParameterSlotBasePointer pParam = pSlot->getEffectParameterSlot(EffectParameterType::Knob, k);
        if (pParam && pParam->getManifest() && pParam->getManifest()->id() == id) {
            return static_cast<int>(k) + 1;
        }
    }
    return 0;
}
} // namespace

void Hub::checkStemsCrate() {
    if (!m_pLiveStems) {
        return;
    }
    int crate = 0;
    const QList<int> ids = m_library.crateTrackIds(QStringLiteral("Stems"), &crate);
    const QSet<int> now(ids.cbegin(), ids.cend());
    if (m_stemsCrateRead) {
        for (int id : std::as_const(m_stemsCrate)) {
            if (!now.contains(id)) {   // taken out
                m_pLiveStems->forgetOurs(id);
                m_stemsCrateAsked.remove(id);
            }
        }
    }
    m_stemsCrate = now;
    m_stemsCrateRead = true;
    QList<int> wanted;
    for (int id : now) {
        if (!m_stemsCrateAsked.contains(id) && m_pLiveStems->stemFileFor(id).isEmpty()) {
            wanted.append(id);
            m_stemsCrateAsked.insert(id);
        }
    }
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (wanted.isEmpty() || !pLibrary) {
        return;
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    QList<TrackPointer> tracks;
    runOnMain(pCollection, [pCollection, wanted, &tracks] {
        for (int id : wanted) {
            if (const TrackPointer pTrack = pCollection->getTrackById(TrackId(QVariant(id)))) {
                tracks.append(pTrack);
            }
        }
    });
    for (const TrackPointer& pTrack : std::as_const(tracks)) {
        m_pLiveStems->request(pTrack, false);
    }
}

QJsonObject Hub::stemsCrateStatus(int trackId) {
    int crate = 0;
    const QList<int> ids = m_library.crateTrackIds(QStringLiteral("Stems"), &crate);
    int ready = 0;
    for (int id : ids) {
        ready += m_pLiveStems && !m_pLiveStems->stemFileFor(id).isEmpty();
    }
    return {{"ok", true}, {"crate", crate}, {"inCrate", ids.contains(trackId)},
            {"hasStems", m_pLiveStems && trackId > 0 && !m_pLiveStems->stemFileFor(trackId).isEmpty()},
            {"tracks", ids.size()}, {"ready", ready},
            {"job", m_pLiveStems ? m_pLiveStems->status() : QJsonObject()}};
}

QJsonArray Hub::stemFxJson() const {
    QJsonArray out;
    for (const StemFxRun& run : m_stemFx) {
        out.append(run.fx);
    }
    return out;
}

void Hub::stemFxEnd(int deck, int unit) {
    StemFxRun& run = m_stemFx[deck];
    ++run.serial;
    run.fx = -1;
    for (int s = 1; s <= 4; ++s) {
        ControlObject::set(ConfigKey(QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit),
                                   QStringLiteral("group_%1_enable").arg(stemGroup(deck, s))),
                0);
    }
    ControlObject::set(ConfigKey(unitSlot(unit, 1), QStringLiteral("enabled")), 0);
    ControlObject::set(ConfigKey(unitSlot(unit, 2), QStringLiteral("enabled")), 0);
    emitJson({{"t", "sfx"}, {"d", deck}, {"f", -1}});
}

QJsonObject Hub::stemFx(int deck, int fx) {
    if (deck < 0 || deck >= kNumDecks || fx < 0 || fx > 3) {
        return {{"ok", false}, {"error", "No such deck or effect"}};
    }
    const QString group = QStringLiteral("[Channel%1]").arg(deck + 1);
    if (ControlObject::get(ConfigKey(group, QStringLiteral("stem_count"))) < 4) {
        return {{"ok", false}, {"error", "This track has no stems"}};
    }
    const int unit = deck % 2 == 0 ? 3 : 4;
    const QString unitGroup = QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit);
    const QList<int>& stems = kStemFxStems[fx];
    if (m_stemFx[deck].fx >= 0) {   // a second tap (this pad or another): the stems back, the effect off
        const int was = m_stemFx[deck].fx;
        stemFxEnd(deck, unit);
        for (int s : kStemFxStems[was]) {
            ControlObject::set(ConfigKey(stemGroup(deck, s), QStringLiteral("volume")), 1);
            ControlObject::set(ConfigKey(stemGroup(deck, s), QStringLiteral("mute")), 0);
        }
        return {{"ok", true}, {"back", true}};
    }
    const std::shared_ptr<EffectsManager> pEffects = mixxx::qml::QmlEffectsManagerProxy::registeredManager();
    if (!pEffects) {
        return {{"ok", false}, {"error", "Mixxx isn't ready"}};
    }
    int delayK = 0, feedbackK = 0, sendK = 0, quantizeK = 0, pitchK = 0, rangeK = 0;
    const bool loaded = runOnMain(qApp, [&] {
        const EffectChainPointer pChain = pEffects->getStandardEffectChain(unit - 1);
        const QList<EffectSlotPointer> slotList = pChain ? pChain->getEffectSlots() : QList<EffectSlotPointer>();
        if (slotList.size() < 2) {
            return;
        }
        const auto ensure = [&](const EffectSlotPointer& pSlot, const QString& id) {
            if (!pSlot->getManifest() || pSlot->getManifest()->id() != id) {
                pSlot->loadEffectWithDefaults(pEffects->getBackendManager()->getManifest(id, EffectBackendType::BuiltIn));
            }
        };
        ensure(slotList[0], QStringLiteral("org.mixxx.effects.echo"));
        ensure(slotList[1], QStringLiteral("org.mixxx.effects.pitchshift"));
        delayK = knobFor(slotList[0], QStringLiteral("delay_time"));
        feedbackK = knobFor(slotList[0], QStringLiteral("feedback_amount"));
        sendK = knobFor(slotList[0], QStringLiteral("send_amount"));
        quantizeK = knobFor(slotList[0], QStringLiteral("quantize"));
        pitchK = knobFor(slotList[1], QStringLiteral("pitch"));
        rangeK = knobFor(slotList[1], QStringLiteral("range"));
    });
    const bool brake = fx == 2;
    if (!loaded || (brake ? !pitchK : !(delayK && feedbackK && sendK))) {
        return {{"ok", false}, {"error", "Couldn't set up the effect"}};
    }
    const auto param = [](int unitNumber, int slot, int k, double v) {
        if (k) {
            ControlObject::set(ConfigKey(unitSlot(unitNumber, slot), QStringLiteral("parameter%1").arg(k)), v);
        }
    };
    StemFxRun& run = m_stemFx[deck];
    run.fx = fx;
    const int serial = ++run.serial;
    const double bpm = ControlObject::get(ConfigKey(group, QStringLiteral("bpm")));
    const int beatMs = static_cast<int>(60000.0 / (bpm > 40 && bpm < 300 ? bpm : 120.0));
    // the unit plays just these stems: their dry sound on (echo) or replaced (the brake's pitch dive)
    ControlObject::set(ConfigKey(unitGroup, QStringLiteral("enabled")), 1);
    ControlObject::set(ConfigKey(unitGroup, QStringLiteral("mix")), 1);
    ControlObject::set(ConfigKey(unitGroup, QStringLiteral("mix_mode")), brake ? 0 : 1);   // D/W : D+W
    for (int s = 1; s <= 4; ++s) {
        ControlObject::set(ConfigKey(unitGroup, QStringLiteral("group_%1_enable").arg(stemGroup(deck, s))),
                stems.contains(s) ? 1 : 0);
    }
    emitJson({{"t", "sfx"}, {"d", deck}, {"f", fx}});
    if (!brake) {
        // Echo, half a beat apart, quantized: a beat of it, then the stems drop out and their echo rings on
        param(unit, 1, delayK, 0.5);
        param(unit, 1, feedbackK, 0.6);
        param(unit, 1, sendK, 1.0);
        param(unit, 1, quantizeK, 1);
        ControlObject::set(ConfigKey(unitSlot(unit, 2), QStringLiteral("enabled")), 0);
        ControlObject::set(ConfigKey(unitSlot(unit, 1), QStringLiteral("enabled")), 1);
        QTimer::singleShot(beatMs, this, [this, deck, stems, serial] {
            if (m_stemFx[deck].serial != serial) {
                return;
            }
            for (int s : stems) {
                ControlObject::set(ConfigKey(stemGroup(deck, s), QStringLiteral("mute")), 1);
            }
        });
        QTimer::singleShot(beatMs * 9, this, [this, deck, unit, serial] {   // 0.6^8: the echo has faded
            if (m_stemFx[deck].serial != serial) {
                return;
            }
            ++m_stemFx[deck].serial;   // the stems stay out (fx stays set: the next tap brings them back)
            ControlObject::set(ConfigKey(unitSlot(unit, 1), QStringLiteral("enabled")), 0);
            for (int s = 1; s <= 4; ++s) {
                ControlObject::set(ConfigKey(QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit),
                                           QStringLiteral("group_%1_enable").arg(stemGroup(deck, s))),
                        0);
            }
        });
    } else {
        // The brake: over a beat, the pitch dives an octave as the stems fade, then they're out
        param(unit, 2, rangeK, 1.0);   // the knob's ends = one octave
        param(unit, 2, pitchK, 0);
        ControlObject::set(ConfigKey(unitSlot(unit, 1), QStringLiteral("enabled")), 0);
        ControlObject::set(ConfigKey(unitSlot(unit, 2), QStringLiteral("enabled")), 1);
        const int steps = std::max(4, beatMs / 25);
        for (int i = 1; i <= steps; ++i) {
            QTimer::singleShot(beatMs * i / steps, this, [this, deck, unit, stems, serial, i, steps, pitchK, param] {
                if (m_stemFx[deck].serial != serial) {
                    return;
                }
                const double t = static_cast<double>(i) / steps;
                param(unit, 2, pitchK, -t);
                for (int s : stems) {
                    ControlObject::set(ConfigKey(stemGroup(deck, s), QStringLiteral("volume")), 1 - t);
                }
                if (i == steps) {   // out: muted, back at full volume for when they return
                    ++m_stemFx[deck].serial;
                    for (int s : stems) {
                        ControlObject::set(ConfigKey(stemGroup(deck, s), QStringLiteral("mute")), 1);
                        ControlObject::set(ConfigKey(stemGroup(deck, s), QStringLiteral("volume")), 1);
                    }
                    param(unit, 2, pitchK, 0);
                    ControlObject::set(ConfigKey(unitSlot(unit, 2), QStringLiteral("enabled")), 0);
                    for (int s = 1; s <= 4; ++s) {
                        ControlObject::set(ConfigKey(QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit),
                                                   QStringLiteral("group_%1_enable").arg(stemGroup(deck, s))),
                                0);
                    }
                }
            });
        }
    }
    return {{"ok", true}, {"fx", fx}};
}

// ---- live stems --------------------------------------------------------------------------------------

namespace {
bool isStemsFile(const QString& location) {
    return location.endsWith(QLatin1String(".stem.mp4"), Qt::CaseInsensitive) ||
            location.endsWith(QLatin1String(".stem.m4a"), Qt::CaseInsensitive);
}
} // namespace

bool Hub::isLiveStems(const QString& location) const {
    return location.contains(QLatin1String("/ZyDeck Stems/")) ||
            (m_pLiveStems && m_pLiveStems->originalFor(location) > 0);
}

void Hub::deckTrackLoaded(int deck, const TrackPointer& pTrack) {
    const QString group = QStringLiteral("[Channel%1]").arg(deck + 1);
    if (m_stemFx[deck].fx >= 0) {   // a new track: its stems start un-muted (Mixxx's stem_auto_reset)
        stemFxEnd(deck, deck % 2 == 0 ? 3 : 4);
    }
    // Its waveform and beats may take a while (Mixxx analyses one track at a time, and after a restart there can be a
    // queue): the pages wait for this rather than giving up
    disconnect(m_waveformDone[deck]);
    if (pTrack) {
        const int id = pTrack->getId().toVariant().toInt();
        const QPointer<Track> pWeak(pTrack.get());
        m_waveformDone[deck] = connect(pTrack.get(), &Track::waveformUpdated, this, [this, deck, id, pWeak] {
            const ConstWaveformPointer pWaveform = pWeak ? pWeak->getWaveform() : ConstWaveformPointer();
            if (pWaveform && pWaveform->getDataSize() >= 2 && pWaveform->getCompletion() >= pWaveform->getDataSize()) {
                disconnect(m_waveformDone[deck]);
                emitJson({{"t", "wf"}, {"id", id}});
            }
        });
    }
    DeckStems& ds = m_deckStems[deck];
    if (!pTrack) {
        ds = {};
        emitDeckStems(deck);
        return;
    }
    const QString location = pTrack->getLocation();
    const bool liveStems = isLiveStems(location);
    if (m_optimize.running) {   // the audio test's load: not the user's, no stems made for it
        ds = {pTrack->getId().toVariant().toInt(), {}, 0, {}};
        return;
    }
    if (!liveStems) {   // (its stems stand in for the original)
        m_library.noteLoaded(pTrack->getId().toVariant().toInt());
    }
    // a swap (to the stems, or back to the original) carries on where the deck was
    const auto seek = m_pendingSeek.find(group);
    if (seek == m_pendingSeek.end()) {
        applyDeckDefaults(deck);   // a new track (a swap keeps the deck as it was)
    } else {
        const PendingSeek pending = *seek;
        m_pendingSeek.erase(seek);
        if (pending.location == location ||
                QFileInfo(pending.location).canonicalFilePath() == QFileInfo(location).canonicalFilePath()) {
            ControlObject::set(ConfigKey(group, QStringLiteral("playposition")), pending.position);
            if (pending.playing) {
                ControlObject::set(ConfigKey(group, QStringLiteral("play")), 1);
            }
        }
    }
    if (liveStems) {   // the stems took over
        ds = {pTrack->getId().toVariant().toInt(), QStringLiteral("on"), 1, {}};
        emitDeckStems(deck);
        return;
    }
    ds = {pTrack->getId().toVariant().toInt(), {}, 0, {}};
    if (!m_pLiveStems || isStemsFile(location)) {   // a stems file of its own: nothing to do
        emitDeckStems(deck);
        return;
    }
    // samples (and anything this short) don't need stems
    if (location.contains(QLatin1String("/ZyDeck Samples/")) || pTrack->getDuration() < 45) {
        emitDeckStems(deck);
        return;
    }
    const QString cached = m_pLiveStems->stemFileFor(ds.trackId);
    if (!cached.isEmpty()) {   // made before: straight in, from its start (the deck has no position of its own yet)
        ds.state = QStringLiteral("ready");
        if (m_liveStemsAuto) {
            swapToStems(deck, false);
        } else {
            emitDeckStems(deck);   // live stems off: the STEM pads offer them
        }
        return;
    }
    if (m_liveStemsAuto) {
        ds.state = QStringLiteral("making");
        m_pLiveStems->request(pTrack);
    }
    emitDeckStems(deck);
}

void Hub::liveStemsProgress(int trackId, double fraction) {
    for (int i = 0; i < kNumDecks; ++i) {
        DeckStems& ds = m_deckStems[i];
        if (ds.trackId == trackId && ds.state == QLatin1String("making") && fraction - ds.progress >= 0.02) {
            ds.progress = fraction;
            emitDeckStems(i);
        }
    }
}

void Hub::liveStemsFinished(int trackId, const QString& path, const QString& error) {
    for (int i = 0; i < kNumDecks; ++i) {
        DeckStems& ds = m_deckStems[i];
        if (ds.trackId != trackId || ds.state != QLatin1String("making")) {
            continue;
        }
        if (path.isEmpty()) {
            ds.state = QStringLiteral("failed");
            ds.error = error;
            emitDeckStems(i);
            continue;
        }
        ds.state = QStringLiteral("ready");
        ds.progress = 1;
        const bool playing = ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(i + 1), QStringLiteral("play"))) > 0;
        if (playing) {
            emitDeckStems(i);   // the pages offer the swap: it makes a short gap
        } else {
            swapToStems(i, true);
        }
    }
}

QJsonObject Hub::swapToStems(int deck, bool keepPosition) {
    if (deck < 0 || deck >= kNumDecks || !m_pLiveStems) {
        return {{"ok", false}, {"error", "No such deck"}};
    }
    const QString path = m_pLiveStems->stemFileFor(m_deckStems[deck].trackId);
    if (path.isEmpty()) {
        return {{"ok", false}, {"error", "Its stems aren't ready"}};
    }
    const QString group = QStringLiteral("[Channel%1]").arg(deck + 1);
    if (keepPosition) {   // where the deck is now, playing or not, once the stems load
        m_pendingSeek.insert(group,
                {ControlObject::get(ConfigKey(group, QStringLiteral("playposition"))),
                        ControlObject::get(ConfigKey(group, QStringLiteral("play"))) > 0,
                        path});
    } else {
        m_pendingSeek.remove(group);
    }
    // Not within half a second of the deck's last load either (see CloneDeckOnLoadDoubleTap in start())
    const int expectId = m_deckStems[deck].trackId;
    QTimer::singleShot(keepPosition ? 0 : 700, this, [this, deck, path, group, expectId] {
        if (m_deckStems[deck].trackId != expectId) {
            return;   // something else was loaded meanwhile
        }
        if (m_pLiveStems) {   // the stems version has the original's cues as they are now (set or edited since)
            m_pLiveStems->syncCues(expectId, path, true);
        }
        if (!m_library.loadLocation(path, QStringLiteral("deck%1").arg(deck + 1), true).value(QStringLiteral("ok")).toBool()) {
            m_pendingSeek.remove(group);
        }
    });
    return {{"ok", true}};
}

QJsonObject Hub::swapToOriginal(int deck) {
    if (deck < 0 || deck >= kNumDecks || !m_pLiveStems) {
        return {{"ok", false}, {"error", "No such deck"}};
    }
    const QString group = QStringLiteral("[Channel%1]").arg(deck + 1);
    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
    if (!pTrack || !isLiveStems(pTrack->getLocation())) {
        return {{"ok", false}, {"error", "Not playing live stems"}};
    }
    // Only when the stems version sounds like the original: every stem playing, at full volume
    const int stems = static_cast<int>(ControlObject::get(ConfigKey(group, QStringLiteral("stem_count"))));
    for (int s = 1; s <= stems && s <= 4; ++s) {
        const QString stemGroup = QStringLiteral("[Channel%1_Stem%2]").arg(deck + 1).arg(s);
        if (ControlObject::get(ConfigKey(stemGroup, QStringLiteral("mute"))) > 0 ||
                ControlObject::get(ConfigKey(stemGroup, QStringLiteral("volume"))) < 0.99) {
            return {{"ok", false}, {"error", "Its stems are in use"}};
        }
    }
    const int original = m_pLiveStems->originalFor(pTrack->getLocation());
    const QString path = original > 0 ? m_library.location(original) : QString();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return {{"ok", false}, {"error", "The original isn't there any more"}};
    }
    m_pendingSeek.insert(group,
            {ControlObject::get(ConfigKey(group, QStringLiteral("playposition"))),
                    ControlObject::get(ConfigKey(group, QStringLiteral("play"))) > 0,
                    path});
    m_pLiveStems->syncCues(original, pTrack->getLocation(), false);   // cues set on the stems come along
    if (!m_library.loadLocation(path, QStringLiteral("deck%1").arg(deck + 1), true).value(QStringLiteral("ok")).toBool()) {
        m_pendingSeek.remove(group);
        return {{"ok", false}, {"error", "Couldn't load the original"}};
    }
    return {{"ok", true}};
}

// ---- lining the waveform up by ear -----------------------------------------------------------------------
// A test pattern played through Mixxx on the preview deck, in the headphones (the buffer, the output, Bluetooth,
// and to a linked device the link, as the cue you mix by): a rising C major arpeggio, C E G C, one note a beat at 120 BPM from
// 0.5 s, 40 s long. The page knows where each note is, and the listener taps on them, starting on a low C: the bar
// makes even a delay of more than a beat unmistakable.

QString Hub::calibrationFile() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("ZyDeck calibration (C E G C).wav"));
    if (QFileInfo::exists(path)) {
        return path;
    }
    constexpr int kRate = 44100, kSeconds = 40;
    constexpr double kNotes[4] = {261.63, 329.63, 392.00, 523.25};   // C4 E4 G4 C5
    constexpr double kPi = 3.14159265358979323846;
    const int frames = kRate * kSeconds;
    QByteArray pcm(frames * 4, '\0');
    auto* p = reinterpret_cast<qint16*>(pcm.data());
    for (int k = 0;; ++k) {
        const double t0 = 0.5 + k * 0.5;
        const int f0 = static_cast<int>(t0 * kRate);
        if (f0 >= frames) {
            break;
        }
        const double f = kNotes[k % 4];
        for (int i = 0; i < kRate / 4 && f0 + i < frames; ++i) {   // 250 ms: a sharp start, a quick fall
            const double t = static_cast<double>(i) / kRate;
            const double env = std::min(1.0, t / 0.002) * std::exp(-t / 0.06);
            const double v = env * (0.62 * std::sin(2 * kPi * f * t) + 0.18 * std::sin(4 * kPi * f * t));
            const auto s16 = static_cast<qint16>(std::lround(v * 30000));
            p[2 * (f0 + i)] = s16;
            p[2 * (f0 + i) + 1] = s16;
        }
    }
    QByteArray wav;
    auto u32 = [&wav](quint32 v) { for (int i = 0; i < 4; ++i) wav.append(static_cast<char>((v >> (8 * i)) & 0xff)); };
    auto u16 = [&wav](quint16 v) { wav.append(static_cast<char>(v & 0xff)); wav.append(static_cast<char>(v >> 8)); };
    wav.append("RIFF"); u32(36 + pcm.size()); wav.append("WAVEfmt ");
    u32(16); u16(1); u16(2); u32(kRate); u32(kRate * 4); u16(4); u16(16);
    wav.append("data"); u32(pcm.size()); wav.append(pcm);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(wav) != wav.size()) {
        return {};
    }
    return path;
}

QJsonObject Hub::calibrate(bool on) {
    // On the preview deck: in the headphones only (no one else hears it), and no deck or sampler taken
    const auto key = [](const QString& g, const char* k) { return ConfigKey(g, QLatin1String(k)); };
    if (!on) {
        if (!m_calib.group.isEmpty()) {
            const QString g = m_calib.group;
            m_calib.group.clear();
            ControlObject::set(key(g, "play"), 0);
            const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(g);
            if (pTrack && pTrack->getLocation() == calibrationFile()) {
                ControlObject::set(key(g, "eject"), 1);
                ControlObject::set(key(g, "eject"), 0);
                m_library.removeTrack(pTrack->getId().toVariant().toInt(), false);   // not in the library
            }
        }
        return {{"ok", true}};
    }
    if (m_controllerMode) {
        return {{"ok", false}, {"error", "Not in controller mode: calibrate on the computer's Mixxx"}};
    }
    if (audioStatus().value(QStringLiteral("headphones")).toString().isEmpty()) {
        return {{"ok", false}, {"error", "The test pattern plays in the headphones: set a headphones output first (Settings › Audio)"}};
    }
    const QString file = calibrationFile();
    if (file.isEmpty()) {
        return {{"ok", false}, {"error", "Couldn't make the test pattern"}};
    }
    const QJsonObject r = m_library.loadLocation(file, QStringLiteral("preview"), true);
    if (!r.value(QStringLiteral("ok")).toBool()) {
        return {{"ok", false}, {"error", r.value(QStringLiteral("error")).toString(QStringLiteral("Couldn't load the test pattern"))}};
    }
    const QString g = r.value(QStringLiteral("group")).toString();
    m_calib.group = g;
    QTimer::singleShot(900, this, [this, g, key] {
        if (m_calib.group == g) {
            ControlObject::set(key(g, "rate"), 0);
            ControlObject::set(key(g, "playposition"), 0);
            ControlObject::set(key(g, "play"), 1);
        }
    });
    return {{"ok", true}, {"group", g}, {"first", 0.5}, {"beat", 0.5}, {"bar", 4}, {"seconds", 40}};
}

QJsonArray Hub::deckDefaultsJson() const {
    return {m_deckDefaults[0], m_deckDefaults[1], m_deckDefaults[2]};
}

void Hub::applyDeckDefaults(int deck) {
    static const char* const kControls[3] = {"keylock", "quantize", "slip_enabled"};
    const QString group = QStringLiteral("[Channel%1]").arg(deck + 1);
    for (int i = 0; i < 3; ++i) {
        if (m_deckDefaults[i] >= 0) {
            ControlObject::set(ConfigKey(group, QLatin1String(kControls[i])), m_deckDefaults[i]);
        }
    }
}

void Hub::emitDeckStems(int deck) {
    const DeckStems& ds = m_deckStems[deck];
    emitJson({{"t", "lstem"}, {"d", deck}, {"s", ds.state}, {"p", ds.progress}, {"e", ds.error}, {"pc", ds.onPc}});
}

QJsonArray Hub::deckStemsJson() const {
    QJsonArray out;
    for (const DeckStems& ds : m_deckStems) {
        out.append(QJsonObject{{"s", ds.state}, {"p", ds.progress}, {"e", ds.error}, {"pc", ds.onPc}});
    }
    return out;
}

QJsonObject Hub::liveStemsStatus() const {
    QJsonObject out{{"available", m_pLiveStems != nullptr}, {"auto", m_liveStemsAuto}, {"decks", deckStemsJson()}};
    if (m_pLiveStems) {
        out.insert(QStringLiteral("job"), m_pLiveStems->status());
    }
    return out;
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
                if (message.size() < 65536) {   // a SysEx without its end can't grow without bounds
                    message.append(c);
                } else {
                    sysex = false;
                    message.clear();
                }
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
    if (state == 2 && !wasReady) {
        m_usbReadyMs = m_clock.elapsed();   // (remoteTick's clock)
    }
    if (state != 2) {
        m_usbParser = MidiParser();
        if (!m_linkUp) {
            m_remoteLive = false;
        }
        if (state == 0) {
            m_linkOffer = {};   // a new cable, a new offer
            m_offerDeclined = false;
        }
    } else if (!wasReady) {
        m_helloMs = -100000;   // say hello now (linked over Wi-Fi or not: the cable takes over once it answers)
        if (!m_linkUp) {
            resendSubscriptions();
        }
    }
    emitRemote();
    remoteTick();
}

void Hub::setControllerMode(bool on) {
    if (on == m_controllerMode) {
        return;
    }
    m_controllerMode = on;
    choosePath();   // (off: nothing to choose, and the Wi-Fi lock let go)
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
    if (standardMidi()) {
        return;
    }
    for (auto it = m_subscriptions.cbegin(); it != m_subscriptions.cend(); ++it) {
        const int comma = it.key().indexOf(QLatin1Char(','));
        sysex(QList<int>{kSubscribe, it.value()} + encodeName(it.key().left(comma), it.key().mid(comma + 1)));
    }
}

/// The cable works: Android has it as MIDI and the mapping answered over it in the last 3 s
bool Hub::cableActive() const {
    return m_usbState == 2 && m_clock.elapsed() - m_usbRxMs < 3000;
}

/// Which way to talk to the computer now (the pages told, and the subscriptions sent again, when it changes), and the
/// Wi-Fi lock held while it's Wi-Fi
void Hub::choosePath() {
    const bool cable = m_controllerMode && (cableActive() || (m_usbState == 2 && !m_linkUp));
    if (cable != m_viaCable) {
        m_viaCable = cable;
        if (m_controllerMode) {
            resendSubscriptions();
            emitRemote();
        }
    }
#ifdef Q_OS_ANDROID
    QJniObject::callStaticMethod<void>("org/mixxx/ZydekLink", "setLowLatency", "(Z)V",
            static_cast<jboolean>(m_controllerMode && m_linkUp && !m_viaCable));
#endif
}

bool Hub::canSend() const {
    return m_controllerMode ? (m_linkUp || m_usbState == 2) : m_pController != nullptr;
}

bool Hub::live() const {
    if (standardMidi()) {
        return canSend();   // Serato and co. don't answer: being connected is all there is
    }
    return m_controllerMode ? m_remoteLive && canSend() : m_pController != nullptr;
}

QJsonObject Hub::remoteStatus() const {
    QJsonObject out{{"on", m_controllerMode},
            {"usb", m_usbState},
            {"via", !m_controllerMode ? "" : m_viaCable ? "usb" : m_linkUp ? "wifi" : ""},
            {"standby", m_controllerMode && m_viaCable && m_linkUp},   // (Wi-Fi linked too, for when the cable's pulled)
            {"powerSave", m_powerSave},
            {"live", live()},
            {"linking", m_pLink != nullptr && !m_linkUp},
            {"saved", m_linkSaved.value(QStringLiteral("name"))},
            {"layout", m_standardMidi ? "standard" : "zydeck"}};
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
        } else if (nameFits(j)) {
            applyName(j.value(QStringLiteral("s")).toBool(), j.value(QStringLiteral("i")).toInt(), j.value(QStringLiteral("n")));
        }
        return;
    }
    if (viaUsb) {
        m_usbRxMs = m_clock.elapsed();
        if (!m_viaCable) {
            choosePath();   // the cable answers: over to it
        }
    }
    if (viaUsb != m_viaCable) {
        return;   // (ZyDeck Connect sends Mixxx's messages down both: only the path in use is listened to)
    }
    if (standardMidi()) {
        standardFromDj(msg);
        return;
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
    choosePath();
    if (m_usbState == 2 && !cableActive() && now - m_helloMs > 3000) {   // (until the cable answers: ZyDeck Connect
        m_helloMs = now;                                                   // relays it once it knows the device)
        m_forceCable = true;
        sendSysexText(kLinkHello,
                QJsonDocument(QJsonObject{{"v", 1}, {"name", deviceName()}}).toJson(QJsonDocument::Compact));
        m_forceCable = false;
    }
#ifdef Q_OS_ANDROID
    const bool powerSave = QJniObject::callStaticMethod<jboolean>("org/mixxx/ZydekLink", "powerSave", "()Z");
    if (powerSave != m_powerSave) {
        m_powerSave = powerSave;
        emitRemote();
    }
#endif
    if (canSend() && !standardMidi() && now - m_remoteRxMs > 4000) {
        sysex({kPing, 0x7F, 0x7F, 0x7F});   // the mapping echoes it: still there?
        if (m_remoteLive && now - m_remoteRxMs > 10000) {
            m_remoteLive = false;   // Mixxx closed, or the mapping isn't on for this port
            emitRemote();
            m_server.broadcast(QStringLiteral("/ws"), QJsonDocument(snapshot()).toJson(QJsonDocument::Compact));
        }
    }
    // the computer it linked with over Wi-Fi, again: kept linked beside a working cable too, ready for when it's pulled
    if (!m_pLink && !m_linkSaved.isEmpty() && now - m_linkTryMs > 5000) {   // (its own clock: the
        m_linkTryMs = now;                                                                    // cable's hello resets m_helloMs)
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
        if (m_linkBuffer.size() > 4 * 1024 * 1024 && m_linkBuffer.indexOf('\n') < 0) {
            linkDrop(true);   // not ZyDeck Link talking
            return;
        }
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
        if (j.contains(QStringLiteral("n")) && nameFits(j)) {
            applyName(j.value(QStringLiteral("s")).toBool(), j.value(QStringLiteral("i")).toInt(), j.value(QStringLiteral("n")));
        }
    } else if (line.startsWith("OK")) {
        m_linkUp = true;
        m_linkHost = 0;
        m_linkSaved = m_linkTarget;
        m_linkSaved.remove(QStringLiteral("host"));
        saveRemote();
        if (!m_viaCable) {
            m_remoteLive = false;
            m_remoteRxMs = m_clock.elapsed();
            resendSubscriptions();   // to the mapping on ZyDeck Link's port (the durations bring the names)
        }
        choosePath();   // (beside a working cable it waits as the standby)
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
    if (wasUp && !m_viaCable) {
        m_remoteLive = false;
    }
    choosePath();   // (on the cable now, if there is one; the Wi-Fi lock let go)
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
    m_standardMidi = j.value(QStringLiteral("layout")).toString() == QLatin1String("standard");
    if (j.value(QStringLiteral("on")).toBool()) {
        QTimer::singleShot(0, this, [this] { setControllerMode(true); });
    }
}

void Hub::saveRemote() const {
    QSaveFile f(QDir(m_pConfig->getSettingsPath()).filePath(kRemoteFile));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{"on", m_controllerMode},
                                      {"computer", m_linkSaved},
                                      {"layout", m_standardMidi ? "standard" : "zydeck"}})
                        .toJson());
        f.commit();
    }
}

// ---- controller mode: Standard MIDI (Serato, rekordbox, ...) --------------------------------------
// MIDI channels 1-4 = decks 1-4, channel 16 = mixer and samplers. Buttons are notes (127 press, 0 release);
// toggles (play, sync, keylock...) are a press and a release, which the DJ app turns into its own toggle.
namespace {
constexpr int kStdPlay = 0x00, kStdCue = 0x01, kStdSync = 0x02, kStdKeylock = 0x03, kStdQuantize = 0x04,
              kStdPfl = 0x05, kStdJogTouch = 0x08, kStdSyncMaster = 0x09, kStdMute = 0x0A, kStdEject = 0x0B,
              kStdLoopBase = 0x10,   // beatloop 1/8 .. 32 at 0x10..0x18
        kStdReloop = 0x19, kStdLoopHalve = 0x1A, kStdLoopDouble = 0x1B, kStdLoopIn = 0x1C, kStdLoopOut = 0x1D,
              kStdLoopMoveBack = 0x1E, kStdLoopMoveFwd = 0x1F, kStdPads = 0x40,   // + mode * 8 + pad (0x40..0x7F)
        kStdKeyDown = 0x78, kStdKeyUp = 0x79, kStdKeyReset = 0x7A, kStdKeySync = 0x7B;
// CCs on a deck's channel; the tempo is its pitch bend (centre = 0 %, ends = ±8 %)
constexpr int kStdCcVolume = 0x00, kStdCcGain = 0x01, kStdCcHigh = 0x02, kStdCcMid = 0x03, kStdCcLow = 0x04,
              kStdCcFilter = 0x05, kStdCcJog = 0x06, kStdCcNudge = 0x07;
// channel 16
constexpr int kStdGlobal = 15, kStdCcXfader = 0x00, kStdCcMain = 0x01, kStdCcPhones = 0x02, kStdCcHeadMix = 0x03,
              kStdSamplerStop = 0x40;   // sampler i plays on note i, stops on 0x40 + i
const QStringList kStdLoopSizes{"0.125", "0.25", "0.5", "1", "2", "4", "8", "16", "32"};

int stdDeck(const QString& group) {   // [ChannelN] -> 0..3, else -1
    static const QRegularExpression re(QStringLiteral(R"(^\[Channel([1-4])\]$)"));
    const QRegularExpressionMatch m = re.match(group);
    return m.hasMatch() ? m.captured(1).toInt() - 1 : -1;
}
int cc7(double v01) {
    return std::clamp(static_cast<int>(std::lround(v01 * 127)), 0, 127);
}
} // namespace

void Hub::stdSend(int status, int data1, int data2) {
    const char msg[3] = {static_cast<char>(status), static_cast<char>(data1 & 0x7F), static_cast<char>(data2 & 0x7F)};
    sendMidi(QByteArray(msg, 3));
}

void Hub::standardFromPage(const QJsonObject& m) {
    const auto note = [this](int deckOrChannel, int n, bool down) { stdSend(0x90 | deckOrChannel, n, down ? 127 : 0); };
    const auto pulse = [&](int ch, int n) { note(ch, n, true); note(ch, n, false); };
    if (m.contains(QStringLiteral("sync"))) {   // [deck, mode]: 3 = as the master
        const QJsonArray a = m.value(QStringLiteral("sync")).toArray();
        pulse(a[0].toInt(), a[1].toInt() == 3 ? kStdSyncMaster : kStdSync);
        return;
    }
    if (m.contains(QStringLiteral("tempo"))) {   // [deck, overlimit, percent] -> pitch bend, ±8 % at the ends
        const QJsonArray a = m.value(QStringLiteral("tempo")).toArray();
        const int v = std::clamp(8192 + static_cast<int>(std::lround(a[2].toDouble() / 8.0 * 8191)), 0, 16383);
        stdSend(0xE0 | (a[0].toInt() & 3), v & 0x7F, v >> 7);
        return;
    }
    if (m.contains(QStringLiteral("scratch"))) {   // [deck, op, ticks]: touch, turn (relative, 64 = still), let go
        const QJsonArray a = m.value(QStringLiteral("scratch")).toArray();
        const int deck = a[0].toInt() & 3, op = a[1].toInt();
        if (op == 1 || op == 0) {
            note(deck, kStdJogTouch, op == 1);
        } else {
            for (int left = std::clamp(a[2].toInt(), -63 * 64, 63 * 64); left != 0;) {
                const int step = std::clamp(left, -63, 63);
                stdSend(0xB0 | deck, kStdCcJog, 64 + step);
                left -= step;
            }
        }
        return;
    }
    if (!m.contains(QStringLiteral("set"))) {
        return;
    }
    const QJsonArray a = m.value(QStringLiteral("set")).toArray();
    const QString group = a[0].toString(), key = a[1].toString();
    const double v = a[2].toDouble();
    const bool down = v > 0;
    int deck = stdDeck(group);
    if (group == QLatin1String("[Master]")) {
        const int cc = key == QLatin1String("crossfader") ? kStdCcXfader : key == QLatin1String("gain") ? kStdCcMain
                : key == QLatin1String("headGain")                         ? kStdCcPhones
                : key == QLatin1String("headMix")                          ? kStdCcHeadMix
                                                                           : -1;
        if (cc >= 0) {
            stdSend(0xB0 | kStdGlobal, cc, cc7(v));   // the page sends these as 0..1
        }
        return;
    }
    static const QRegularExpression rack(QStringLiteral(R"(^\[(EqualizerRack1|QuickEffectRack1)_\[Channel([1-4])\](_Effect1)?\]$)"));
    if (const QRegularExpressionMatch r = rack.match(group); r.hasMatch()) {
        deck = r.captured(2).toInt() - 1;
        const int cc = key == QLatin1String("parameter3") ? kStdCcHigh : key == QLatin1String("parameter2") ? kStdCcMid
                : key == QLatin1String("parameter1")                                                        ? kStdCcLow
                : key == QLatin1String("super1")                                                            ? kStdCcFilter
                                                                                                            : -1;
        if (cc >= 0) {
            stdSend(0xB0 | deck, cc, cc7(v));
        }
        return;
    }
    static const QRegularExpression sampler(QStringLiteral(R"(^\[Sampler(\d+)\]$)"));
    if (const QRegularExpressionMatch r = sampler.match(group); r.hasMatch()) {
        const int i = r.captured(1).toInt() - 1;
        if (i < 64 && down && (key == QLatin1String("cue_gotoandplay") || key == QLatin1String("start_play"))) {
            pulse(kStdGlobal, i);
        } else if (i < 64 && down && (key == QLatin1String("cue_gotoandstop") || key == QLatin1String("stop"))) {
            pulse(kStdGlobal, kStdSamplerStop + i);
        }
        return;
    }
    static const QRegularExpression stem(QStringLiteral(R"(^\[Channel([1-4])_Stem([1-4])\]$)"));
    if (const QRegularExpressionMatch r = stem.match(group); r.hasMatch()) {
        if (key == QLatin1String("mute")) {   // like the STEM pads: 4 = vocals first
            pulse(r.captured(1).toInt() - 1, kStdPads + 3 * 8 + (4 - r.captured(2).toInt()));
        }
        return;
    }
    if (deck < 0) {
        return;
    }
    if (key == QLatin1String("volume") || key == QLatin1String("pregain")) {
        stdSend(0xB0 | deck, key == QLatin1String("volume") ? kStdCcVolume : kStdCcGain, cc7(v));
    } else if (key == QLatin1String("wheel")) {   // nudge: a bend, relative like a jog's outer ring
        stdSend(0xB0 | deck, kStdCcNudge, 64 + std::clamp(static_cast<int>(std::lround(v * 400)), -63, 63));
    } else if (key == QLatin1String("play") || key == QLatin1String("keylock") || key == QLatin1String("quantize") ||
            key == QLatin1String("pfl") || key == QLatin1String("mute") || key == QLatin1String("eject")) {
        if (key == QLatin1String("eject") && !down) {
            return;
        }
        pulse(deck, key == QLatin1String("play") ? kStdPlay : key == QLatin1String("keylock") ? kStdKeylock
                        : key == QLatin1String("quantize")                                  ? kStdQuantize
                        : key == QLatin1String("pfl")                                       ? kStdPfl
                        : key == QLatin1String("mute")                                      ? kStdMute
                                                                                            : kStdEject);
    } else if (key == QLatin1String("cue_default")) {
        note(deck, kStdCue, down);
    } else if (key == QLatin1String("pitch_adjust")) {   // absolute semitones from the page: steps for the app
        const int target = static_cast<int>(std::lround(v));
        while (m_stdKeyShift[deck] < target) {
            pulse(deck, kStdKeyUp);
            m_stdKeyShift[deck]++;
        }
        while (m_stdKeyShift[deck] > target) {
            pulse(deck, kStdKeyDown);
            m_stdKeyShift[deck]--;
        }
    } else if (key == QLatin1String("pitch_up") || key == QLatin1String("pitch_down")) {   // a key step
        if (down) {
            const bool up = key == QLatin1String("pitch_up");
            pulse(deck, up ? kStdKeyUp : kStdKeyDown);
            m_stdKeyShift[deck] += up ? 1 : -1;
        }
    } else if (key == QLatin1String("reset_key") || key == QLatin1String("sync_key")) {
        if (key == QLatin1String("reset_key") && down) {
            m_stdKeyShift[deck] = 0;
        }
        note(deck, key == QLatin1String("reset_key") ? kStdKeyReset : kStdKeySync, down);
    } else if (key == QLatin1String("loop_move")) {
        if (v != 0) {
            pulse(deck, v > 0 ? kStdLoopMoveFwd : kStdLoopMoveBack);
        }
    } else if (key.startsWith(QLatin1String("beatloop_")) && key.endsWith(QLatin1String("_toggle"))) {
        const int i = kStdLoopSizes.indexOf(key.mid(9, key.size() - 9 - 7));
        if (i >= 0) {
            note(deck, kStdLoopBase + i, down);
        }
    } else {
        const int n = key == QLatin1String("reloop_toggle") ? kStdReloop : key == QLatin1String("loop_halve") ? kStdLoopHalve
                : key == QLatin1String("loop_double")                                                          ? kStdLoopDouble
                : key == QLatin1String("loop_in")                                                              ? kStdLoopIn
                : key == QLatin1String("loop_out")                                                             ? kStdLoopOut
                                                                                                               : -1;
        if (n >= 0) {
            note(deck, n, down);
        }
    }
}

/// The pad pages: samplers (channel 0) on channel 16, deck pads (channels 1-4) at 0x40 + mode * 8 + pad.
void Hub::standardPad(int channel, int note, bool down) {
    if (channel == 0 && note >= 0 && note < 64) {
        stdSend(0x90 | kStdGlobal, note, down ? 127 : 0);
    } else if (channel >= 1 && channel <= kNumDecks && note >= 0 && note < 64) {
        stdSend(0x90 | (channel - 1), kStdPads + note, down ? 127 : 0);
    }
}

/// LEDs from the DJ app (if its mapping sends them) become the pages' state.
void Hub::standardFromDj(const QByteArray& msg) {
    if (msg.size() != 3) {
        return;
    }
    const int status = static_cast<unsigned char>(msg[0]) & 0xF0, ch = static_cast<unsigned char>(msg[0]) & 0x0F;
    const int n = static_cast<unsigned char>(msg[1]);
    const int vel = status == 0x80 ? 0 : static_cast<unsigned char>(msg[2]);
    if (status != 0x90 && status != 0x80) {
        return;
    }
    const auto cv = [this](const QString& name, double value) {
        m_controlValues.insert(name, value);
        emitJson({{"t", "cv"}, {"k", name}, {"v", value}, {"ts", static_cast<double>(m_clock.elapsed())}});
    };
    if (ch == kStdGlobal && n < 64) {
        if (m_samplers[n] != (vel ? 2 : 0)) {
            m_samplers[n] = vel ? 2 : 0;
            emitJson({{"t", "s"}, {"i", n}, {"v", m_samplers[n]}});
        }
        return;
    }
    if (ch >= kNumDecks) {
        return;
    }
    const QString group = QStringLiteral("[Channel%1]").arg(ch + 1);
    switch (n) {
    case kStdPlay: cv(group + QStringLiteral(",play"), vel ? 1 : 0); return;
    case kStdSync: cv(group + QStringLiteral(",sync_enabled"), vel ? 1 : 0); return;
    case kStdKeylock: cv(group + QStringLiteral(",keylock"), vel ? 1 : 0); return;
    case kStdQuantize: cv(group + QStringLiteral(",quantize"), vel ? 1 : 0); return;
    case kStdPfl: cv(group + QStringLiteral(",pfl"), vel ? 1 : 0); return;
    default: break;
    }
    if (n >= kStdPads && n < kStdPads + 8 && m_decks[ch].cues[n - kStdPads] != (vel ? 0xF0A23B + 1 : 0)) {
        m_decks[ch].cues[n - kStdPads] = vel ? 0xF0A23B + 1 : 0;   // a lit hot cue pad (no colour over MIDI)
        emitJson({{"t", "cue"}, {"d", ch}, {"k", n - kStdPads}, {"v", m_decks[ch].cues[n - kStdPads]}});
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

// A name from the computer, if it's for what's loaded there now: it says which track's length it's for ("ms"), so one
// for the track before (answered late, after the next one's) can't stick. (Older ZyDeck Connects don't say.)
bool Hub::nameFits(const QJsonObject& reply) const {
    if (!reply.contains(QStringLiteral("ms"))) {
        return true;
    }
    const QString key = QStringLiteral("[%1%2],duration")
                                .arg(reply.value(QStringLiteral("s")).toBool() ? QStringLiteral("Sampler") : QStringLiteral("Channel"))
                                .arg(reply.value(QStringLiteral("i")).toInt() + 1);
    const auto it = m_controlValues.constFind(key);
    return it != m_controlValues.constEnd() &&
            std::abs(it.value().toDouble() * 1000 - reply.value(QStringLiteral("ms")).toDouble()) <= 1000;
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
                    {"lat", latencySeconds()},
                    {"vlat", visualExtraSeconds()},
                    {"alat", audioSeconds()},
                    {"loff", latencyOffsetMs()},
                    {"bt", routeKind() == QLatin1String("Bluetooth")},
                    {"lstems", deckStemsJson()},
                    {"ssrv", m_stemServer},
                    {"ddef", deckDefaultsJson()},
                    {"notice", QDateTime::currentMSecsSinceEpoch() - m_noticeAt < 30000 ? m_notice : QString()},
                    {"sfx", stemFxJson()},
                    {"rec", recordStatus()},
                    {"cv", m_controlValues}}}};
}

} // namespace zydek
