#include "core.h"

#include <QDateTime>
#include <QHostAddress>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTcpSocket>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <cmath>
#include <memory>

namespace zc {

namespace {
bool isPrivate(const QHostAddress& a) {
    static const QRegularExpression p(QStringLiteral(R"(^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.))"));
    return a.protocol() == QAbstractSocket::IPv4Protocol && p.match(a.toString()).hasMatch();
}

bool sameKey(const QByteArray& a, const QByteArray& b) {   // in constant time
    if (a.size() != b.size()) {
        return false;
    }
    char diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<char>(a[i] ^ b[i]);
    }
    return diff == 0 && !a.isEmpty();
}

/// The key, kept in ZyDeck Connect's settings; the first time, the old Python ZyDeck Link's, so a phone or tablet
/// that linked to it before still can.
QString loadKey() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString path = dir + "/connect.json";
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        const QString k = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("key")).toString();
        if (!k.isEmpty()) {
            return k;
        }
    }
    const QString home = QDir::homePath();
#if defined(Q_OS_MACOS)
    const QString old = home + "/Library/Application Support/ZyDeck Link/link.json";
#elif defined(Q_OS_WIN)
    const QString old = qEnvironmentVariable("APPDATA", home) + "/ZyDeck Link/link.json";
#else
    const QString old = qEnvironmentVariable("XDG_CONFIG_HOME", home + "/.config") + "/zydeck-link/link.json";
#endif
    QString key;
    QFile o(old);
    if (o.open(QIODevice::ReadOnly)) {
        key = QJsonDocument::fromJson(o.readAll()).object().value(QStringLiteral("key")).toString();
    }
    if (key.isEmpty()) {
        QByteArray b(12, 0);
        for (char& c : b) {
            c = static_cast<char>(QRandomGenerator::system()->bounded(256));
        }
        key = QString::fromLatin1(b.toHex());
    }
    QDir().mkpath(dir);
    QSaveFile s(path);
    if (s.open(QIODevice::WriteOnly)) {
        s.write(QJsonDocument(QJsonObject{{"key", key}}).toJson());
        s.commit();
        s.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    return key;
}
} // namespace

QStringList lanAddresses() {
    QStringList out;
    QUdpSocket probe;   // no packet is sent: it only picks the outgoing interface
    probe.connectToHost(QHostAddress(QStringLiteral("192.168.0.1")), 9);
    if (probe.waitForConnected(200) && isPrivate(probe.localAddress())) {
        out.append(probe.localAddress().toString());
    }
    static const QRegularExpression virt(QStringLiteral("^(docker|br-|veth|virbr|vmnet|vboxnet|utun|bridge|lxc|zt|tailscale)"));
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces()) {
        if (!(i.flags() & QNetworkInterface::IsUp) || (i.flags() & QNetworkInterface::IsLoopBack) ||
                i.type() == QNetworkInterface::Virtual || virt.match(i.name()).hasMatch()) {
            continue;   // (containers' and VMs' own networks: a phone can't reach those)
        }
        for (const QNetworkAddressEntry& e : i.addressEntries()) {
            if (isPrivate(e.ip()) && !out.contains(e.ip().toString())) {
                out.append(e.ip().toString());
            }
        }
    }
    return out;
}

struct Core::Client {
    QByteArray buffer;
    QString device;
    bool linked = false;
    std::unique_ptr<QFile> upload;   // POST /web/upload: where its body goes, and how much is still to come
    qint64 uploadLeft = 0;
    QUrlQuery uploadQuery;
    void dropUpload() {   // (not all of it came: nothing half-written stays)
        if (upload) {
            upload->close();
            upload->remove();
            upload.reset();
        }
    }
};

