#include "zydek/zydekhttpserver.h"

#include <QCryptographicHash>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include "moc_zydekhttpserver.cpp"

namespace zydek {

namespace {

constexpr int kMaxHeaderBytes = 16 * 1024;
constexpr qint64 kMaxMessageBytes = 4 * 1024 * 1024;
const QByteArray kWebSocketGuid = QByteArrayLiteral("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

enum Opcode {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

QByteArray reasonPhrase(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 101:
        return "Switching Protocols";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    default:
        return "Error";
    }
}

} // namespace

HttpServer::HttpServer(QObject* pParent)
        : QObject(pParent) {
    connect(&m_server, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
}

HttpServer::~HttpServer() {
    m_server.close();
}

bool HttpServer::listen(quint16 port) {
    return m_server.listen(QHostAddress::Any, port);
}

void HttpServer::onNewConnection() {
    while (QTcpSocket* pSocket = m_server.nextPendingConnection()) {
        // Every touch becomes a small message: don't let Nagle's algorithm hold them back.
        pSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_connections.insert(pSocket, Connection{});
        connect(pSocket, &QTcpSocket::readyRead, this, [this, pSocket] { onReadyRead(pSocket); });
        connect(pSocket, &QTcpSocket::disconnected, this, [this, pSocket] { onDisconnected(pSocket); });
    }
}

void HttpServer::onDisconnected(QTcpSocket* pSocket) {
    auto it = m_connections.find(pSocket);
    if (it != m_connections.end()) {
        const bool wasWebSocket = it->webSocket;
        const QString path = it->path;
        m_connections.erase(it);
        if (wasWebSocket) {
            emit wsClosed(pSocket, path);
        }
    }
    pSocket->deleteLater();
}

void HttpServer::onReadyRead(QTcpSocket* pSocket) {
    auto it = m_connections.find(pSocket);
    if (it == m_connections.end()) {
        return;
    }
    it->buffer.append(pSocket->readAll());
    if (it->webSocket) {
        handleFrames(pSocket, *it);
        return;
    }
    if (it->buffer.indexOf("\r\n\r\n") < 0) {
        if (it->buffer.size() > kMaxHeaderBytes) {
            pSocket->abort();
        }
        return;
    }
    handleRequest(pSocket, *it);
}

void HttpServer::handleRequest(QTcpSocket* pSocket, Connection& conn) {
    const int headerEnd = conn.buffer.indexOf("\r\n\r\n");
    const QList<QByteArray> lines = conn.buffer.left(headerEnd).split('\n');
    conn.buffer.remove(0, headerEnd + 4);

    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    QHash<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i) {
        const int colon = lines[i].indexOf(':');
        if (colon > 0) {
            headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
    }
    // "+" in a query is a space (form encoding); a real plus arrives as %2B
    QByteArray target = requestLine.value(1);
    const qsizetype queryStart = target.indexOf('?');
    if (queryStart >= 0) {
        target = target.left(queryStart) + target.mid(queryStart).replace('+', "%20");
    }
    const QUrl url(QString::fromUtf8(target));
    const QString path = url.path();
    Query query;
    for (const auto& item : QUrlQuery(url).queryItems(QUrl::FullyDecoded)) {
        query.insert(item.first, item.second);
    }

    auto respond = [pSocket](const Response& response) {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(response.status) + ' ' +
                reasonPhrase(response.status) + "\r\nContent-Type: " + response.contentType +
                "\r\nContent-Length: " + QByteArray::number(response.body.size()) +
                "\r\nConnection: close\r\n";
        for (const auto& header : response.headers) {
            out += header.first + ": " + header.second + "\r\n";
        }
        out += "\r\n" + response.body;
        pSocket->write(out);
        pSocket->disconnectFromHost();
    };

    if (requestLine.value(0) != "GET") {
        respond({405, "text/plain", "GET only\n", {}});
        return;
    }

    if (headers.value("upgrade").toLower() == "websocket") {
        const QByteArray key = headers.value("sec-websocket-key");
        if (key.isEmpty()) {
            respond({400, "text/plain", "missing Sec-WebSocket-Key\n", {}});
            return;
        }
        const QByteArray accept =
                QCryptographicHash::hash(QByteArray(key + kWebSocketGuid), QCryptographicHash::Sha1).toBase64();
        pSocket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " +
                accept + "\r\n\r\n");
        conn.webSocket = true;
        conn.path = path;
        emit wsOpened(pSocket, path, query);
        if (!conn.buffer.isEmpty()) {
            handleFrames(pSocket, conn);
        }
        return;
    }

    respond(m_handler ? m_handler(path, query) : Response{404, "text/plain", "not found\n", {}});
}

void HttpServer::handleFrames(QTcpSocket* pSocket, Connection& conn) {
    QByteArray& buf = conn.buffer;
    while (buf.size() >= 2) {
        const auto b0 = static_cast<unsigned char>(buf[0]);
        const auto b1 = static_cast<unsigned char>(buf[1]);
        const bool fin = b0 & 0x80;
        const int opcode = b0 & 0x0F;
        const bool masked = b1 & 0x80;
        qint64 length = b1 & 0x7F;
        int pos = 2;
        if (length == 126) {
            if (buf.size() < 4) {
                return;
            }
            length = (static_cast<unsigned char>(buf[2]) << 8) | static_cast<unsigned char>(buf[3]);
            pos = 4;
        } else if (length == 127) {
            if (buf.size() < 10) {
                return;
            }
            length = 0;
            for (int i = 0; i < 8; ++i) {
                length = (length << 8) | static_cast<unsigned char>(buf[2 + i]);
            }
            pos = 10;
        }
        if (length > kMaxMessageBytes || !masked) {   // clients must mask their frames
            pSocket->abort();
            return;
        }
        if (buf.size() < pos + 4 + length) {
            return;   // wait for the rest of the frame
        }
        const QByteArray mask = buf.mid(pos, 4);
        QByteArray payload = buf.mid(pos + 4, length);
        for (int i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }
        buf.remove(0, pos + 4 + static_cast<int>(length));

        switch (opcode) {
        case kPing:
            writeFrame(pSocket, kPong, payload);
            break;
        case kPong:
            break;
        case kClose:
            writeFrame(pSocket, kClose, payload.left(2));
            pSocket->disconnectFromHost();
            return;
        case kText:
        case kBinary:
        case kContinuation:
            if (opcode != kContinuation) {
                conn.fragmentOpcode = opcode;
                conn.fragments.clear();
            }
            conn.fragments += payload;
            if (fin) {
                const QByteArray message = std::move(conn.fragments);
                conn.fragments.clear();
                if (conn.fragmentOpcode == kText) {
                    emit wsText(pSocket, conn.path, message);
                }
            }
            break;
        default:
            pSocket->abort();
            return;
        }
    }
}

void HttpServer::writeFrame(QTcpSocket* pSocket, int opcode, const QByteArray& payload) {
    QByteArray frame;
    frame.reserve(payload.size() + 10);
    frame.append(static_cast<char>(0x80 | opcode));
    const qint64 n = payload.size();
    if (n < 126) {
        frame.append(static_cast<char>(n));
    } else if (n < 65536) {
        frame.append(static_cast<char>(126));
        frame.append(static_cast<char>(n >> 8));
        frame.append(static_cast<char>(n & 0xFF));
    } else {
        frame.append(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            frame.append(static_cast<char>((n >> (8 * i)) & 0xFF));
        }
    }
    frame.append(payload);
    pSocket->write(frame);
}

void HttpServer::sendText(QTcpSocket* pClient, const QByteArray& utf8) {
    if (pClient && m_connections.contains(pClient) && m_connections.value(pClient).webSocket) {
        writeFrame(pClient, kText, utf8);
    }
}

void HttpServer::broadcast(const QString& path, const QByteArray& utf8) {
    for (auto it = m_connections.cbegin(); it != m_connections.cend(); ++it) {
        if (it->webSocket && it->path == path) {
            writeFrame(it.key(), kText, utf8);
        }
    }
}

QList<QTcpSocket*> HttpServer::clients(const QString& path) const {
    QList<QTcpSocket*> out;
    for (auto it = m_connections.cbegin(); it != m_connections.cend(); ++it) {
        if (it->webSocket && it->path == path) {
            out.append(it.key());
        }
    }
    return out;
}

} // namespace zydek
