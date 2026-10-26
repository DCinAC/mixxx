#include "profiler.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <algorithm>
#include <cmath>

#include "library.h"
#include "midi.h"

namespace zc {

namespace {
constexpr int kKept = 30;
constexpr qint64 kMaxMs = 3 * 3600 * 1000, kMaxBytes = 100 * 1024 * 1024;
const QRegularExpression kId(QStringLiteral(R"(^[0-9_-]{19}$)"));   // (a session's id: when it started)

// [min, median, 95th percentile, 99th, max], as the devices' summaries have them (plus the 99th)
QJsonValue stats(QList<double> a) {
    if (a.isEmpty()) {
        return QJsonValue();
    }
    std::sort(a.begin(), a.end());
    const auto q = [&a](double p) { return std::round(a[(std::min)(a.size() - 1, static_cast<qsizetype>(p * a.size()))] * 10) / 10; };
    return QJsonArray{q(0), q(.5), q(.95), q(.99), q(1)};
}
} // namespace

Profiler::Profiler(Midi* midi, std::function<QString()> deviceAddress, QObject* parent)
        : QObject(parent),
          m_midi(midi),
          m_deviceAddress(std::move(deviceAddress)) {
    m_lagTimer.setInterval(20);
    m_lagTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_lagTimer, &QTimer::timeout, this, [this] {
        const qint64 now = m_clock.elapsed();
        if (m_lagLastMs) {
            m_lagMax = (std::max)(m_lagMax, now - m_lagLastMs - 20);
        }
        m_lagLastMs = now;
    });
    connect(&m_ping, &QProcess::finished, this, [this] {
        // "... time=12.3 ms" a reply; how many went, how many came back
        static const QRegularExpression time(QStringLiteral(R"(time[=<]([0-9.]+) ?ms)"));
        QList<double> ms;
        auto it = time.globalMatch(QString::fromLatin1(m_ping.readAllStandardOutput()));
        while (it.hasNext()) {
            ms.append(it.next().captured(1).toDouble());
        }
        const int sent = m_ping.property("sent").toInt();
        m_pinged = {{"routerMs", stats(ms)}, {"routerLost", (std::max)(0, sent - static_cast<int>(ms.size()))}};
    });
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &Profiler::second);
    connect(&m_ps, &QProcess::finished, this, [this] {
        // pid %cpu rss(KB) command: Mixxx (any of its builds) and this program
        QJsonObject procs;
        const qint64 self = QCoreApplication::applicationPid();
        for (const QByteArray& raw : m_ps.readAllStandardOutput().split('\n')) {
            const QList<QByteArray> f = raw.simplified().split(' ');
            if (f.size() < 4) {
                continue;
            }
            const QByteArray command = f.mid(3).join(' ');
            const QJsonObject p{{"cpu", f[1].toDouble()}, {"mb", std::round(f[2].toDouble() / 1024)}};
            if (f[0].toLongLong() == self) {
                procs.insert(QStringLiteral("connect"), p);
            } else if (command.contains("/MacOS/Mixxx") || command.endsWith("/mixxx") || command == "mixxx") {
                procs.insert(QStringLiteral("mixxx"), p);
            }
        }
        m_procs = procs;
    });
}

QString Profiler::dir() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/sessions");
}

QJsonObject Profiler::state() const {
    return {{"t", "prof"}, {"on", m_on}, {"id", m_id}, {"marks", m_marks},
            {"seconds", m_on ? static_cast<double>(m_clock.elapsed()) / 1000 : 0.0}};
}

void Profiler::write(QJsonObject line) {
    if (!line.contains(QStringLiteral("t"))) {
        line.insert(QStringLiteral("t"), static_cast<double>(m_clock.elapsed()) / 1000);
    }
    QFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return;
    }
    f.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
    if (f.size() > kMaxBytes) {
        f.close();
        stop(QStringLiteral("size"));
    }
}

