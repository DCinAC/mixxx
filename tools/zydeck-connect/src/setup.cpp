#include "setup.h"

#include <QSettings>

#include "core.h"
#include "library.h"
#include "midi.h"
#include "system.h"

namespace zc {

Setup::Setup(Core* core, QObject* parent)
        : QObject(parent),
          m_core(core) {
    connect(&m_ps, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        const QString out = QString::fromLocal8Bit(m_ps.readAllStandardOutput());
#ifdef Q_OS_WIN
        checked(out.contains(QLatin1String("mixxx.exe"), Qt::CaseInsensitive));
#else
        checked(!out.trimmed().isEmpty());
#endif
    });
    connect(&m_timer, &QTimer::timeout, this, &Setup::check);
    m_timer.start(3000);
    QTimer::singleShot(0, this, &Setup::check);
}

bool Setup::consented() const {
    return QSettings().value(QStringLiteral("setup/mixxx")).toString() == QLatin1String("yes");
}

void Setup::consent(bool yes) {
    QSettings().setValue(QStringLiteral("setup/mixxx"), yes ? "yes" : "no");
    check();
}

void Setup::redo() {
    QSettings().setValue(QStringLiteral("setup/mixxx"), "yes");
    try {
        installMapping();
    } catch (const QString& why) {
        set(State::Failed, why);
        return;
    }
    m_unanswered = 0;
    check();
}

void Setup::pong() {
    m_lastAnswer.start();
    m_unanswered = 0;
    if (m_state == State::RestartMixxx || m_state == State::StartMixxx || m_state == State::Checking) {
        set(State::Linked);
    }
}

void Setup::mappingStarted() {
    pong();
}

void Setup::set(State s, const QString& why) {
    if (s == m_state && why == m_why) {
        return;
    }
    m_state = s;
    m_why = why;
    emit changed(s);
}

/// Every 3 s: the files, the settings, and whether Mixxx is running (asked of the system, without waiting for it)
void Setup::check() {
    if (!m_core->midi()->problem().isEmpty()) {
        set(State::NoPort, m_core->midi()->problem());
        return;
    }
    bool known = false;
    if (mixxxDir(&known).isEmpty()) {
        if (known) {   // (still looking: macOS may be asking the user whether ZyDeck Connect may)
            set(State::NoMixxx);
        }
        return;
    }
    // (not yet allowed: it still looks whether Mixxx runs, and answers, before it asks; checked() decides)
    if (consented() && !mappingCurrent()) {   // ZyDeck's own files in Mixxx's folder: kept up to date without asking
        try {
            installMapping();
        } catch (const QString& why) {
            set(State::Failed, why);
            return;
        }
    }
    if (m_ps.state() != QProcess::NotRunning) {
        return;
    }
#if defined(Q_OS_WIN)
    m_ps.start(QStringLiteral("tasklist"), {QStringLiteral("/FI"), QStringLiteral("IMAGENAME eq mixxx.exe"), QStringLiteral("/NH")});
#else
    m_ps.start(QStringLiteral("pgrep"), {QStringLiteral("-ix"), QStringLiteral("mixxx")});
#endif
}

void Setup::checked(bool running) {
    m_running = running;
    if (mixxxDir().isEmpty() || !m_core->midi()->problem().isEmpty()) {
        return;
    }
    if (!consented()) {   // ask only once it's known whether Mixxx is open, and not at all if it already works
        if (running && controllerOn() && m_lastAnswer.isValid() && m_lastAnswer.elapsed() < 8000) {
            QSettings().setValue(QStringLiteral("setup/mixxx"), "yes");
            set(State::Linked);
            return;
        }
        if (running && controllerOn()) {
            m_core->pingMixxx();   // (set up by hand before, maybe: its answer comes before the next look)
            if (++m_unanswered < 2) {
                return;
            }
        }
        set(State::NeedConsent);
        return;
    }
    if (!controllerOn()) {
        if (running) {   // its settings are its own while it runs (it writes them when it quits)
            set(State::QuitMixxx);
            return;
        }
        try {
            turnControllerOn();
        } catch (const QString& why) {
            set(State::Failed, why);
            return;
        }
    }
    if (!running) {
        m_unanswered = 0;
        set(State::StartMixxx);
        return;
    }
    // running, and set up: does it answer through the port?
    if (m_lastAnswer.isValid() && m_lastAnswer.elapsed() < 8000) {
        set(State::Linked);
    } else if (++m_unanswered > 3) {   // (Mixxx takes a moment to start; then it should answer)
        set(State::RestartMixxx);
    }
    m_core->pingMixxx();
}

} // namespace zc
