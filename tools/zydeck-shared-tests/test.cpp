// ZyDeck's shared code (src/zydek/zydeckshared.h, the hub's and ZyDeck Connect's) against cases.json:
//   zydeck-shared-tests cases.json        prints each case that's wrong; exits 1 if any is
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <cstdio>

#include "zydeckshared.h"

namespace {
int failures = 0;
void check(bool ok, const QString& what) {
    if (!ok) {
        ++failures;
        std::printf("C++ WRONG: %s\n", qPrintable(what));
    }
}
QString show(const QString& s) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QFile f(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("cases.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("can't read %s\n", qPrintable(f.fileName()));
        return 2;
    }
    const QJsonObject cases = QJsonDocument::fromJson(f.readAll()).object();
    using namespace zydeck::shared;

    for (const QJsonValue c : cases.value(QStringLiteral("fileNames")).toArray()) {
        const QString in = c[0].toString(), want = c[1].toString(), got = safeFileName(in);
        check(got == want, QStringLiteral("safeFileName(%1) = %2, not %3").arg(show(in), show(got), show(want)));
    }
    for (const QJsonValue c : cases.value(QStringLiteral("artistTitle")).toArray()) {
        const auto got = artistTitle(c[0].toObject());
        const QJsonArray want = c[1].toArray();
        check(got.first == want[0].toString() && got.second == want[1].toString(),
                QStringLiteral("artistTitle(%1) = [%2, %3]")
                        .arg(QString::fromUtf8(QJsonDocument(c[0].toObject()).toJson(QJsonDocument::Compact)), show(got.first), show(got.second)));
    }
    for (const QJsonValue c : cases.value(QStringLiteral("values")).toArray()) {
        const double v = c[0].toDouble();
        const QList<int> bytes = encodeValue(v);
        QList<int> want;
        QByteArray raw;
        for (const QJsonValue b : c[1].toArray()) {
            want.append(b.toInt());
            raw.append(static_cast<char>(b.toInt()));
        }
        check(bytes == want, QStringLiteral("encodeValue(%1) has the wrong bytes").arg(v, 0, 'g', 17));
        check(decodeValue(raw) == c[2].toDouble(), QStringLiteral("decodeValue(bytes of %1) = %2, not %3")
                                                           .arg(v, 0, 'g', 17).arg(decodeValue(raw), 0, 'g', 17).arg(c[2].toDouble(), 0, 'g', 17));
    }
    // the link's JSON-in-SysEx, there and back
    const QJsonObject offer{{"name", QStringLiteral("Mac mini 曲")}, {"port", 8771}, {"key", QStringLiteral("00ff")}};
    const QByteArray msg = sysexText(kLinkOffer, offer);
    bool sevenBit = true;
    for (int i = 1; i + 1 < msg.size(); ++i) {
        sevenBit = sevenBit && !(static_cast<unsigned char>(msg[i]) & 0x80);
    }
    check(static_cast<unsigned char>(msg.front()) == 0xF0 && static_cast<unsigned char>(msg.back()) == 0xF7 && sevenBit,
            QStringLiteral("sysexText isn't one SysEx of 7-bit data"));
    check(sysexJson(msg) == offer, QStringLiteral("sysexJson(sysexText(offer)) isn't the offer"));
    check(sysexJson(QByteArray::fromHex("f07d61f7")).isEmpty(), QStringLiteral("sysexJson of an empty message isn't {}"));

    // the jog's message (kJog): both ways round, over the cable and as a Wi-Fi line, the far ends of its numbers too
    for (const Jog& j : {Jog{0, 1, 3, 1}, Jog{1, 2, -3, 16383}, Jog{0, 0, 0, 0}, Jog{1, 1, 8191, 77}, Jog{0, 1, -8192, 9000}}) {
        Jog got;
        const QByteArray m = jogSysex(j);
        bool sevenBit = true;
        for (int i = 1; i + 1 < m.size(); ++i) {
            sevenBit = sevenBit && static_cast<unsigned char>(m[i]) < 0x80;
        }
        check(sevenBit && jogFromSysex(m, &got) && got.side == j.side && got.touch == j.touch && got.ticks == j.ticks && got.seq == j.seq,
                QStringLiteral("jogSysex/jogFromSysex: ticks %1, seq %2 don't come back").arg(j.ticks).arg(j.seq));
        const QByteArray line = jogLine(j);
        check(line.startsWith("G ") && jogFromLine(line.mid(2), &got) && got.ticks == j.ticks && got.seq == j.seq && got.touch == j.touch,
                QStringLiteral("jogLine/jogFromLine: %1 doesn't come back").arg(QString::fromLatin1(line.trimmed())));
    }
    Jog none;
    check(!jogFromSysex(sysexText(kHardware, {}), &none), QStringLiteral("jogFromSysex took a kHardware message"));

    std::printf("C++: %s\n", failures ? "WRONG" : "all right");
    return failures ? 1 : 0;
}
