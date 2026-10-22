#pragma once
// ZyDecks on this computer's network, found by themselves, as their own Link devices scan finds them: each announces
// "_zydeck._tcp" (port 8766) over mDNS, and answers GET /api/whoami ({"zydeck": true, "name": ...}). Asked both
// ways: an mDNS question every 10 s (the answers come straight back to us), and every address of this computer's
// networks once a minute (Android doesn't announce on its own hotspot, and some routers don't pass announcements on).
// offer() asks one to link to this computer: it shows "Control Mixxx on ...?" and links if the user says yes.

#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

namespace zc {

class Discovery : public QObject {
    Q_OBJECT
  public:
    explicit Discovery(QObject* parent = nullptr);

    struct Found {
        QString name, address;   // as it calls itself; its IPv4 address
        qint64 seen = 0;         // ms since the epoch
        qint64 asked = 0;        // when offer() asked it (0: not)
    };
    QList<Found> found() const;   // the ones seen in the last 2 minutes, in the order they turned up
    void offer(const QString& address, const QJsonObject& offer);   // ask it to link (it asks its user)
    void scanNow();

  signals:
    void changed();

  private:
    void askMdns();
    void readMdns();
    void probeNetwork();
    void whoami(const QString& address);
    void seen(const QString& address, const QString& name);

    QUdpSocket m_mdns;
    QNetworkAccessManager m_net;
    QTimer m_mdnsTimer, m_probeTimer;
    QHash<QString, Found> m_found;   // address -> it
    QList<QString> m_order;
    QHash<QString, qint64> m_probing;   // address -> when asked (not again while waiting)
};

} // namespace zc