Core::Core(QObject* parent)
        : QObject(parent),
          m_key(loadKey()),
          m_midi(new Midi(this)),
          m_hardware(new Hardware(m_midi, this)),
          m_profiler(new Profiler(m_midi, [this] {   // (a linked device's address: the network the profiler watches)
              for (const Device& d : devices()) {
                  if (!d.address.isEmpty()) {
                      return d.address;
                  }
              }
              return QString();
          }, this)) {
    m_name = QSysInfo::machineHostName().section(QLatin1Char('.'), 0, 0);
    if (m_name.isEmpty()) {
        m_name = tr("This computer");
    }
    connect(m_midi, &Midi::fromMixxx, this, &Core::fromMixxx);
    connect(m_midi, &Midi::hardware, m_hardware, &Hardware::feed);
    connect(this, &Core::mixxxMappingStarted, m_hardware, &Hardware::mappingsStarted);
    connect(m_hardware, &Hardware::changed, this, &Core::hardwareOut);
    connect(m_profiler, &Profiler::tell, this, [this](const QJsonObject& control) {   // the session profiler: all the devices
        const QByteArray line = "P " + QJsonDocument(control).toJson(QJsonDocument::Compact) + '\n';
        for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
            if (it.value()->linked) {
                it.key()->write(line);
            }
        }
    });
    connect(m_midi, &Midi::hello, this, [this](const QString& device, const QString& port) {
        seen(device, tr("USB cable"));
        m_midi->reply(port, sysexText(kLinkOffer, offer()));
        m_midi->reply(port, sysexText(zydeck::shared::kHardware, m_hardware->state()));
        activity(device, false);
    });
    connect(m_midi, &Midi::nameAsk, this, [this](const QString& port, const QJsonObject& ask) {
        QThreadPool::globalInstance()->start([this, port, ask] {   // (it can wait a few seconds for a new track)
            m_midi->reply(port, sysexText(kNameReply, nameReply(ask)));
        });
    });
    connect(&m_server, &QTcpServer::newConnection, this, &Core::connection);
    connect(&m_decksWatch, &QTimer::timeout, this, &Core::decksChanged);
    m_decksWatch.start(250);
}

// Mixxx ZyDeck writes zydeck-decks.json as a track loads (and as its hot cues change: their names come along; and as
// its beat grid is edited, saved to the library first: the devices fetch the grid and sections again): each
// new name goes to the devices then, without waiting to be asked (the asking, by the track's length, is for plain Mixxx). Each name carries its track's length ("ms"), so a
// device takes it only for that track, not for the one before or after (an answer arriving late)
void Core::decksChanged() {
    const QFileInfo fi(mixxxDir() + QStringLiteral("/zydeck-decks.json"));
    if (!fi.exists() || fi.lastModified() == m_decksAt) {
        return;
    }
    m_decksAt = fi.lastModified();
    QFile f(fi.filePath());
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject decks = QJsonDocument::fromJson(f.readAll()).object();
    static const QRegularExpression slot(QStringLiteral(R"(^\[(Channel|Sampler)(\d+)\]$)"));
    for (auto it = decks.constBegin(); it != decks.constEnd(); ++it) {
        const QJsonObject t = it.value().toObject();
        const QRegularExpressionMatch m = slot.match(it.key());
        if (!m.hasMatch() || t.isEmpty() || m_decksSent.value(it.key()) == it.value()) {
            continue;   // (an empty slot: the device sees the length go to 0 itself)
        }
        m_decksSent.insert(it.key(), it.value());
        if (m.captured(1) == QLatin1String("Channel")) {   // its sections, for Mixxx ZyDeck's Auto DJ (and the pages)
            const int id = t.value(QStringLiteral("id")).toInt();
            QThreadPool::globalInstance()->start([this, id] { m_waveforms.structure(id); });
        }
        const QJsonObject n{{"s", m.captured(1) == QLatin1String("Sampler")}, {"i", m.captured(2).toInt() - 1},
                {"ms", std::round(t.value(QStringLiteral("seconds")).toDouble() * 1000)},
                {"n", QJsonObject{{"t", t.value(QStringLiteral("title")).toString()},
                              {"a", t.value(QStringLiteral("artist")).toString()}, {"id", t.value(QStringLiteral("id")).toInt()},
                              {"l", t.value(QStringLiteral("cues"))},   // (l: its hot cues' names, as they change too)
                              {"g", t.value(QStringLiteral("grid"))}}}};   // (g: its beat grid's, as it's edited: the devices fetch it again)
        const QByteArray line = "J " + QJsonDocument(n).toJson(QJsonDocument::Compact) + '\n';
        for (auto c = m_clients.cbegin(); c != m_clients.cend(); ++c) {
            if (c.value()->linked) {
                c.key()->write(line);
            }
        }
        m_midi->toDevices(sysexText(kNameReply, n));
    }
}

