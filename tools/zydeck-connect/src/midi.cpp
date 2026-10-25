#include "midi.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <vector>

#include "RtMidi.h"
#include "library.h"   // (mixxxDir)

#ifdef Q_OS_WIN
#include <QLibrary>
#include <windows.h>
#endif

namespace zc {

QByteArray sysexText(quint8 kind, const QJsonObject& obj) {
    QByteArray m;
    m.append(static_cast<char>(0xF0)).append(static_cast<char>(0x7D)).append(static_cast<char>(kind));
    m.append(QJsonDocument(obj).toJson(QJsonDocument::Compact).toHex());
    m.append(static_cast<char>(0xF7));
    return m;
}

QJsonObject sysexJson(const QByteArray& msg) {
    if (msg.size() < 5) {
        return {};
    }
    return QJsonDocument::fromJson(QByteArray::fromHex(msg.mid(3, msg.size() - 4))).object();
}

namespace {
// A device on the cable reaches Mixxx through ZyDeck Connect: Connect holds the cable's port and relays it to and
// from the ZyDeck Connect port, the one Mixxx has open from its start. So a cable plugged in (or out) while Mixxx runs
// just works (Mixxx only looks for MIDI ports as it starts). Windows lets one app at a time open a MIDI input anyway.
// Elsewhere Mixxx could open the cable itself: a port Mixxx is set to handle with the ZyDeck mapping (mixxxOwns) isn't
// relayed, or every message would arrive twice.
constexpr bool kRelayCable = true;

// Whether Mixxx's settings have the ZyDeck mapping on for this MIDI port itself ([Controller] <name> 1, its
// [ControllerPreset] a ZyDeck one; Mixxx writes the port's name with _ for spaces). Read again every 10 s
bool mixxxOwns(const QString& port) {
#ifdef Q_OS_WIN
    Q_UNUSED(port);
    return false;
#else
    static QMutex mutex;
    static QSet<QString> owned;
    static qint64 readAt = 0;
    QMutexLocker lock(&mutex);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - readAt > 10000) {
        readAt = now;
        owned.clear();
        QFile f(mixxxDir() + QStringLiteral("/mixxx.cfg"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString section;
            QHash<QString, bool> on;
            QHash<QString, QString> preset;
            while (!f.atEnd()) {
                const QString line = QString::fromUtf8(f.readLine()).trimmed();
                if (line.startsWith(QLatin1Char('['))) {
                    section = line;
                    continue;
                }
                const qsizetype space = line.indexOf(QLatin1Char(' '));
                if (space <= 0) {
                    continue;
                }
                if (section == QLatin1String("[Controller]")) {
                    on.insert(line.left(space), line.mid(space + 1).trimmed() == QLatin1String("1"));
                } else if (section == QLatin1String("[ControllerPreset]")) {
                    preset.insert(line.left(space), line.mid(space + 1));
                }
            }
            for (auto it = on.cbegin(); it != on.cend(); ++it) {
                if (it.value() && preset.value(it.key()).contains(QLatin1String("ZyDeck"), Qt::CaseInsensitive)) {
                    owned.insert(it.key());
                }
            }
        }
    }
    QString key = port;
    key.replace(QLatin1Char(' '), QLatin1Char('_'));
    for (const QString& o : std::as_const(owned)) {
        if (o != QLatin1String("ZyDeck_Connect") && (key == o || key.startsWith(o + QLatin1Char('_')))) {
            return true;
        }
    }
    return false;
#endif
}

QByteArray bytes(const std::vector<unsigned char>* m) {
    return QByteArray(reinterpret_cast<const char*>(m->data()), static_cast<int>(m->size()));
}

void portCallback(double, std::vector<unsigned char>* m, void* self) {
    if (m && !m->empty()) {
        static_cast<Midi*>(self)->fromMixxxRaw(bytes(m));
    }
}

const QRegularExpression kAlsaNumbers(QStringLiteral(R"(\s+\d+:\d+$)"));   // ALSA adds client:port numbers

#ifdef Q_OS_WIN
// loopMIDI's driver (teVirtualMIDI, free from tobias-erichsen.de): Windows has no virtual MIDI ports of its own.
// What's sent comes out of the port for Mixxx; what Mixxx sends to it arrives in the callback, not looped back.
using PortCb = void(CALLBACK*)(void*, LPBYTE, DWORD, DWORD_PTR);
using CreatePort = void*(WINAPI*)(LPCWSTR, PortCb, DWORD_PTR, DWORD, DWORD);
using SendData = BOOL(WINAPI*)(void*, LPBYTE, DWORD);
using ClosePort = void(WINAPI*)(void*);
SendData s_send = nullptr;
ClosePort s_close = nullptr;
void CALLBACK winCallback(void*, LPBYTE data, DWORD length, DWORD_PTR self) {
    if (length) {
        reinterpret_cast<Midi*>(self)->fromMixxxRaw(QByteArray(reinterpret_cast<const char*>(data), static_cast<int>(length)));
    }
}
#endif
} // namespace

struct Midi::Cable {
    Midi* midi;
    QString port;
    std::unique_ptr<RtMidiIn> in;
    static void callback(double, std::vector<unsigned char>* m, void* self) {
        if (m && !m->empty()) {
            auto* c = static_cast<Cable*>(self);
            c->midi->fromCable(c->port, bytes(m));
        }
    }
};

Midi::Midi(QObject* parent)
        : QObject(parent) {
    m_clock.start();
    m_pace.setInterval(1);
    m_pace.setTimerType(Qt::PreciseTimer);
    connect(&m_pace, &QTimer::timeout, this, &Midi::drain);
#ifdef Q_OS_WIN
    QLibrary lib(sizeof(void*) == 8 ? QStringLiteral("teVirtualMIDI64") : QStringLiteral("teVirtualMIDI"));
    auto create = reinterpret_cast<CreatePort>(lib.resolve("virtualMIDICreatePortEx2"));
    s_send = reinterpret_cast<SendData>(lib.resolve("virtualMIDISendData"));
    s_close = reinterpret_cast<ClosePort>(lib.resolve("virtualMIDIClosePort"));
    constexpr DWORD kParseRx = 1, kInstantiateBoth = 12;
    if (create && s_send) {
        const std::wstring name = QString::fromLatin1(kPortName).toStdWString();
        m_winPort = create(name.c_str(), winCallback, reinterpret_cast<DWORD_PTR>(this), 65535, kParseRx | kInstantiateBoth);
    }
    if (!m_winPort) {   // still useful over the cable: names and waveforms
        m_problem = tr("No ZyDeck Connect MIDI port: install loopMIDI (free) for the Wi-Fi link. Over the USB cable "
                       "everything else works.");
    }
#else
    try {
        m_out = std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, kPortName);
        m_out->openVirtualPort(kPortName);
        m_in = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, kPortName, 1000);
        m_in->ignoreTypes(false, true, true);
        m_in->setCallback(portCallback, this);
        m_in->openVirtualPort(kPortName);
    } catch (const RtMidiError& e) {
        m_problem = tr("Couldn't make the MIDI port: %1").arg(QString::fromStdString(e.getMessage()));
        m_out.reset();
        m_in.reset();
    }
