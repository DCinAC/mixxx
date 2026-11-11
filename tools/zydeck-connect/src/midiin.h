#pragma once
// One MIDI input, read only: the setup wizards' (jogwizard.h, padwizard.h), which listen to a DJ controller beside
// Mixxx (macOS lets both read it) and never write to it. Its messages come on the context object's thread.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <vector>

#include "RtMidi.h"
#include "midi.h"   // (kPortName)

namespace zc {

class MidiReader {
  public:
    std::function<void(quint8, quint8, quint8)> onMessage;
    QPointer<QObject> context;   // (where onMessage runs: RtMidi calls back on its own thread)

    ~MidiReader() { close(); }

    static QStringList ports() {
        QStringList names;
        try {
            RtMidiIn probe(RtMidi::UNSPECIFIED, "ZyDeck setup probe");
            for (unsigned i = 0; i < probe.getPortCount(); ++i) {
                const QString n = QString::fromStdString(probe.getPortName(i));
                if (!n.contains(QLatin1String(kPortName)) && !n.contains(QLatin1String("ZyDeck")) && !n.contains(QLatin1String("Through"))) {
                    names.append(n);
                }
            }
        } catch (const RtMidiError&) {
        }
        return names;
    }

    bool open(const QString& name) {
        close();
        try {
            m_in = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, "ZyDeck setup", 1000);
            for (unsigned i = 0; i < m_in->getPortCount(); ++i) {
                if (QString::fromStdString(m_in->getPortName(i)) == name) {
                    m_in->setCallback(&MidiReader::callback, this);
                    m_in->ignoreTypes(true, true, true);
                    m_in->openPort(i, "ZyDeck setup");
                    return true;
                }
            }
        } catch (const RtMidiError&) {
        }
        m_in.reset();
        return false;
    }

    void close() {
        if (m_in) {
            try {
                m_in->closePort();
            } catch (const RtMidiError&) {
            }
            m_in.reset();
        }
    }

    void inject(quint8 st, quint8 d1, quint8 d2) {   // (the demo)
        if (onMessage) {
            onMessage(st, d1, d2);
        }
    }

  private:
    static void callback(double, std::vector<unsigned char>* m, void* user) {
        auto* self = static_cast<MidiReader*>(user);
        if (!m || m->size() != 3 || !self->context) {
            return;
        }
        const quint8 st = (*m)[0], d1 = (*m)[1], d2 = (*m)[2];
        QMetaObject::invokeMethod(self->context, [self, st, d1, d2] { self->inject(st, d1, d2); }, Qt::QueuedConnection);
    }
    std::unique_ptr<RtMidiIn> m_in;
};

} // namespace zc
