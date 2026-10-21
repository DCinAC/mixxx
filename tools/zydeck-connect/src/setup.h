#pragma once
// Mixxx's side, set up once per computer and checked all the time: the ZyDeck mapping in Mixxx's controllers folder
// (kept the same as the one built in), the controller turned on for the ZyDeck Connect port in Mixxx's settings
// (mixxx.cfg: only while Mixxx is closed, which reads it when it starts and writes it when it quits; backed up
// first), and Mixxx actually answering through the port (the mapping echoes a ping, and says when it starts).
// It asks only when something needs the user: once, whether to set Mixxx up at all; to quit Mixxx when it's open
// and its settings need the change; to restart Mixxx when it was started before the port existed.

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace zc {

class Core;

class Setup : public QObject {
    Q_OBJECT
  public:
    enum class State {
        Checking,
        NoMixxx,       // Mixxx hasn't run on this computer yet (no library, no settings)
        NoPort,        // no MIDI port (Windows without loopMIDI's driver)
        NeedConsent,   // not set up, and not yet allowed to
        QuitMixxx,     // Mixxx is open and its settings need the controller turned on: quit it once
        StartMixxx,    // all set; Mixxx isn't running
        RestartMixxx,  // Mixxx is running but doesn't answer: started before ZyDeck Connect made the port
        Linked,        // Mixxx answers through the port
        Failed,        // couldn't write the mapping or the settings (why())
    };
    explicit Setup(Core* core, QObject* parent = nullptr);

    State state() const { return m_state; }
    QString why() const { return m_why; }
    bool consented() const;
    void consent(bool yes);   // the user's answer to "set Mixxx up?"
    void redo();              // MAP: the mapping again, and look again now

    void pong();              // the mapping echoed our ping
    void mappingStarted();    // the mapping says it (re)started

  signals:
    void changed(zc::Setup::State state);

  private:
    void check();
    void checked(bool running);
    void set(State s, const QString& why = QString());

    Core* m_core;
    State m_state = State::Checking;
    QString m_why;
    QTimer m_timer;
    QProcess m_ps;              // is Mixxx running?
    QElapsedTimer m_lastAnswer; // the mapping's last echo (or start)
    int m_unanswered = 0;       // pings since the last echo
};

} // namespace zc