void Profiler::start(const QString& by) {
    if (m_on) {
        return;
    }
    QDir().mkpath(dir());
    QFileInfoList old = QDir(dir()).entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files, QDir::Time);
    while (old.size() >= kKept) {
        QFile::remove(old.takeLast().absoluteFilePath());
    }
    m_id = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    m_path = dir() + QLatin1Char('/') + m_id + QStringLiteral(".jsonl");
    m_on = true;
    m_marks = 0;
    m_clock.start();
    m_midi->takeStats();   // (from now)
    const QFileInfo log(mixxxDir() + QStringLiteral("/mixxx.log"));
    m_logPos = log.exists() ? log.size() : 0;
    write({{"k", "start"}, {"t", 0}, {"src", "mac"}, {"at", QDateTime::currentDateTime().toString(Qt::ISODate)},
            {"by", by.isEmpty() ? QStringLiteral("this computer") : by}, {"computer", QSysInfo::machineHostName()}});
    m_router = findRouter();
    m_lagLastMs = 0;
    m_lagMax = 0;
    m_lagTimer.start();
    m_tick.start();
    emit tell({{"do", "start"}});
}

void Profiler::stop(const QString& why) {
    if (!m_on) {
        return;
    }
    write({{"k", "stop"}, {"src", "mac"}, {"why", why}});
    m_on = false;
    m_tick.stop();
    m_lagTimer.stop();
    emit tell({{"do", "stop"}});
}

void Profiler::mark(const QString& by, const QString& note) {
    if (!m_on) {
        return;
    }
    write({{"k", "mark"}, {"src", by.isEmpty() ? QStringLiteral("mac") : by}, {"n", ++m_marks}, {"note", note.left(200)}});
}

// A device's line: its own time kept as "dt", this file's timeline as "t"
void Profiler::deviceLine(const QString& device, QJsonObject line) {
    const QString k = line.value(QStringLiteral("k")).toString();
    if (!m_on) {
        if (k != QLatin1String("start")) {
            return;   // (a session here ended; the device's goes on until it's told)
        }
        start(device);
    }
    if (k == QLatin1String("mark")) {
        line.insert(QStringLiteral("n"), ++m_marks);   // (numbered here, across the devices)
    }
    line.insert(QStringLiteral("dt"), line.value(QStringLiteral("t")));
    line.insert(QStringLiteral("t"), static_cast<double>(m_clock.elapsed()) / 1000);
    line.insert(QStringLiteral("src"), device);
    write(line);
    if (k == QLatin1String("stop") && line.value(QStringLiteral("why")) == QLatin1String("asked")) {
        stop(QStringLiteral("asked on ") + device);   // (stopped on a device: the others too)
    }
}

// Mixxx's log since the last look: its warnings (and worse), and the lines about sound or MIDI giving out
void Profiler::readMixxxLog(QJsonArray* warnings) {
    QFile f(mixxxDir() + QStringLiteral("/mixxx.log"));
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    if (f.size() < m_logPos) {
        m_logPos = 0;   // (Mixxx started again: a new log)
    }
    f.seek(m_logPos);
    const QByteArray fresh = f.read(256 * 1024);
    const qsizetype end = fresh.lastIndexOf('\n') + 1;   // (whole lines; the rest next time)
    m_logPos += end;
    static const QRegularExpression notable(QStringLiteral(R"(\b(Warning|Critical|Fatal)\b|underflow|overflow|xrun|took too long)"),
            QRegularExpression::CaseInsensitiveOption);
    for (const QByteArray& line : fresh.left(end).split('\n')) {
        const QString text = QString::fromUtf8(line).trimmed();
        if (!text.isEmpty() && notable.match(text).hasMatch() && warnings->size() < 20) {
            warnings->append(text.left(300));
        }
    }
}

