#include "hardware.h"

#include <QJsonArray>

#include "midi.h"

namespace zc {

namespace {

// Hercules DJControl Inpulse 300 MK2: its MIDI input's name has this in it
const QLatin1String kInpulse("Inpulse 300");

// Its pads, on channel 7 (deck A, 0x96) and 8 (deck B, 0x97): a block of 16 notes a mode, 8 pads and 8 more with
// SHIFT held. The mode a pad was pressed in is the mode the controller is in (it keeps the mode itself).
// (Slicer's block is where the 4-deck mapping put its stems)
const char* const kBlocks[8] = {"hotcue", "roll", "slicer", "sampler", "toneplay", "beatjump", "slicerloop", "fx"};

} // namespace

Hardware::Hardware(Midi* midi, QObject* parent)
        : QObject(parent),
          m_midi(midi) {
    m_look.setInterval(2000);   // (as often as Midi looks for MIDI inputs)
    connect(&m_look, &QTimer::timeout, this, &Hardware::present);
    m_look.start();
}

bool Hardware::wants(const QString& port, const QByteArray& msg) {
    return msg.size() == 3 && (static_cast<quint8>(msg[0]) & 0xF0) == 0x90 && msg[2] != 0 &&
            port.contains(kInpulse, Qt::CaseInsensitive);
}

Hardware::Event Hardware::read(const QByteArray& msg) {
    if (msg.size() != 3 || msg[2] == 0) {
        return {};
    }
    const quint8 status = static_cast<quint8>(msg[0]), note = static_cast<quint8>(msg[1]);
    if (status == 0x90 && note == 0x03) {   // Assistant: the mapping's decks 3-4 switch
        return {QStringLiteral("d34"), {}, false};
    }
    if ((status == 0x96 || status == 0x97) && note < 0x80) {
        return {status == 0x96 ? QStringLiteral("pads0") : QStringLiteral("pads1"), QString::fromLatin1(kBlocks[note >> 4]), false};
    }
    return {};
}

void Hardware::feed(const QString& port, const QByteArray& msg) {
    const Event e = read(msg);
    if (e.what.isEmpty()) {
        return;
    }
    m_port = port;
    if (e.what == QLatin1String("d34")) {
        m_d34 = !m_d34;
    } else {
        QString& mode = m_pads[e.what == QLatin1String("pads1") ? 1 : 0];
        if (mode == e.mode && !e.set) {
            return;   // (another pad in the same mode: nothing new)
        }
        mode = e.mode;
    }
    QJsonObject s = state();
    s.insert(QStringLiteral("ev"), e.what);
    emit changed(s);
}

void Hardware::mappingsStarted() {
    m_pads[0].clear();   // (still what the controller has, but say so only once it's seen again)
    m_pads[1].clear();
    if (m_d34) {
        m_d34 = false;
        QJsonObject s = state();
        s.insert(QStringLiteral("ev"), QStringLiteral("d34"));
        emit changed(s);
    }
}

QString Hardware::port() const {
    const QStringList cables = m_midi->cables();
    for (const QString& c : cables) {
        if (c.contains(kInpulse, Qt::CaseInsensitive)) {
            return c;
        }
    }
    return {};
}

QJsonObject Hardware::state() const {
    const QString p = port();
    if (p.isEmpty()) {
        return {};
    }
    const auto mode = [](const QString& m) { return m.isEmpty() ? QJsonValue() : QJsonValue(m); };
    return {{"dev", p}, {"d34", m_d34}, {"pads", QJsonArray{mode(m_pads[0]), mode(m_pads[1])}}};
}

void Hardware::present() {
    const QString p = port();
    if (p == m_port) {
        return;
    }
    if (p.isEmpty()) {   // unplugged: what it had is gone with it (Mixxx can't take it back without a restart)
        m_d34 = false;
        m_pads[0].clear();
        m_pads[1].clear();
    }
    m_port = p;
    emit changed(state());
}

} // namespace zc
