#pragma once
// The "ZyDeck Connect" MIDI port for Mixxx, and every other MIDI input watched for ZyDeck's hello over the USB
// cable. RtMidi's callbacks come on its own threads: what they find goes out as signals (queued to the receiver).

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>

class RtMidiIn;
class RtMidiOut;

namespace zc {

inline constexpr const char* kPortName = "ZyDeck Connect";
// ZyDeck's SysEx (manufacturer 0x7D, "educational use"): text is the hex of UTF-8 JSON (MIDI data bytes are 7-bit)
inline constexpr quint8 kLinkHello = 0x60, kLinkOffer = 0x61, kNameAsk = 0x62, kNameReply = 0x63;
QByteArray sysexText(quint8 kind, const QJsonObject& obj);
QJsonObject sysexJson(const QByteArray& msg);

class Midi : public QObject {
    Q_OBJECT
  public:
    explicit Midi(QObject* parent = nullptr);
    ~Midi() override;

    void toMixxx(const QByteArray& msg);    // out of the ZyDeck Connect port, into Mixxx
    void toCables(const QByteArray& msg);   // Windows: Mixxx's messages relayed to the cables (see kRelayCable)
    void reply(const QString& port, const QByteArray& msg);   // to the device on that cable
    QString problem() const;                // why there's no port ("" when there is)
    QStringList cables() const;             // the MIDI inputs being watched
    void rescan() { watch(); }              // look for cables now (it does every 2 s anyway)

    void fromMixxxRaw(const QByteArray& msg);                // (the port's callback)
    void fromCable(const QString& port, const QByteArray& msg);   // (a cable's callback)

  signals:
    void fromMixxx(const QByteArray& msg);
    void hello(const QString& device, const QString& port);
    void nameAsk(const QString& port, const QJsonObject& ask);

  private:
    void watch();
    // Pacing what goes into Mixxx. Mixxx reads its MIDI input in turns, into a buffer of 1024 events (4 bytes of
    // SysEx each), and drops what doesn't fit ("PortMidi: Buffer overflow"): both devices' subscriptions as they link,
    // or the backlog a Wi-Fi stall lets go all at once. Up to kBurst events go straight out (ordinary playing never
    // gets near it, so it adds no delay); past that the rest waits and goes at kRate a millisecond, a waiting control
    // value replaced by a newer one for the same control (button presses, jog ticks and the rest keep their order)
    void sendNow(const QByteArray& msg);   // (lock held)
    void refill();                         // (lock held)
    void drain();
    QList<QByteArray> m_queue;
    double m_tokens = 0;
    qint64 m_tokensAtNs = -1;
    QElapsedTimer m_clock;
    QTimer m_pace;

    struct Cable;
    mutable QMutex m_mutex;
    std::unique_ptr<RtMidiOut> m_out;
    std::unique_ptr<RtMidiIn> m_in;
    void* m_winPort = nullptr;   // Windows: teVirtualMIDI's port
    QString m_problem;
    QHash<QString, Cable*> m_cables;              // port name -> its input
    QHash<QString, RtMidiOut*> m_replies;         // port name -> output to it
    QHash<QString, qint64> m_devices;             // port name -> when ZyDeck last said hello over it
    QTimer m_watch;
};

} // namespace zc
