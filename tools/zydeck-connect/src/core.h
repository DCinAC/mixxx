#pragma once
// ZyDeck Connect's work, without a window: the MIDI port and the cables (midi.h), Mixxx's library (library.h), and
// the link over Wi-Fi, TCP port 8771:
//   "ZYDECK <key> <device name, hex>" -> "OK <this computer's name, hex>" (or "NO"), then lines both ways:
//   "M <hex MIDI>" between the device and the ZyDeck Connect port, "J <json>" asking and answering track names
//   GET /waveform/<id>?k=<key> and /beats/<id>?k=<key>: Mixxx's analysed waveform and beat grid, for the pages
// The same key goes to a device over the cable (its hello) or in the pairing code (zydeck://connect?...).

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QTcpServer>

#include "library.h"
#include "midi.h"

class QTcpSocket;

namespace zc {

inline constexpr quint16 kTcpPort = 8771;
inline constexpr const char* kAppName = "ZyDeck Connect";

/// This computer's LAN IPv4 addresses, the one with the default route first.
QStringList lanAddresses();

class Core : public QObject {
    Q_OBJECT
  public:
    explicit Core(QObject* parent = nullptr);
    bool listen();   // false: the port is taken (ZyDeck Connect, or the old ZyDeck Link, is already running)

    QString name() const { return m_name; }
    QJsonObject offer() const;   // {name, hosts, port, key}: what a device needs to link over Wi-Fi
    QString pairUrl() const;     // the same as zydeck://connect?..., for the pairing code
    Midi* midi() { return m_midi; }
    Library* library() { return &m_library; }

    struct Device {
        QString name, how;   // how: "Wi-Fi" or "USB cable"
    };
    QList<Device> devices() const;   // linked over Wi-Fi, or said hello over a cable in the last 10 s
    QString status;                  // the last thing done from the window ("" for none)

  signals:
    void changed();   // a device came or went

  private:
    struct Client;
    void connection();
    void read(QTcpSocket* s);
    void line(QTcpSocket* s, Client& c, const QByteArray& line);
    void http(QTcpSocket* s, const QByteArray& request);
    void fromMixxx(const QByteArray& msg);
    QJsonObject nameReply(const QJsonObject& ask);
    void seen(const QString& device, const QString& how);

    QString m_name, m_key;
    Library m_library;
    Midi* m_midi;
    QTcpServer m_server;
    QHash<QTcpSocket*, Client*> m_clients;
    struct Seen {
        QString how;
        qint64 at;
    };
    QHash<QString, Seen> m_seen;
};

} // namespace zc
