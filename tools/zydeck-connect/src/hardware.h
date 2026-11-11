#pragma once
// The DJ controller plugged into this computer, followed READ ONLY: Midi already listens to every MIDI input (it looks
// for phones on cables), and only ever writes to a device that said hello (a ZyDeck), so nothing here reaches the
// controller. What it finds (its pad modes, its decks 3-4 switch) goes to the devices for their pages to follow
// (zydeckshared.h kHardware); a page switching its own decks or pad modes never comes back this way.
//
// Known: Hercules DJControl Inpulse 300 MK2 with its 4-deck mapping (Assistant switches the decks to 3-4; Mixxx's
// mapping keeps that, so it's counted here: back to 1-2 when the mappings start again).

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

namespace zc {

class Midi;

class Hardware : public QObject {
    Q_OBJECT
  public:
    explicit Hardware(Midi* midi, QObject* parent = nullptr);

    /// Worth a look (on the MIDI thread, so cheap): a pressed key from a known controller
    static bool wants(const QString& port, const QByteArray& msg);
    void feed(const QString& port, const QByteArray& msg);
    void mappingsStarted();      // Mixxx's mappings (re)started: the Inpulse's is on decks 1-2 again
    QJsonObject state() const;   // as kHardware has it; {} with none plugged in

    /// What a press means (pure: tools/zydeck-shared-tests style checks): "" for nothing of ours
    struct Event {
        QString what;   // "d34", "pads0", "pads1" or ""
        QString mode;   // for pads: the mode
        bool set = false;   // a mode key (says the mode outright) rather than a pad (the mode it was pressed in)
    };
    static Event read(const QByteArray& msg);

    /// What its mode keys send, as the pad-mode wizard (padwizard.h) heard them on deck A (deck B: one channel up)
    struct Key {
        quint8 st = 0, note = 0;
        bool shift = false;   // with SHIFT held
        int block = 0;        // its pads' block: kBlocks
    };
    static QString learnedFile();
    static QString blockName(int block);

  signals:
    void changed(const QJsonObject& state);   // with "ev"

  private:
    QString port() const;   // the known controller's MIDI input, "" without one
    void present();         // plugged in or out: say so
    Midi* m_midi;
    QTimer m_look;
    QString m_port;
    bool m_d34 = false;
    QString m_pads[2];
    bool m_shift[2] = {false, false};
    void loadLearned();   // (again whenever the file changes)
    QList<Key> m_learned;
    QDateTime m_learnedAt;
};

} // namespace zc
