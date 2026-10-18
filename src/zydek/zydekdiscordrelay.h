#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTcpServer>
#include <QTimer>

class QTcpSocket;

namespace zydek {

/// The ZyDeck Discord bot's end of Mixxx's Live Broadcasting, wherever the bot is.
///
/// Mixxx (libshout) streams to an Icecast-style source on this device (127.0.0.1, a random port and password);
/// this sends the stream on to the bot over one WebSocket: ws:// to a bot on the same network ("192.168.0.100:
/// 8780"), wss:// through a Cloudflare tunnel ("https://….trycloudflare.com"). Cloudflare passes WebSockets but
/// not libshout's own request (SOURCE, a body of no length). The stream's header pages are kept and sent first
/// on each new connection to the bot, so it can come and go (a bot restart, the tunnel reconnecting) mid-set.
class DiscordRelay : public QObject {
    Q_OBJECT
  public:
    explicit DiscordRelay(QObject* pParent);
    ~DiscordRelay() override;

    /// Listens for Mixxx; the bot: "host[:port]", "http://host[:port]" or "https://host" (wss). false: no port.
    bool start(const QString& bot, const QString& key);
    void stop();
    /// A new address or key while streaming: to the bot again
    void reconnect();
    quint16 port() const;
    const QString& password() const {
        return m_password;
    }
    /// 0 waiting for Mixxx · 1 connecting to the bot · 2 streaming to it · 3 the bot can't be reached (error())
    int state() const;
    const QString& error() const {
        return m_error;
    }

    struct Target {
        bool tls = false;
        QString host;
        quint16 port = 0;
        bool valid() const {
            return !host.isEmpty() && port > 0;
        }
    };
    /// "192.168.0.100:8780", "http://x:8780", "https://x.trycloudflare.com" (a host alone: 8780 on the network,
    /// 443 and TLS for a trycloudflare.com one)
    static Target parseTarget(const QString& bot);

  private:
    void onSourceConnection();
    void onSourceData();
    void onSourceGone();
    void onPage(const QByteArray& page, bool header);
    void connectUp();
    void onUpConnected();
    void onUpData();
    void closeUp(const QString& error);
    void sendFrame(int opcode, const QByteArray& payload);

    QTcpServer m_server;
    QString m_password;
    Target m_target;
    QString m_key;

    QPointer<QTcpSocket> m_pSource;
    QByteArray m_sourceHead;
    bool m_sourceStreaming = false;
    QByteArray m_ogg;              // incoming bytes not yet a whole page
    QList<QByteArray> m_headers;   // OpusHead, OpusTags pages
    bool m_audioStarted = false;

    QPointer<QTcpSocket> m_pUp;
    QByteArray m_upIn;
    bool m_upOpen = false;
    QString m_error;
    QTimer m_retry;
};

} // namespace zydek
