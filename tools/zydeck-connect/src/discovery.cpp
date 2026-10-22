#include "discovery.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QUrlQuery>

namespace zc {

namespace {
constexpr quint16 kZyDeckPort = 8766, kMdnsPort = 5353;
const QHostAddress kMdnsGroup(QStringLiteral("224.0.0.251"));

/// An mDNS question for "_zydeck._tcp.local" PTR, answer straight to us (the QU bit)
QByteArray mdnsQuestion() {
    QByteArray q;
    q.append("\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00", 12);   // id 0, flags 0, one question
    for (const char* label : {"_zydeck", "_tcp", "local"}) {
        q.append(static_cast<char>(qstrlen(label))).append(label);
    }
    q.append("\x00\x00\x0c\x80\x01", 5);   // end of name, type PTR, class IN with QU
    return q;
}

qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}
} // namespace

Discovery::Discovery(QObject* parent)
        : QObject(parent) {
    m_mdns.bind(QHostAddress::AnyIPv4, 0);   // (answers to a question from another port come back here)
    connect(&m_mdns, &QUdpSocket::readyRead, this, &Discovery::readMdns);
    connect(&m_mdnsTimer, &QTimer::timeout, this, &Discovery::askMdns);
    m_mdnsTimer.start(10000);
    connect(&m_probeTimer, &QTimer::timeout, this, &Discovery::probeNetwork);
    m_probeTimer.start(60000);
    QTimer::singleShot(300, this, &Discovery::scanNow);
}

void Discovery::scanNow() {
    askMdns();
    probeNetwork();
}

void Discovery::askMdns() {
    m_mdns.writeDatagram(mdnsQuestion(), kMdnsGroup, kMdnsPort);
}

/// Anything that names _zydeck: its sender is a ZyDeck (asked for its name to be sure)
void Discovery::readMdns() {
    while (m_mdns.hasPendingDatagrams()) {
        QHostAddress from;
        QByteArray data(static_cast<int>(m_mdns.pendingDatagramSize()), 0);
        m_mdns.readDatagram(data.data(), data.size(), &from);
        if (data.toLower().contains("_zydeck")) {
            bool ok = false;
            const QHostAddress v4(from.toIPv4Address(&ok));
            if (ok) {
                whoami(v4.toString());
            }
        }
    }
}

/// Every address of this computer's (private, not too big) networks, asked whether it's a ZyDeck
void Discovery::probeNetwork() {
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces()) {
        if (!(i.flags() & QNetworkInterface::IsUp) || (i.flags() & QNetworkInterface::IsLoopBack) ||
                i.type() == QNetworkInterface::Virtual) {
            continue;
        }
        for (const QNetworkAddressEntry& e : i.addressEntries()) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol || e.prefixLength() < 22 ||
                    !(e.ip().isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8) ||
                            e.ip().isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12) ||
                            e.ip().isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16))) {
                continue;   // (a public network, or a huge one: not swept)
            }
            const quint32 mask = e.netmask().toIPv4Address(), net = e.ip().toIPv4Address() & mask;
            for (quint32 host = 1; host < (~mask); ++host) {
                const QHostAddress a(net | host);
                if (a != e.ip()) {
                    whoami(a.toString());
                }
            }
        }
    }
}

void Discovery::whoami(const QString& address) {
    const qint64 now = nowMs();
    if (now - m_probing.value(address) < 5000) {
        return;
    }
    m_probing.insert(address, now);
    QNetworkRequest req(QUrl(QStringLiteral("http://%1:%2/api/whoami").arg(address).arg(kZyDeckPort)));
    req.setTransferTimeout(1500);
    QNetworkReply* r = m_net.get(req);
    connect(r, &QNetworkReply::finished, this, [this, r, address] {
        r->deleteLater();
        m_probing.remove(address);
        if (r->error() != QNetworkReply::NoError) {
            return;
        }
        const QJsonObject j = QJsonDocument::fromJson(r->readAll()).object();
        if (j.value(QStringLiteral("zydeck")).toBool()) {
            seen(address, j.value(QStringLiteral("name")).toString());
        }
    });
}

void Discovery::seen(const QString& address, const QString& name) {
    const bool fresh = !m_found.contains(address) || m_found.value(address).name != name;
    Found& f = m_found[address];
    f.name = name.isEmpty() ? address : name;
    f.address = address;
    f.seen = nowMs();
    if (!m_order.contains(address)) {
        m_order.append(address);
    }
    if (fresh) {
        emit changed();
    }
}

QList<Discovery::Found> Discovery::found() const {
    QList<Found> out;
    const qint64 now = nowMs();
    for (const QString& a : m_order) {
        const auto it = m_found.constFind(a);
        if (it != m_found.cend() && now - it->seen < 120000) {
            out.append(*it);
        }
    }
    return out;
}

void Discovery::offer(const QString& address, const QJsonObject& offer) {
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("offer"), QString::fromUtf8(QJsonDocument(offer).toJson(QJsonDocument::Compact)));
    QUrl url(QStringLiteral("http://%1:%2/api/offer").arg(address).arg(kZyDeckPort));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setTransferTimeout(3000);
    QNetworkReply* r = m_net.get(req);
    connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
    if (m_found.contains(address)) {
        m_found[address].asked = nowMs();
        emit changed();
    }
}

} // namespace zc