// The DJ controller's pad modes and decks switch (hardware.h), to every device: its pages follow them if asked to
void Core::hardwareOut(const QJsonObject& state) {
    const QByteArray line = "H " + QJsonDocument(state).toJson(QJsonDocument::Compact) + '\n';
    for (auto c = m_clients.cbegin(); c != m_clients.cend(); ++c) {
        if (c.value()->linked) {
            c.key()->write(line);
        }
    }
    m_midi->toDevices(sysexText(zydeck::shared::kHardware, state));
}

bool Core::listen() {
    return m_server.listen(QHostAddress::Any, kTcpPort);
}

QJsonObject Core::offer() const {
    return {{"name", m_name}, {"hosts", QJsonArray::fromStringList(lanAddresses())}, {"port", kTcpPort}, {"key", m_key}};
}

QString Core::pairUrl() const {
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"), m_name);
    q.addQueryItem(QStringLiteral("hosts"), lanAddresses().join(QLatin1Char(',')));
    q.addQueryItem(QStringLiteral("port"), QString::number(kTcpPort));
    q.addQueryItem(QStringLiteral("key"), m_key);
    return QStringLiteral("zydeck://connect?") + q.toString(QUrl::FullyEncoded);
}

QList<Core::Device> Core::devices() const {
    QList<Device> out;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<qint64> order;
    for (auto it = m_seen.cbegin(); it != m_seen.cend(); ++it) {
        if (it->how == tr("Wi-Fi") || now - it->at < 10000) {
            int i = 0;
            while (i < order.size() && order[i] <= it->first) {
                ++i;
            }
            order.insert(i, it->first);
            out.insert(i, {it.key(), it->how, it->address, it->lastIn, it->lastOut});
        }
    }
    return out;
}

void Core::seen(const QString& device, const QString& how, const QString& address) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto it = m_seen.constFind(device);
    const bool fresh = it == m_seen.cend() || it->how != how || (!address.isEmpty() && it->address != address);
    m_seen.insert(device, {how, now, it == m_seen.cend() ? ++m_arrivals : it->first, now, it == m_seen.cend() ? 0 : it->lastOut,
                                  address.isEmpty() && it != m_seen.cend() ? it->address : address});
    if (fresh) {
        emit changed();
    }
}

void Core::activity(const QString& device, bool in) {
    const auto it = m_seen.find(device);
    if (it != m_seen.end()) {
        (in ? it->lastIn : it->lastOut) = QDateTime::currentMSecsSinceEpoch();
    }
}

