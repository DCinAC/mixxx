#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTcpServer>
#include <functional>

class QTcpSocket;

namespace zydek {

/// A small HTTP/1.1 and WebSocket (RFC 6455) server for the Zydek tablet and phone pages.
///
/// Mixxx's Android dependencies don't include Qt's WebSockets or HTTP server modules, and the pages only
/// need GET requests and text messages, so this is built directly on QTcpServer. One port serves both:
/// a request with "Upgrade: websocket" becomes a WebSocket on that path, anything else is answered by
/// the request handler and the connection closed.
class HttpServer : public QObject {
    Q_OBJECT
  public:
    struct Response {
        int status = 200;
        QByteArray contentType = "text/plain; charset=utf-8";
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;
    };
    using Query = QHash<QString, QString>;
    using Handler = std::function<Response(const QString& path, const Query& query)>;

    explicit HttpServer(QObject* pParent = nullptr);
    ~HttpServer() override;

    /// Listen on all interfaces (the tablet connects over WiFi).
    bool listen(quint16 port);
    quint16 port() const {
        return m_server.serverPort();
    }
    void setHandler(Handler handler) {
        m_handler = std::move(handler);
    }

    void sendText(QTcpSocket* pClient, const QByteArray& utf8);
    /// Send to every WebSocket client connected on this path.
    void broadcast(const QString& path, const QByteArray& utf8);
    QList<QTcpSocket*> clients(const QString& path) const;

  signals:
    void wsOpened(QTcpSocket* pClient, const QString& path, const zydek::HttpServer::Query& query);
    void wsText(QTcpSocket* pClient, const QString& path, const QByteArray& text);
    void wsClosed(QTcpSocket* pClient, const QString& path);

  private:
    struct Connection {
        QByteArray buffer;
        bool webSocket = false;
        QString path;
        QByteArray fragments;   // a message split over several frames
        int fragmentOpcode = 0;
    };

    void onNewConnection();
    void onReadyRead(QTcpSocket* pSocket);
    void onDisconnected(QTcpSocket* pSocket);
    void handleRequest(QTcpSocket* pSocket, Connection& conn);
    void handleFrames(QTcpSocket* pSocket, Connection& conn);
    void writeFrame(QTcpSocket* pSocket, int opcode, const QByteArray& payload);

    QTcpServer m_server;
    QHash<QTcpSocket*, Connection> m_connections;
    Handler m_handler;
};

} // namespace zydek
