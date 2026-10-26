#pragma once
// ZyDeck: what the phone's hub (src/zydek) and ZyDeck Connect (tools/zydeck-connect) must do the same way, kept
// once, here: the controller protocol's numbers, and the names downloaded tracks and their files get. Qt only (both are Qt), header only
// (Connect builds it from its own folder). tools/zydeck-shared-tests checks it, and the copies that can't share it
// (the phone's Python, the Mixxx mapping's JavaScript), against the same cases.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <cmath>

namespace zydeck::shared {

// ZyDeck's SysEx (manufacturer 0x7D, "educational use"), the link's own messages: their text is the hex of UTF-8
// JSON, as MIDI data bytes are 7-bit
inline constexpr quint8 kLinkHello = 0x60;   // device -> computer: {"v": 1, "name": device}
inline constexpr quint8 kLinkOffer = 0x61;   // computer -> device: {"name", "hosts": [...], "port", "key"}
inline constexpr quint8 kNameAsk = 0x62;     // device -> computer: {"s": sampler?, "i": index, "ms": track length}
inline constexpr quint8 kNameReply = 0x63;   // computer -> device: {"s", "i", "ms", "n": {t, a, id, l} or null}

/// F0 7D <kind> <hex of the JSON> F7
inline QByteArray sysexText(quint8 kind, const QJsonObject& obj) {
    QByteArray m;
    m.append(static_cast<char>(0xF0)).append(static_cast<char>(0x7D)).append(static_cast<char>(kind));
    m.append(QJsonDocument(obj).toJson(QJsonDocument::Compact).toHex());
    m.append(static_cast<char>(0xF7));
    return m;
}

/// The JSON in a sysexText() message ({} if it isn't one)
inline QJsonObject sysexJson(const QByteArray& msg) {
    if (msg.size() < 5) {
        return {};
    }
    return QJsonDocument::fromJson(QByteArray::fromHex(msg.mid(3, msg.size() - 4))).object();
}

// A control's value in the mapping's SysEx ("v7"): round(value * 1e5) + 2^48 as 7 x 7 bits, LSB first, so sample
// positions (tens of millions) fit. Rounded as JavaScript's Math.round does (halves up, -0.5 to 0), as the mapping
// does at the other end
inline constexpr int kValueBytes = 7;

inline QList<int> encodeValue(double v) {
    // (not floor(v * 1e5 + 0.5): a compiler may fuse a multiply and an add into one, which rounds a value right at
    // a half the other way from JavaScript, which multiplies, then adds. volatile: the product rounded on its own,
    // whatever the compiler (GCC fuses across statements too); scaled - down is then exact)
    volatile double product = v * 1e5;
    const double scaled = product, down = std::floor(scaled);
    const qint64 n = static_cast<qint64>(scaled - down >= 0.5 ? down + 1 : down) + (qint64(1) << 48);
    QList<int> out;
    for (int i = 0; i < kValueBytes; ++i) {
        out.append(static_cast<int>((n >> (7 * i)) & 0x7F));
    }
    return out;
}

inline double decodeValue(const QByteArray& bytes) {
    qint64 n = 0;
    for (int i = 0; i < kValueBytes && i < bytes.size(); ++i) {
        n |= qint64(static_cast<unsigned char>(bytes[i]) & 0x7F) << (7 * i);
    }
    return static_cast<double>(n - (qint64(1) << 48)) / 1e5;
}

/// A name for a file made from a track's artist and title: fine on every system ZyDeck writes to (macOS, Windows,
/// Linux, Android, a NAS over SMB). Windows' and SMB's forbidden characters and control characters become spaces,
/// runs of spaces one, no spaces or dots at either end (a leading dot hides a file, a trailing one or a space is
/// refused or dropped by Windows and SMB), at most 150 characters (counted as characters, so an emoji isn't cut in
/// half), not one of Windows' device names ("CON", "nul.txt": an _ before it), "untitled" if nothing's left.
/// The phone's Python (zydek_web.py safe_filename) does the same, checked against the same cases.
inline QString safeFileName(QString s) {
    static const QRegularExpression bad(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])"));
    static const QRegularExpression reserved(QStringLiteral(R"(^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?$)"),
            QRegularExpression::CaseInsensitiveOption);
    const auto trim = [](QString& t) {
        while (!t.isEmpty() && (t.front() == QLatin1Char(' ') || t.front() == QLatin1Char('.'))) {
            t.remove(0, 1);
        }
        while (!t.isEmpty() && (t.back() == QLatin1Char(' ') || t.back() == QLatin1Char('.'))) {
            t.chop(1);
        }
    };
    s.replace(bad, QStringLiteral(" "));
    s = s.simplified();
    trim(s);
    const QList<uint> chars = s.toUcs4();
    if (chars.size() > 150) {
        s = QString::fromUcs4(reinterpret_cast<const char32_t*>(chars.constData()), 150);
        trim(s);
    }
    if (reserved.match(s).hasMatch()) {
        s.prepend(QLatin1Char('_'));
    }
    return s.isEmpty() ? QStringLiteral("untitled") : s;
}

/// "(Official Video)", "[HD]", "(Lyrics)" and the like, in a video's title
inline const QRegularExpression& titleJunk() {
    static const QRegularExpression junk(
            QStringLiteral(R"(\s*[\(\[][^\)\]]*\b(official|video|audio|lyrics?|visuali[sz]er|hd|hq|4k|mv|m/v|clip)\b[^\)\]]*[\)\]])"),
            QRegularExpression::CaseInsensitiveOption);
    return junk;
}

/// Artist and title for a downloaded track's file and tags, from yt-dlp's info: the site's own (YouTube Music),
/// else "Artist - Title" in the video's title (its junk taken out: titleJunk), else the channel ("... - Topic" and
/// "VEVO" taken off) and the title. The phone's Python (zydek_web.py guess_artist_title) does the same.
inline QPair<QString, QString> artistTitle(const QJsonObject& info) {
    QString artist = info.value(QStringLiteral("artist")).toString();
    if (artist.isEmpty()) {
        artist = info.value(QStringLiteral("artists")).toArray().at(0).toString();
    }
    const QString track = info.value(QStringLiteral("track")).toString();
    if (!artist.isEmpty() && !track.isEmpty()) {
        return {artist, track};
    }
    const QString title = info.value(QStringLiteral("title")).toString().remove(titleJunk()).trimmed();
    for (const QString& sep : {QStringLiteral(" - "), QStringLiteral(" \u2013 "), QStringLiteral(" \u2014 "), QStringLiteral(" | ")}) {
        const qsizetype at = title.indexOf(sep);
        if (at > 0) {
            return {title.left(at).trimmed(), title.mid(at + sep.size()).trimmed()};
        }
    }
    static const QRegularExpression topic(QStringLiteral(R"(\s*-\s*Topic$|VEVO$)"));
    QString channel = info.value(QStringLiteral("channel")).toString();
    if (channel.isEmpty()) {
        channel = info.value(QStringLiteral("uploader")).toString();
    }
    return {channel.remove(topic).trimmed(), title};
}

} // namespace zydeck::shared