void Profiler::second() {
    if (m_clock.elapsed() > kMaxMs) {
        stop(QStringLiteral("time"));
        return;
    }
    const Midi::Stats s = m_midi->takeStats();
    QJsonObject mac{{"toMixxx", s.toMixxx}, {"paced", s.paced}, {"replaced", s.replaced}, {"queueMax", s.queueMax},
            {"mixxxMs", stats(s.mixxxMs)}, {"mixxxAll", [&s] {
                 QJsonArray a;
                 for (const double v : s.mixxxMs) {
                     a.append(std::round(v * 100) / 100);
                 }
                 return a;
             }()}};
    for (auto it = m_procs.constBegin(); it != m_procs.constEnd(); ++it) {
        mac.insert(it.key(), it.value());
    }
    mac.insert(QStringLiteral("lagMax"), static_cast<double>(m_lagMax));   // (this program ran that late, at worst)
    m_lagMax = 0;
    for (auto it = m_pinged.constBegin(); it != m_pinged.constEnd(); ++it) {
        mac.insert(it.key(), it.value());
    }
    m_pinged = {};
    QJsonArray warnings;
    readMixxxLog(&warnings);
    if (!warnings.isEmpty()) {
        mac.insert(QStringLiteral("warnings"), warnings);
    }
    write({{"k", "sec"}, {"src", "mac"}, {"mac", mac}});
#ifndef Q_OS_WIN
    if (m_ping.state() == QProcess::NotRunning && !m_router.isEmpty()) {   // (for the next second's line)
#ifdef Q_OS_MACOS
        const QStringList args{QStringLiteral("-c"), QStringLiteral("9"), QStringLiteral("-i"), QStringLiteral("0.1"), QStringLiteral("-t"), QStringLiteral("2"), m_router};
        m_ping.setProperty("sent", 9);
#else
        const QStringList args{QStringLiteral("-c"), QStringLiteral("4"), QStringLiteral("-i"), QStringLiteral("0.2"), QStringLiteral("-w"), QStringLiteral("2"), m_router};
        m_ping.setProperty("sent", 4);
#endif
        m_ping.start(QStringLiteral("ping"), args);
    }
    if (m_ps.state() == QProcess::NotRunning) {   // (for the next second's line)
        m_ps.start(QStringLiteral("ps"), {QStringLiteral("-A"), QStringLiteral("-o"), QStringLiteral("pid=,%cpu=,rss=,command=")});
    }
#endif
}

// The router of the network the devices are on: by the way to one of them (macOS's default route may be a VPN's)
QString Profiler::findRouter() const {
    const QString device = m_deviceAddress ? m_deviceAddress() : QString();
    const auto run = [](const QString& program, const QStringList& args) {
        QProcess p;
        p.start(program, args);
        p.waitForFinished(2000);
        return QString::fromLatin1(p.readAllStandardOutput());
    };
#if defined(Q_OS_MACOS)
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral(R"(interface:\s*(\S+))"))
                                              .match(run(QStringLiteral("route"), {QStringLiteral("-n"), QStringLiteral("get"), device.isEmpty() ? QStringLiteral("default") : device}));
    const QString router = m.hasMatch() ? run(QStringLiteral("ipconfig"), {QStringLiteral("getoption"), m.captured(1), QStringLiteral("router")}).trimmed() : QString();
    return QHostAddress(router).isNull() ? QString() : router;
#elif defined(Q_OS_LINUX)
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral(R"(\bdev\s+(\S+))"))
                                              .match(run(QStringLiteral("ip"), {QStringLiteral("route"), QStringLiteral("get"), device.isEmpty() ? QStringLiteral("1.1.1.1") : device}));
    const QRegularExpressionMatch g = QRegularExpression(QStringLiteral(R"(default via (\S+))"))
                                              .match(m.hasMatch() ? run(QStringLiteral("ip"), {QStringLiteral("route"), QStringLiteral("show"), QStringLiteral("default"), QStringLiteral("dev"), m.captured(1)}) : QString());
    return g.hasMatch() ? g.captured(1) : QString();
#else
    Q_UNUSED(device);
    Q_UNUSED(run);
    return {};
#endif
}

QJsonArray Profiler::sessions() const {
    QJsonArray out;
    for (const QFileInfo& f : QDir(dir()).entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files, QDir::Time)) {
        out.append(QJsonObject{{"id", f.completeBaseName()}, {"bytes", f.size()}});
    }
    return out;
}

QByteArray Profiler::session(const QString& id) const {
    if (!kId.match(id).hasMatch()) {
        return {};
    }
    QFile f(dir() + QLatin1Char('/') + id + QStringLiteral(".jsonl"));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool Profiler::remove(const QString& id) {
    if (!kId.match(id).hasMatch() || (m_on && id == m_id)) {
        return false;
    }
    return QFile::remove(dir() + QLatin1Char('/') + id + QStringLiteral(".jsonl"));
}

} // namespace zc
