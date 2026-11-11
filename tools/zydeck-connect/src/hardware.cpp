#include "hardware.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

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
    loadLearned();
}

QString Hardware::learnedFile() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/inpulse-pad-modes.json");
}

QString Hardware::blockName(int block) {
    return block >= 0 && block < 8 ? QString::fromLatin1(kBlocks[block]) : QString();
}

void Hardware::loadLearned() {
    const QFileInfo fi(learnedFile());
    if (fi.lastModified() == m_learnedAt) {
        return;
    }
    m_learnedAt = fi.lastModified();
    m_learned.clear();
    QFile f(fi.filePath());
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonArray keys = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("keys")).toArray();
    for (const QJsonValue& v : keys) {
        const QJsonObject k = v.toObject();
        m_learned.append({static_cast<quint8>(k.value(QStringLiteral("st")).toInt()), static_cast<quint8>(k.value(QStringLiteral("note")).toInt()),
                k.value(QStringLiteral("shift")).toBool(), k.value(QStringLiteral("block")).toInt()});
    }
}

bool Hardware::wants(const QString& port, const QByteArray& msg) {
    if (msg.size() != 3 || (static_cast<quint8>(msg[0]) & 0xF0) != 0x90 || !port.contains(kInpulse, Qt::CaseInsensitive)) {
        return false;
    }
    return msg[2] != 0 || msg[1] == 0x04;   // (presses; SHIFT's letting go too)
}

Hardware::Event Hardware::read(const QByteArray& msg) {
    if (msg.size() != 3 || msg[2] == 0) {
        return {};
    }
    const quint8 status = static_cast<quint8>(msg[0]), note = static_cast<quint8>(msg[1]);
    if (status == 0x90 && note == 0x03) {   // Assistant: the mapping's decks 3-4 switch
        return {QStringLiteral("d34"), {}, false};
    }
    // Its mode keys: mode n is note 0x0F + n - 1 on deck A's channel 2 / deck B's 3, and its pads are block n - 1
    // (Hercules' MIDI chart for the Inpulse 300; heard so on the MK2 too, 2026-11-12: SHIFT + a key sends mode 5-8's)
    if ((status == 0x91 || status == 0x92) && note >= 0x0F && note <= 0x16) {
        return {status == 0x91 ? QStringLiteral("pads0") : QStringLiteral("pads1"), QString::fromLatin1(kBlocks[note - 0x0F]), true};
    }
    if ((status == 0x96 || status == 0x97) && note < 0x80) {
        return {status == 0x96 ? QStringLiteral("pads0") : QStringLiteral("pads1"), QString::fromLatin1(kBlocks[note >> 4]), false};
    }
    return {};
}

void Hardware::feed(const QString& port, const QByteArray& msg) {
    const quint8 status = static_cast<quint8>(msg[0]);
    if ((status == 0x91 || status == 0x92) && msg[1] == 0x04) {   // SHIFT, deck A / B
        m_shift[status - 0x91] = msg[2] != 0;
        return;
    }
    Event e = read(msg);
    if (msg[2] == 0) {
        return;
    }
    // A mode key as the pad-mode wizard heard it (deck A's; deck B's one channel up), SHIFT or not: for a controller
    // that doesn't keep to the chart (the MK2 does)
    bool learned = false;
    for (const Key& k : std::as_const(m_learned)) {
        for (int side = 0; side < 2 && !learned; ++side) {
            if (status == k.st + side && static_cast<quint8>(msg[1]) == k.note && m_shift[side] == k.shift && !blockName(k.block).isEmpty()) {
                e = {side ? QStringLiteral("pads1") : QStringLiteral("pads0"), blockName(k.block), true};
                learned = true;
            }
        }
    }
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
    loadLearned();
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
