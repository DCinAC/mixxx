#pragma once
// The session profiler, the computer's side. A session starts on any device (its hub's /api/profile) or here
// (/web/profile, or the page at /profile): each device linked over Wi-Fi then records too, and sends its lines here
// as they're written ("P <json>" on the link), so this file has every device's seconds and this computer's on one
// timeline. This computer's, once a second: the devices' pings' time from here into Mixxx and back (Mixxx's own leg
// of their round trip), how the messages into Mixxx were paced, Mixxx's and ZyDeck Connect's CPU and memory, and
// what Mixxx's log warned of. In ZyDeck Connect's data folder, sessions/<start>.jsonl; the newest 30.

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <functional>

namespace zc {

class Midi;

class Profiler : public QObject {
    Q_OBJECT
  public:
    // deviceAddress: a linked device's address, to find the network they're on (and its router)
    Profiler(Midi* midi, std::function<QString()> deviceAddress, QObject* parent = nullptr);

    bool on() const { return m_on; }
    void start(const QString& by);                   // by: a device's name, or "" (here)
    void stop(const QString& why);
    void mark(const QString& by, const QString& note);
    void deviceLine(const QString& device, QJsonObject line);   // a line of a device's own session
    QJsonObject state() const;
    QJsonArray sessions() const;
    QByteArray session(const QString& id) const;    // its lines ("" if there's none)
    bool remove(const QString& id);

  signals:
    void tell(const QJsonObject& control);   // to the linked devices: {"do": "start" | "stop"}

  private:
    void second();
    void write(QJsonObject line);
    QString dir() const;
    void readMixxxLog(QJsonArray* warnings);

    Midi* m_midi;
    bool m_on = false;
    QString m_id, m_path;
    QElapsedTimer m_clock;
    int m_marks = 0;
    QTimer m_tick;
    QProcess m_ps;              // the processes' CPU and memory (ps), asked once a second
    QJsonObject m_procs;        // what it last said
    std::function<QString()> m_deviceAddress;
    QTimer m_lagTimer;          // this program's own lateness: a 20 ms timer, how late it runs (its thread's busy)
    qint64 m_lagLastMs = 0, m_lagMax = 0;
    QString m_router;           // the router of the network the devices are on ("": not known)
    QProcess m_ping;            // this computer's own Wi-Fi: the router pinged, ~10 times in each second
    QJsonObject m_pinged;       // what the last round said
    QString findRouter() const;
    qint64 m_logPos = -1;       // how far into Mixxx's log has been read
};

} // namespace zc
