#include "zydek/zydekhub.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "controllers/zydek/zydekcontroller.h"
#include "mixer/playerinfo.h"
#include "moc_zydekhub.cpp"
#include "track/track.h"
#include "waveform/waveform.h"

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
constexpr int kHello = 0x5F;

const QString kSettingsFile = QStringLiteral("zydek-controller-settings.json");

/// round(v * 1e5) + 2^34 as five 7-bit bytes, LSB first (exact for +-171k in 1e-5 steps).
QList<int> encodeValue(double v) {
    const qint64 n = std::llround(v * 1e5) + (qint64(1) << 34);
    QList<int> out;
    for (int i = 0; i < 5; ++i) {
        out.append(static_cast<int>((n >> (7 * i)) & 0x7F));
    }
    return out;
}

double decodeValue(const QByteArray& bytes) {
    qint64 n = 0;
    for (int i = 0; i < 5 && i < bytes.size(); ++i) {
        n |= qint64(static_cast<unsigned char>(bytes[i]) & 0x7F) << (7 * i);
    }
    return static_cast<double>(n - (qint64(1) << 34)) / 1e5;
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

const QByteArray kIndexPage = QByteArrayLiteral(
        "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Zydek</title><style>body{font:18px system-ui,sans-serif;background:#111;color:#ddd;margin:24px}"
        "a{display:block;color:#ff8a1e;margin:14px 0}</style><h1>Zydek</h1>"
        "<a href=/controller>Controller</a><a href=/settings>Settings and debug</a><a href=/sizes>Size test</a>");

} // namespace

Hub::Hub(UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig) {
    m_server.setHandler([this](const QString& path, const HttpServer::Query& query) {
        return handleHttp(path, query);
    });
    connect(&m_server, &HttpServer::wsOpened, this, &Hub::onWsOpened);
    connect(&m_server, &HttpServer::wsText, this, &Hub::onWsText);
    connect(&m_server, &HttpServer::wsClosed, this, &Hub::onWsClosed);

    QFile f(QDir(m_pConfig->getSettingsPath()).filePath(kSettingsFile));
    if (f.open(QIODevice::ReadOnly)) {
        m_ctlSettings = QJsonDocument::fromJson(f.readAll()).object();
    }
}

void Hub::start() {
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
            path == QLatin1String("/sizes")) {
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
    if (path == QLatin1String("/lan-url")) {
        return json({{"url", lanUrl()}});
    }
    if (path == QLatin1String("/status")) {
        return json({{"live", m_pController != nullptr}});
    }
    return {404, "text/plain", "not found\n", {}};
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
    if (path == QLatin1String("/ctl")) {
        m_ctlRoles.remove(pClient);
        broadcastPresence();
    }
}

void Hub::onWsText(QTcpSocket* pClient, const QString& path, const QByteArray& text) {
    if (path == QLatin1String("/ws")) {
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

// ---- page -> Mixxx --------------------------------------------------------------------------------

void Hub::handlePageMessage(const QByteArray& text) {
    if (!m_pController) {
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
    m_pController->injectSysex(data);
}

void Hub::press(int channel, int note, bool down) {
    // channel 0 = samplers, 1-4 = decks 1-4
    if (channel >= 0 && channel <= kNumDecks && note >= 0 && note < 128) {
        m_pController->injectShortMessage(static_cast<unsigned char>(0x90 | channel),
                static_cast<unsigned char>(note),
                down ? 127 : 0);
    }
}

// ---- Mixxx -> pages -------------------------------------------------------------------------------

void Hub::fromMixxx(const QByteArray& msg) {
    auto byte = [&msg](int i) { return static_cast<unsigned char>(msg[i]); };
    if (msg.size() >= 4 && byte(0) == 0xF0 && byte(1) == 0x7D &&
            (byte(2) == kValue || byte(2) == kHello)) {
        if (byte(2) == kHello) {   // the mapping (re)started: subscriptions must be made again
            for (auto it = m_subscriptions.cbegin(); it != m_subscriptions.cend(); ++it) {
                const int comma = it.key().indexOf(QLatin1Char(','));
                sysex(QList<int>{kSubscribe, it.value()} +
                        encodeName(it.key().left(comma), it.key().mid(comma + 1)));
            }
            return;
        }
        const int end = msg.indexOf('\0', 3);
        if (end < 0) {
            return;
        }
        const QString name = QString::fromLatin1(msg.mid(3, end - 3));
        const double value = decodeValue(msg.mid(end + 1, 5));
        m_controlValues.insert(name, value);
        emitJson({{"t", "cv"}, {"k", name}, {"v", value}});
        return;
    }
    if (msg.size() == 10 && byte(0) == 0xF0 && byte(1) == 0x7D) {
        const int kind = byte(2), idx = byte(3), sub = byte(4);
        const int v = byte(5) | byte(6) << 7 | byte(7) << 14 | byte(8) << 21;
        if (kind == kSamplerLength && idx < kNumSamplers) {
            updateName(true, idx);
        } else if (kind == kDeckLength && idx < kNumDecks) {
            updateName(false, idx);
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

void Hub::updateName(bool sampler, int index) {
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
                    {"live", m_pController != nullptr},
                    {"cv", m_controlValues}}}};
}

} // namespace zydek