// What's on a deck: Mixxx ZyDeck says exactly (zydeck-decks.json, which it writes as a track loads: a moment's wait
// for it); plain Mixxx only tells the track's length, which the library is searched for
QJsonObject Core::nameReply(const QJsonObject& ask) {
    const qint64 ms = static_cast<qint64>(ask.value(QStringLiteral("ms")).toDouble());
    const bool sampler = ask.value(QStringLiteral("s")).toBool();
    const QString group = (sampler ? QStringLiteral("[Sampler%1]") : QStringLiteral("[Channel%1]")).arg(ask.value(QStringLiteral("i")).toInt() + 1);
    QJsonObject n;
    const QString decks = mixxxDir() + QStringLiteral("/zydeck-decks.json");
    for (int attempt = 0; ms > 0 && n.isEmpty() && QFileInfo::exists(decks) && attempt < 8; ++attempt) {
        QFile f(decks);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject t = QJsonDocument::fromJson(f.readAll()).object().value(group).toObject();
            if (!t.isEmpty() && std::abs(t.value(QStringLiteral("seconds")).toDouble() * 1000 - ms) <= 1000) {   // (that track, not the one before)
                n = {{"t", t.value(QStringLiteral("title")).toString()}, {"a", t.value(QStringLiteral("artist")).toString()},
                        {"id", t.value(QStringLiteral("id")).toInt()}, {"l", t.value(QStringLiteral("cues"))}};   // (l: hot cue names)
                break;
            }
        }
        QThread::msleep(250);
    }
    if (n.isEmpty() && ms > 0) {
        n = m_library.name(ms);
    }
    return {{"s", ask.value(QStringLiteral("s")).toBool()}, {"i", ask.value(QStringLiteral("i")).toInt()}, {"ms", ms},
            {"n", n.isEmpty() ? QJsonValue() : QJsonValue(n)}};   // (ms: the track it's for)
}

// /web/profile?do=start|stop|mark (note=)|status|list|get (id=)|delete (id=): profiler.h
QJsonObject Core::profileApi(const QUrlQuery& query, int* status) {
    *status = 200;
    const QString what = query.queryItemValue(QStringLiteral("do"));
    const QString id = query.queryItemValue(QStringLiteral("id"));
    if (what == QLatin1String("start")) {
        m_profiler->start({});
    } else if (what == QLatin1String("stop")) {
        m_profiler->stop(QStringLiteral("asked"));
    } else if (what == QLatin1String("mark")) {
        m_profiler->mark({}, query.queryItemValue(QStringLiteral("note"), QUrl::FullyDecoded));
    } else if (what == QLatin1String("list")) {
        return {{"ok", true}, {"sessions", m_profiler->sessions()}, {"state", m_profiler->state()}};
    } else if (what == QLatin1String("get")) {
        const QByteArray lines = m_profiler->session(id);
        if (lines.isEmpty()) {
            *status = 404;
            return {{"ok", false}, {"error", "No such session"}};
        }
        return {{"_lines", QString::fromUtf8(lines)}};
    } else if (what == QLatin1String("delete")) {
        if (!m_profiler->remove(id)) {
            *status = 409;
            return {{"ok", false}, {"error", "Can't delete it (still recording?)"}};
        }
    }
    QJsonObject out = m_profiler->state();
    out.insert(QStringLiteral("ok"), true);
    return out;
}

// ---- Mixxx -> linked devices -------------------------------------------------------------------------------
namespace {
const QByteArray kOurPing = QByteArray::fromHex("f07d5e7e7e7ef7");   // (the devices' own pings carry their sequence)
const QByteArray kMappingStarted = QByteArray::fromHex("f07d5ff7");
} // namespace

void Core::pingMixxx() {
    m_midi->toMixxx(kOurPing);
}

void Core::fromMixxx(const QByteArray& msg) {
    if (msg == kOurPing) {   // ours, back: not for the devices
        emit mixxxAnswered();
        return;
    }
    if (msg == kMappingStarted) {
        emit mixxxMappingStarted();
    }
    m_midi->toCables(msg);
    const QByteArray line = "M " + msg.toHex() + '\n';
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (it.value()->linked) {
            it.key()->write(line);
            activity(it.value()->device, false);
        }
    }
}

// Quitting: the devices' sockets close after this object's own members are gone, and a closing socket says so
// (disconnected): they mustn't call back in here then (that was the crash on quit)
Core::~Core() {
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        it.key()->disconnect(this);
        it.value()->dropUpload();
        delete it.value();
    }
    m_clients.clear();
}

