#include "zydek/zydekdiscordrelay.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QSslSocket>
#include <QTcpSocket>
#include <QUrl>
#include <QtEndian>

#include "moc_zydekdiscordrelay.cpp"

namespace zydek {

namespace {
constexpr int kMaxHead = 16384;
constexpr qint64 kMaxBacklog = 512 * 1024;   // ~30 s at 128 kbps: past that the bot's link is too slow, skip audio
constexpr int kRetryMs = 3000;

QByteArray randomBytes(int n) {
    QByteArray out(n, Qt::Uninitialized);
    for (char& c : out) {
        c = static_cast<char>(QRandomGenerator::global()->bounded(256));
    }
    return out;
}
} // namespace

DiscordRelay::DiscordRelay(QObject* pParent)
        : QObject(pParent) {
    connect(&m_server, &QTcpServer::newConnection, this, &DiscordRelay::onSourceConnection);
    m_retry.setSingleShot(true);
    m_retry.setInterval(kRetryMs);
    connect(&m_retry, &QTimer::timeout, this, &DiscordRelay::connectUp);
}

DiscordRelay::~DiscordRelay() {
    stop();
}

DiscordRelay::Target DiscordRelay::parseTarget(const QString& bot) {
    Target t;
    QString text = bot.trimmed();
    if (text.isEmpty()) {
        return t;
    }
    if (!text.contains(QStringLiteral("://"))) {
        text.prepend(text.contains(QStringLiteral("trycloudflare.com")) ? QStringLiteral("https://") : QStringLiteral("http://"));
    }
    const QUrl url(text);
    t.tls = url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("wss");
    t.host = url.host();
    t.port = static_cast<quint16>(url.port(t.tls ? 443 : (bot.contains(QStringLiteral("://")) ? 80 : 8780)));
    return t;
}

bool DiscordRelay::start(const QString& bot, const QString& key) {
    m_target = parseTarget(bot);
    m_key = key;
    m_error.clear();
    if (!m_server.isListening()) {
        m_password = QString::fromLatin1(randomBytes(18).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        if (!m_server.listen(QHostAddress::LocalHost, 0)) {
            m_error = m_server.errorString();
            return false;
        }
    }
    return true;
}

void DiscordRelay::stop() {
    m_retry.stop();
    closeUp(QString());
    if (m_pSource) {
        m_pSource->abort();
        m_pSource->deleteLater();
    }
    m_server.close();
    m_sourceStreaming = false;
    m_error.clear();
}

void DiscordRelay::reconnect() {
    m_retry.stop();
    closeUp(QString());
    m_error.clear();
    connectUp();
}

quint16 DiscordRelay::port() const {
    return m_server.serverPort();
}

int DiscordRelay::state() const {
    if (!m_sourceStreaming) {
        return 0;
    }
    if (m_upOpen) {
        return 2;
    }
    return m_error.isEmpty() ? 1 : 3;
}

// ---- Mixxx's side: an Icecast-style source ------------------------------------------------------------------

void DiscordRelay::onSourceConnection() {
    while (QTcpSocket* pSocket = m_server.nextPendingConnection()) {
        if (m_pSource && m_sourceStreaming) {   // the newest source wins (libshout reconnecting)
            m_pSource->abort();
            m_pSource->deleteLater();
            m_sourceStreaming = false;
        }
        m_pSource = pSocket;
        m_sourceHead.clear();
        connect(pSocket, &QTcpSocket::readyRead, this, &DiscordRelay::onSourceData);
        connect(pSocket, &QTcpSocket::disconnected, this, [this, pSocket] {
            if (pSocket == m_pSource) {
                onSourceGone();
            }
            pSocket->deleteLater();
        });
    }
}

void DiscordRelay::onSourceData() {
    QTcpSocket* pSocket = m_pSource;
    if (!pSocket) {
        return;
    }
    if (m_sourceStreaming) {
        m_ogg.append(pSocket->readAll());
    } else {
        m_sourceHead.append(pSocket->readAll());
        // libshout: "SOURCE /mount HTTP/1.0" without a password first (told 401), then with it
        while (!m_sourceStreaming) {
            const int end = m_sourceHead.indexOf("\r\n\r\n");
            if (end < 0) {
                if (m_sourceHead.size() > kMaxHead) {
                    pSocket->abort();
                }
                return;
            }
            const QByteArray head = m_sourceHead.left(end);
            m_sourceHead.remove(0, end + 4);
            const QByteArray expected = "Basic " + ("source:" + m_password.toUtf8()).toBase64();
            bool ok = false;
            for (const QByteArray& line : head.split('\n')) {
                const int colon = line.indexOf(':');
                if (colon > 0 && line.left(colon).trimmed().toLower() == "authorization") {
                    ok = line.mid(colon + 1).trimmed() == expected;
                }
            }
            if (!ok) {
                pSocket->write("HTTP/1.0 401 Authentication Required\r\nWWW-Authenticate: Basic realm=\"ZyDeck\"\r\n"
                               "Allow: SOURCE\r\nContent-Length: 0\r\n\r\n");
                continue;
            }
            pSocket->write("HTTP/1.0 200 OK\r\n\r\n");
            m_sourceStreaming = true;
            m_ogg = m_sourceHead;
            m_sourceHead.clear();
            m_headers.clear();
            m_audioStarted = false;
            m_error.clear();
            connectUp();
        }
    }
    // whole Ogg pages, so a new connection to the bot starts on a page boundary
    while (m_ogg.size() >= 27) {
        if (!m_ogg.startsWith("OggS")) {
            const int at = m_ogg.indexOf("OggS", 1);
            m_ogg.remove(0, at < 0 ? m_ogg.size() - 3 : at);
            if (at < 0) {
                return;
            }
            continue;
        }
        const int segments = static_cast<unsigned char>(m_ogg[26]);
        if (m_ogg.size() < 27 + segments) {
            return;
        }
        int size = 27 + segments;
        for (int i = 0; i < segments; ++i) {
            size += static_cast<unsigned char>(m_ogg[27 + i]);
        }
        if (m_ogg.size() < size) {
            return;
        }
        const QByteArray page = m_ogg.left(size);
        m_ogg.remove(0, size);
        const qint64 granule = qFromLittleEndian<qint64>(page.constData() + 6);
        onPage(page, !m_audioStarted && granule == 0);
    }
}

void DiscordRelay::onSourceGone() {
    m_sourceStreaming = false;
    m_ogg.clear();
    m_retry.stop();
    closeUp(QString());   // the bot hears the set stop
}

void DiscordRelay::onPage(const QByteArray& page, bool header) {
    if (header) {
        m_headers.append(page);
    } else {
        m_audioStarted = true;
    }
    if (m_upOpen && m_pUp) {
        if (!header && m_pUp->bytesToWrite() > kMaxBacklog) {
            return;   // the link to the bot can't keep up: skip ahead rather than fall behind
        }
        sendFrame(0x2, page);
    }
}

// ---- the bot's side: a WebSocket client (RFC 6455) -----------------------------------------------------------

void DiscordRelay::connectUp() {
    if (m_pUp || !m_sourceStreaming) {
        return;
    }
    if (!m_target.valid()) {
        m_error = QStringLiteral("No bot address");
        return;
    }
    QTcpSocket* pSocket;
    if (m_target.tls) {
        auto* pSsl = new QSslSocket(this);
        connect(pSsl, &QSslSocket::encrypted, this, &DiscordRelay::onUpConnected);
        pSocket = pSsl;
    } else {
        pSocket = new QTcpSocket(this);
        connect(pSocket, &QTcpSocket::connected, this, &DiscordRelay::onUpConnected);
    }
    m_pUp = pSocket;
    m_upIn.clear();
    m_upOpen = false;
    pSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(pSocket, &QTcpSocket::readyRead, this, &DiscordRelay::onUpData);
    connect(pSocket, &QTcpSocket::errorOccurred, this, [this, pSocket] {
        if (pSocket == m_pUp) {
            closeUp(pSocket->errorString());
        }
    });
    connect(pSocket, &QTcpSocket::disconnected, this, [this, pSocket] {
        if (pSocket == m_pUp) {
            closeUp(m_upOpen ? QStringLiteral("The bot hung up") : QStringLiteral("The bot didn't answer"));
        }
    });
    if (m_target.tls) {
        static_cast<QSslSocket*>(pSocket)->connectToHostEncrypted(m_target.host, m_target.port);
    } else {
        pSocket->connectToHost(m_target.host, m_target.port);
    }
    QTimer::singleShot(10000, pSocket, [this, pSocket] {
        if (pSocket == m_pUp && !m_upOpen) {
            closeUp(QStringLiteral("The bot didn't answer in 10 s"));
        }
    });
}

void DiscordRelay::onUpConnected() {
    if (!m_pUp) {
        return;
    }
    const QByteArray hostHeader = m_target.host.toUtf8() +
            ((m_target.tls ? m_target.port != 443 : m_target.port != 80) ? ':' + QByteArray::number(m_target.port) : QByteArray());
    m_pUp->write("GET /ingest HTTP/1.1\r\nHost: " + hostHeader +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
            randomBytes(16).toBase64() + "\r\nAuthorization: Bearer " + m_key.toUtf8() +
            "\r\nUser-Agent: ZyDeck\r\n\r\n");
}

void DiscordRelay::onUpData() {
    if (!m_pUp) {
        return;
    }
    m_upIn.append(m_pUp->readAll());
    if (!m_upOpen) {
        const int end = m_upIn.indexOf("\r\n\r\n");
        if (end < 0) {
            if (m_upIn.size() > kMaxHead) {
                closeUp(QStringLiteral("The bot's answer made no sense"));
            }
            return;
        }
        const QByteArray status = m_upIn.left(m_upIn.indexOf("\r\n"));
        m_upIn.remove(0, end + 4);
        if (!status.contains(" 101")) {
            closeUp(status.contains(" 401") ? QStringLiteral("The bot doesn't take this stream key")
                                            : QStringLiteral("The bot said: %1").arg(QString::fromUtf8(status.mid(status.indexOf(' ') + 1))));
            return;
        }
        m_upOpen = true;
        m_error.clear();
        for (const QByteArray& page : std::as_const(m_headers)) {
            sendFrame(0x2, page);
        }
    }
    // frames from the bot (unmasked): answer pings, and a close
    while (m_upIn.size() >= 2) {
        const int opcode = m_upIn[0] & 0x0F;
        quint64 length = m_upIn[1] & 0x7F;
        int offset = 2;
        if (length == 126) {
            if (m_upIn.size() < 4) {
                return;
            }
            length = qFromBigEndian<quint16>(m_upIn.constData() + 2);
            offset = 4;
        } else if (length == 127) {
            if (m_upIn.size() < 10) {
                return;
            }
            length = qFromBigEndian<quint64>(m_upIn.constData() + 2);
            offset = 10;
        }
        if (length > kMaxHead || static_cast<quint64>(m_upIn.size()) < offset + length) {
            if (length > kMaxHead) {
                closeUp(QStringLiteral("The bot's answer made no sense"));
            }
            return;
        }
        const QByteArray payload = m_upIn.mid(offset, static_cast<int>(length));
        m_upIn.remove(0, offset + static_cast<int>(length));
        if (opcode == 0x9) {
            sendFrame(0xA, payload);
        } else if (opcode == 0x8) {
            closeUp(QStringLiteral("The bot closed the connection"));
            return;
        }
    }
}

void DiscordRelay::closeUp(const QString& error) {
    const bool wasOpen = m_upOpen;
    m_upOpen = false;
    if (m_pUp) {
        QTcpSocket* pSocket = m_pUp;
        m_pUp = nullptr;
        if (wasOpen && pSocket->state() == QAbstractSocket::ConnectedState) {
            const QByteArray mask = randomBytes(4);
            pSocket->write(QByteArray("\x88\x80", 2) + mask);   // close, no payload
            pSocket->flush();
        }
        pSocket->disconnect(this);
        pSocket->abort();
        pSocket->deleteLater();
    }
    if (!error.isEmpty()) {
        m_error = error;
    }
    if (m_sourceStreaming && !error.isEmpty()) {
        m_retry.start();   // Mixxx is still sending: try the bot again
    }
}

void DiscordRelay::sendFrame(int opcode, const QByteArray& payload) {
    QByteArray frame;
    frame.reserve(payload.size() + 14);
    frame.append(static_cast<char>(0x80 | opcode));
    const qsizetype n = payload.size();
    if (n < 126) {
        frame.append(static_cast<char>(0x80 | n));
    } else if (n < 65536) {
        frame.append(static_cast<char>(0x80 | 126));
        frame.append(static_cast<char>(n >> 8));
        frame.append(static_cast<char>(n & 0xFF));
    } else {
        frame.append(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) {
            frame.append(static_cast<char>((static_cast<quint64>(n) >> (8 * i)) & 0xFF));
        }
    }
    const QByteArray mask = randomBytes(4);
    frame.append(mask);
    const qsizetype start = frame.size();
    frame.append(payload);
    for (qsizetype i = 0; i < n; ++i) {
        frame[start + i] = static_cast<char>(frame[start + i] ^ mask[i % 4]);
    }
    m_pUp->write(frame);
}

} // namespace zydek