#endif
    connect(&m_watch, &QTimer::timeout, this, &Midi::watch);
    m_watch.start(2000);
    watch();
}

Midi::~Midi() {
    m_watch.stop();
    QList<Cable*> cables;
    {
        QMutexLocker lock(&m_mutex);
        cables = m_cables.values();
        m_cables.clear();
        qDeleteAll(m_replies);
        m_replies.clear();
    }
    qDeleteAll(cables);
    m_in.reset();
#ifdef Q_OS_WIN
    if (m_winPort && s_close) {
        s_close(m_winPort);
    }
#endif
}

QString Midi::problem() const {
    return m_problem;
}

QStringList Midi::cables() const {
    QMutexLocker lock(&m_mutex);
    return m_cables.keys();
}

void Midi::fromMixxxRaw(const QByteArray& msg) {
    emit fromMixxx(msg);   // (queued to the main thread)
}

namespace {
constexpr double kBurst = 512;   // events at once: half of Mixxx's input buffer
constexpr double kRate = 16;     // events a millisecond after that (64 kB/s: thousands of control values a second)
// how much of Mixxx's buffer a message takes: a SysEx 4 bytes an event, anything else one
double events(const QByteArray& msg) {
    return (std::min)(kBurst, msg.size() > 3 ? std::ceil(msg.size() / 4.0) : 1.0);
}
// a control's value (the mapping's "set": F0 7D 50 <param> <name> 00 <value> F7): the control it's for, else ""
QByteArray setKey(const QByteArray& msg) {
    if (msg.size() < 6 || static_cast<quint8>(msg[0]) != 0xF0 || msg[1] != 0x7D || msg[2] != 0x50) {
        return {};
    }
    const qsizetype end = msg.indexOf('\0', 4);
    return end > 4 ? msg.left(end) : QByteArray();
}
} // namespace

void Midi::refill() {
    const qint64 now = m_clock.nsecsElapsed();
    m_tokens = m_tokensAtNs < 0 ? kBurst : (std::min)(kBurst, m_tokens + (now - m_tokensAtNs) / 1e6 * kRate);
    m_tokensAtNs = now;
}

void Midi::toMixxx(const QByteArray& msg) {
    QMutexLocker lock(&m_mutex);
    refill();
    if (m_queue.isEmpty() && m_tokens >= events(msg)) {
        m_tokens -= events(msg);
        sendNow(msg);
        return;
    }
    if (const QByteArray key = setKey(msg); !key.isEmpty()) {
        for (QByteArray& waiting : m_queue) {
            if (setKey(waiting) == key) {
                waiting = msg;   // (the control's newest value, where its first one was waiting)
                return;
            }
        }
    }
    m_queue.append(msg);
    if (m_queue.size() == 1) {
        QMetaObject::invokeMethod(&m_pace, qOverload<>(&QTimer::start), Qt::QueuedConnection);
    }
}

void Midi::drain() {
    QMutexLocker lock(&m_mutex);
    refill();
    while (!m_queue.isEmpty() && m_tokens >= events(m_queue.first())) {
        m_tokens -= events(m_queue.first());
        sendNow(m_queue.takeFirst());
    }
    if (m_queue.isEmpty()) {
        m_pace.stop();
    }
}