// ---- the link ------------------------------------------------------------------------------------------------
void Core::connection() {
    while (QTcpSocket* s = m_server.nextPendingConnection()) {
        s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_clients.insert(s, new Client);
        connect(s, &QTcpSocket::readyRead, this, [this, s] { read(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            Client* c = m_clients.take(s);
            if (c) {
                c->dropUpload();
            }
            if (c && c->linked) {
                m_seen.remove(c->device);
                emit changed();
            }
            delete c;
            s->deleteLater();
        });
        QPointer<QTcpSocket> guard(s);
        QTimer::singleShot(10000, this, [this, guard] {   // nothing said in time: not a ZyDeck
            if (guard && m_clients.contains(guard) && !m_clients.value(guard)->linked && !m_clients.value(guard)->upload) {
                guard->abort();
            }
        });
    }
}

void Core::read(QTcpSocket* s) {
    Client* c = m_clients.value(s);
    if (!c) {
        return;
    }
    c->buffer += s->readAll();
    if (c->upload) {
        uploadMore(s, *c);
        return;
    }
    if (!c->linked && c->buffer.startsWith("POST ")) {
        const int end = c->buffer.indexOf("\r\n\r\n");
        if (end >= 0) {
            const QByteArray head = c->buffer.left(end);
            c->buffer.remove(0, end + 4);
            uploadStart(s, *c, head);
        } else if (c->buffer.size() > 16384) {
            s->abort();
        }
        return;
    }
    if (!c->linked && c->buffer.startsWith("GET ")) {
        const int end = c->buffer.indexOf("\r\n\r\n");
        if (end >= 0) {
            http(s, c->buffer.left(end));
            c->buffer.clear();
        } else if (c->buffer.size() > 16384) {
            s->abort();
        }
        return;
    }
    if (c->buffer.size() > 4 * 1024 * 1024 && c->buffer.indexOf('\n') < 0) {
        s->abort();   // not ZyDeck talking
        return;
    }
    for (int nl; (nl = c->buffer.indexOf('\n')) >= 0;) {
        const QByteArray l = c->buffer.left(nl).trimmed();
        c->buffer.remove(0, nl + 1);
        line(s, *c, l);
        if (!m_clients.contains(s)) {
            return;
        }
    }
}

void Core::line(QTcpSocket* s, Client& c, const QByteArray& l) {
    if (!c.linked) {
        const QList<QByteArray> parts = l.split(' ');
        if (parts.size() < 2 || parts[0] != "ZYDECK" || !sameKey(parts[1], m_key.toLatin1())) {
            s->write("NO\n");
            s->disconnectFromHost();
            return;
        }
        c.device = parts.size() > 2 ? QString::fromUtf8(QByteArray::fromHex(parts[2])) : QString();
        if (c.device.isEmpty()) {
            c.device = QStringLiteral("ZyDeck");
        }
        c.linked = true;
        s->write("OK " + m_name.toUtf8().toHex() + '\n');
        s->write("H " + QJsonDocument(m_hardware->state()).toJson(QJsonDocument::Compact) + '\n');
        bool v4 = false;   // (as the network search has it: a plain IPv4 address, not IPv6's ::ffff: form)
        const quint32 ip = s->peerAddress().toIPv4Address(&v4);
        seen(c.device, tr("Wi-Fi"), v4 ? QHostAddress(ip).toString() : s->peerAddress().toString());
        return;
    }
    if (l.startsWith("M ")) {
        const QByteArray data = QByteArray::fromHex(l.mid(2));
        if (!data.isEmpty()) {
            m_midi->toMixxx(data);
            activity(c.device, true);
        }
    } else if (l.startsWith("P ")) {   // a line of the device's session (profiler.h)
        const QJsonObject p = QJsonDocument::fromJson(l.mid(2)).object();
        if (!p.isEmpty()) {
            m_profiler->deviceLine(c.device, p);
        }
    } else if (l.startsWith("J ")) {
        const QJsonObject ask = QJsonDocument::fromJson(l.mid(2)).object();
        QPointer<QTcpSocket> guard(s);
        QThreadPool::globalInstance()->start([this, guard, ask] {
            const QByteArray reply = "J " + QJsonDocument(nameReply(ask)).toJson(QJsonDocument::Compact) + '\n';
            QMetaObject::invokeMethod(this, [guard, reply] {
                if (guard) {
                    guard->write(reply);
                }
            });
        });
    }
}

namespace {
// A /web/ answer: JSON, for the pages too when they have the key
QByteArray jsonReply(int status, const QByteArray& body, bool keyed) {
    return "HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " No") + "\r\nContent-Type: application/json\r\n"
           "Content-Length: " + QByteArray::number(body.size()) + "\r\n" +
           QByteArray(keyed ? "Access-Control-Allow-Origin: *\r\n" : "") +   // (the pages, with the key)
           "Cache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}
} // namespace

// POST /web/upload?k=<key>&name=...: a download a device did itself (the phone, with its own Bilibili login, which
// never comes here), its body straight into a file in the downloads folder's .incoming as it arrives
void Core::uploadStart(QTcpSocket* s, Client& c, const QByteArray& head) {
    const auto refuse = [s](int status, const QString& error) {
        s->write(jsonReply(status, QJsonDocument(QJsonObject{{"ok", false}, {"error", error}}).toJson(QJsonDocument::Compact), false));
        s->disconnectFromHost();
    };
    const QByteArray first = head.left(head.indexOf("\r\n"));
    if (!first.startsWith("POST /web/upload?")) {
        refuse(404, QStringLiteral("No such thing"));
        return;
    }
    const QUrl url(QString::fromUtf8(first.mid(5, first.lastIndexOf(' ') - 5)));
    QString qs = url.query(QUrl::FullyEncoded);
    qs.replace(QLatin1Char('+'), QStringLiteral("%20"));
    const QUrlQuery query(qs);
    if (!sameKey(query.queryItemValue(QStringLiteral("k")).toLatin1(), m_key.toLatin1())) {
        refuse(403, QStringLiteral("wrong key"));
        return;
    }
    qint64 size = -1;
    for (const QByteArray& h : head.split('\n')) {
        if (h.toLower().startsWith("content-length:")) {
            size = h.mid(15).trimmed().toLongLong();
        }
    }
    if (size <= 0 || size > (qint64{4} << 30)) {   // (a track: not nothing, not more than 4 GB)
        refuse(400, QStringLiteral("No track, or too big"));
        return;
    }
    QString error;
    const QString part = m_web.uploadFile(query, &error);
    std::unique_ptr<QFile> file;
    if (!part.isEmpty()) {
        file = std::make_unique<QFile>(part);
    }
    if (!file || !file->open(QIODevice::WriteOnly)) {
        refuse(409, error.isEmpty() ? QStringLiteral("Can't write the file here") : error);
        return;
    }
    c.upload = std::move(file);
    c.uploadLeft = size;
    c.uploadQuery = query;
    // a sender gone quiet (out of Wi-Fi, say): given up on after half a minute without a byte
    QPointer<QTcpSocket> guard(s);
    auto* stall = new QTimer(s);
    stall->setInterval(30000);
    auto seen = std::make_shared<qint64>(size);
    connect(stall, &QTimer::timeout, this, [this, guard, stall, seen] {
        Client* now = guard ? m_clients.value(guard) : nullptr;
        if (!now || !now->upload) {
            stall->deleteLater();
        } else if (now->uploadLeft == *seen) {
            now->dropUpload();
            guard->abort();
        } else {
            *seen = now->uploadLeft;
        }
    });
    stall->start();
    uploadMore(s, c);
}

void Core::uploadMore(QTcpSocket* s, Client& c) {
    const QByteArray chunk = c.buffer.left(c.uploadLeft);
    c.buffer.clear();
    if (c.upload->write(chunk) != chunk.size()) {
        c.dropUpload();
        s->write(jsonReply(507, R"j({"ok":false,"error":"Couldn't write it here (is the disk full?)"})j", false));
        s->disconnectFromHost();
        return;
    }
    c.uploadLeft -= chunk.size();
    if (c.uploadLeft > 0) {
        return;
    }
    c.upload->close();
    const QString part = c.upload->fileName();
    c.upload.reset();
    s->write(jsonReply(200, QJsonDocument(m_web.uploaded(part, c.uploadQuery)).toJson(QJsonDocument::Compact), false));
    s->disconnectFromHost();
}

/// GET /waveform/<id>?k=<key> and /beats/<id>?k=<key>. The waveform goes deflated (as Mixxx's own files are:
/// qCompress is zlib's format, which is HTTP's "deflate").
void Core::http(QTcpSocket* s, const QByteArray& request) {
    // /web/...: downloads and stems (web.h), for a device with the key or for this computer itself (Mixxx ZyDeck,
    // which can't know the key)
    const QByteArray first = request.left(request.indexOf("\r\n"));
    const bool loopback = s->peerAddress().isLoopback();
    // this computer's pages, here only: the session profiler's, the downloader (the phone's Web tab, here)
    const QByteArray pageName = first.startsWith("GET /profile ") || first.startsWith("GET /profile?") ? "profile"
            : first.startsWith("GET /downloader ") || first.startsWith("GET /downloader?")       ? "downloader"
                                                                                                  : "";
    if (!pageName.isEmpty()) {
        QFile page(QStringLiteral(":/page/%1.html").arg(QString::fromLatin1(pageName)));
        const bool ok = loopback && page.open(QIODevice::ReadOnly);
        const QByteArray body = ok ? page.readAll() : QByteArray("not here\n");
        s->write("HTTP/1.1 " + QByteArray(ok ? "200 OK" : "404 No") + "\r\nContent-Type: " + (ok ? "text/html; charset=utf-8" : "text/plain") +
                 "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body);
        s->disconnectFromHost();
        return;
    }
    if (first.startsWith("GET /web/")) {
        const QUrl url(QString::fromUtf8(first.mid(4, first.lastIndexOf(' ') - 4)));
        QString qs = url.query(QUrl::FullyEncoded);
        qs.replace(QLatin1Char('+'), QStringLiteral("%20"));   // (a form's spaces)
        const QUrlQuery query(qs);
        int status = 403;
        QJsonObject out{{"ok", false}, {"error", "wrong key"}};
        const bool keyed = sameKey(query.queryItemValue(QStringLiteral("k")).toLatin1(), m_key.toLatin1());
        // without the key: Mixxx ZyDeck on this computer, which says so (X-ZyDeck: mixxx). Not just any request from
        // here: a web page open in this computer's browser can make one (an image's address will do), but it can't
        // add a header of its own without asking first (CORS), and isn't let
        const bool mixxx = loopback && request.toLower().contains("\r\nx-zydeck:");   // (Mixxx ZyDeck, the profiler's page here)
        if ((keyed || mixxx) && url.path() == QLatin1String("/web/profile")) {
            out = profileApi(query, &status);
            if (out.contains(QStringLiteral("_lines"))) {   // a session, as it is
                const QByteArray lines = out.value(QStringLiteral("_lines")).toString().toUtf8();
                s->write("HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nContent-Length: " + QByteArray::number(lines.size()) +
                         "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + lines);
                s->disconnectFromHost();
                return;
            }
        } else if ((keyed || mixxx) && url.path() == QLatin1String("/web/structure")) {   // a track's sections (waveforms.h)
            QPointer<QTcpSocket> guard(s);
            const int id = query.queryItemValue(QStringLiteral("id")).toInt();
            QThreadPool::globalInstance()->start([this, guard, id, keyed] {
                const QJsonObject answer = m_waveforms.structure(id);
                const QByteArray body = QJsonDocument(answer).toJson(QJsonDocument::Compact);
                const int st = answer.value(QStringLiteral("ok")).toBool(true) ? 200 : 409;
                QMetaObject::invokeMethod(this, [guard, st, body, keyed] {
                    if (guard) {
                        guard->write(jsonReply(st, body, keyed));
                        guard->disconnectFromHost();
                    }
                });
            });
            return;
        } else if ((keyed || mixxx) && (url.path().startsWith(QLatin1String("/web/library/")) || url.path() == QLatin1String("/web/autodj") ||
                                  url.path() == QLatin1String("/web/autodj/autoplay"))) {
            // the library page's lists: read on a worker, as the waveforms are (a big library, a NAS's files looked
            // for: the MIDI and the devices' link, on this thread, go on meanwhile)
            QPointer<QTcpSocket> guard(s);
            const QString what = url.path().mid(5);
            QThreadPool::globalInstance()->start([this, guard, what, query, keyed] {
                int st = 200;
                const QByteArray body = QJsonDocument(Web::library(&m_library, what, query, &st)).toJson(QJsonDocument::Compact);
                QMetaObject::invokeMethod(this, [guard, st, body, keyed] {
                    if (guard) {
                        guard->write(jsonReply(st, body, keyed));
                        guard->disconnectFromHost();
                    }
                });
            });
            return;
        } else if (keyed || mixxx) {
            out = m_web.handle(url.path().mid(5), query, &status, keyed);
        }
        s->write(jsonReply(status, QJsonDocument(out).toJson(QJsonDocument::Compact), keyed));
        s->disconnectFromHost();
        return;
    }
    static const QRegularExpression route(QStringLiteral(R"(^GET /(waveform|beats)/(\d+)(?:\?k=([0-9a-f]+))?)"));
    const QRegularExpressionMatch m = route.match(QString::fromLatin1(request));
    QPointer<QTcpSocket> guard(s);
    const auto respond = [this, guard](int status, const QByteArray& body, const QByteArray& headers) {
        QMetaObject::invokeMethod(this, [guard, status, body, headers] {
            if (!guard) {
                return;
            }
            QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " No") + "\r\n";
            head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
            head += "Access-Control-Allow-Origin: *\r\nCache-Control: max-age=300\r\nConnection: close\r\n" + headers + "\r\n";
            guard->write(head + body);
            guard->disconnectFromHost();
        });
    };
    if (!m.hasMatch()) {
        respond(404, "not found\n", "Content-Type: text/plain\r\n");
        return;
    }
    if (!sameKey(m.captured(3).toLatin1(), m_key.toLatin1())) {
        respond(403, "wrong key\n", "Content-Type: text/plain\r\n");
        return;
    }
    const bool waveform = m.captured(1) == QLatin1String("waveform");
    const int id = m.captured(2).toInt();
    QThreadPool::globalInstance()->start([this, waveform, id, respond] {
        if (waveform) {
            // made here (from the cache, or now); Mixxx's own analysis file if the track couldn't be read
            m_waveforms.get(id, [this, id, respond](const QByteArray& made) {
                const QByteArray data = made.isEmpty() ? m_library.waveform(id) : made;
                if (data.isEmpty()) {
                    respond(404, "not found\n", "Content-Type: text/plain\r\n");
                } else {
                    respond(200, qCompress(data, 5).mid(4), "Content-Type: application/octet-stream\r\nContent-Encoding: deflate\r\n");
                }
            });
        } else {
            const QJsonObject b = m_library.beats(id);
            if (b.isEmpty()) {
                respond(404, "not found\n", "Content-Type: text/plain\r\n");
            } else {
                respond(200, QJsonDocument(b).toJson(QJsonDocument::Compact), "Content-Type: application/json\r\n");
            }
        }
    });
}

} // namespace zc