void Midi::sendNow(const QByteArray& msg) {
#ifdef Q_OS_WIN
    if (m_winPort && s_send) {
        s_send(m_winPort, reinterpret_cast<LPBYTE>(const_cast<char*>(msg.constData())), static_cast<DWORD>(msg.size()));
    }
#else
    if (m_out) {
        try {
            m_out->sendMessage(reinterpret_cast<const unsigned char*>(msg.constData()), static_cast<size_t>(msg.size()));
        } catch (const RtMidiError&) {
        }
    }
#endif
}

void Midi::toCables(const QByteArray& msg) {
    if (!kRelayCable) {
        return;
    }
    QStringList ports;
    {
        QMutexLocker lock(&m_mutex);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it) {
            if (now - it.value() < 3600 * 1000 && !mixxxOwns(it.key())) {
                ports.append(it.key());
            }
        }
    }
    for (const QString& p : std::as_const(ports)) {
        reply(p, msg);
    }
}

void Midi::reply(const QString& port, const QByteArray& msg) {
    QMutexLocker lock(&m_mutex);
    RtMidiOut* out = m_replies.value(port);
    try {
        if (!out) {
            auto made = std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, kPortName);
            const QString base = QString(port).remove(kAlsaNumbers);
            int index = -1;
            for (unsigned i = 0; i < made->getPortCount(); ++i) {
                const QString n = QString::fromStdString(made->getPortName(i));
                if (n == port) {
                    index = static_cast<int>(i);
                    break;
                }
                if (index < 0 && QString(n).remove(kAlsaNumbers) == base) {
                    index = static_cast<int>(i);
                }
            }
            if (index < 0) {
                return;
            }
            made->openPort(static_cast<unsigned>(index), kPortName);
            out = made.release();
            m_replies.insert(port, out);
        }
        out->sendMessage(reinterpret_cast<const unsigned char*>(msg.constData()), static_cast<size_t>(msg.size()));
    } catch (const RtMidiError&) {
    }
}

/// Every 2 s: open the MIDI inputs that came (a phone or tablet plugged in), close the ones that went.
void Midi::watch() {
    QStringList names;
    try {
        RtMidiIn probe(RtMidi::UNSPECIFIED, std::string(kPortName) + " probe");
        for (unsigned i = 0; i < probe.getPortCount(); ++i) {
            names.append(QString::fromStdString(probe.getPortName(i)));
        }
    } catch (const RtMidiError&) {
        return;
    }
    for (int i = 0; i < names.size(); ++i) {
        const QString& name = names[i];
        {
            QMutexLocker lock(&m_mutex);
            if (m_cables.contains(name)) {
                continue;
            }
        }
        if (name.contains(QLatin1String(kPortName)) || name.contains(QLatin1String("ZyDeck Link")) || name.contains(QLatin1String("Through"))) {
            continue;   // ours (and the old Python one's), and the system's loop-back
        }
        auto* c = new Cable{this, name, nullptr};
        try {
            c->in = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, kPortName, 1000);
            c->in->ignoreTypes(false, true, true);
            c->in->setCallback(Cable::callback, c);
            c->in->openPort(static_cast<unsigned>(i), kPortName);
        } catch (const RtMidiError&) {
            delete c;   // (busy: Windows lets one app open it)
            continue;
        }
        QMutexLocker lock(&m_mutex);
        m_cables.insert(name, c);
    }
    QList<Cable*> gone;
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_cables.begin(); it != m_cables.end();) {
            if (!names.contains(it.key())) {   // unplugged
                gone.append(it.value());
                delete m_replies.take(it.key());
                m_devices.remove(it.key());
                it = m_cables.erase(it);
            } else {
                ++it;
            }
        }
    }
    qDeleteAll(gone);   // (closing an input waits for its callback, which may be waiting for the lock)
}

void Midi::fromCable(const QString& port, const QByteArray& msg) {
    const auto byte = [&msg](int i) { return static_cast<quint8>(msg[i]); };
    if (msg.size() < 4 || byte(0) != 0xF0 || byte(1) != 0x7D || (byte(2) != kLinkHello && byte(2) != kNameAsk)) {
        bool relay = false;
        {
            QMutexLocker lock(&m_mutex);
            relay = kRelayCable && m_devices.contains(port);
        }
        if (relay && mixxxOwns(port)) {
            relay = false;   // (Mixxx reads it itself)
        }
        if (relay) {
            toMixxx(msg);
        }
        return;   // (the mapping's own messages: relayed above, unless Mixxx reads this port itself)
    }
    const QJsonObject j = sysexJson(msg);
    if (byte(2) == kLinkHello) {
        {
            QMutexLocker lock(&m_mutex);
            m_devices.insert(port, QDateTime::currentMSecsSinceEpoch());
        }
        const QString device = j.value(QStringLiteral("name")).toString();
        emit hello(device.isEmpty() ? port : device, port);
    } else {
        emit nameAsk(port, j);
    }
}

} // namespace zc
