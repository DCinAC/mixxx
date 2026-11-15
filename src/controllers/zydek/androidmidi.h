#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QTimer>
#include <QVector>

#include "controllers/controllerenumerator.h"
#include "controllers/midi/midicontroller.h"
#include "preferences/usersettings.h"

/// A MIDI controller plugged into this phone or tablet (a DJ controller on a USB OTG adapter), through Android's MIDI
/// service (packaging/android/src/org/mixxx/ZydekMidiHost.java): what PortMidi's controllers are on a computer, so a
/// computer's mapping runs on it unchanged.
///
/// It also times its own path for the session profiler: Android's MIDI service -> this app -> Mixxx's thread -> the
/// mapping's script (takeStats()).
class AndroidMidiController : public MidiController {
    Q_OBJECT
  public:
    struct Info {
        QString name, vendor, product, serial;
        int vendorId = -1, productId = -1;
    };

    AndroidMidiController(int deviceId, const Info& info);
    ~AndroidMidiController() override;

    PhysicalTransportProtocol getPhysicalTransportProtocol() const override {
        return PhysicalTransportProtocol::USB;
    }
    QString getVendorString() const override {
        return m_info.vendor;
    }
    QString getProductString() const override {
        return m_info.product;
    }
    std::optional<uint16_t> getVendorId() const override {
        return m_info.vendorId >= 0 ? std::optional<uint16_t>(static_cast<uint16_t>(m_info.vendorId)) : std::nullopt;
    }
    std::optional<uint16_t> getProductId() const override {
        return m_info.productId >= 0 ? std::optional<uint16_t>(static_cast<uint16_t>(m_info.productId)) : std::nullopt;
    }
    QString getSerialNumber() const override {
        return m_info.serial;
    }
    std::optional<uint8_t> getUsbInterfaceNumber() const override {
        return std::nullopt;
    }

    const Info& info() const {
        return m_info;
    }
    /// Android's id for it while plugged in (a new one each time), -1 unplugged
    int deviceId() const {
        return m_deviceId;
    }
    void setDeviceId(int id) {
        m_deviceId = id;
    }

    /// Bytes from the controller, on Mixxx's controller thread: midiNs when Android's MIDI service had them, appNs
    /// when they reached this app (CLOCK_MONOTONIC, ns)
    void deliver(const QByteArray& bytes, qint64 midiNs, qint64 appNs);

    /// The profiler: the last second, then starts over. Empty when nothing came or went.
    QJsonObject takeStats();

  protected:
    void sendShortMsg(unsigned char status, unsigned char byte1, unsigned char byte2) override;

  private:
    int open(const QString& resourcePath) override;
    int close() override;
    bool sendBytes(const QByteArray& data) override;
    void handle(unsigned char status, unsigned char d1, unsigned char d2);

    Info m_info;
    int m_deviceId;
    // the MIDI byte stream (running status, SysEx split over packets)
    unsigned char m_status = 0;
    unsigned char m_data[2] = {0, 0};
    int m_have = 0;
    bool m_inSysex = false;
    QByteArray m_sysex;

    // this second's timings, ms
    QMutex m_statsLock;
    int m_n = 0, m_cc = 0, m_notes = 0, m_sysexN = 0;
    QVector<float> m_svc, m_toThread, m_script, m_total;
    QHash<int, qint64> m_lastAt;            // a message kind (status, number) -> when it last came
    QHash<int, QVector<float>> m_gaps;      // its gaps, ms
    int m_outN = 0;
};

/// The controllers Android's MIDI service offers. Each keeps its object once seen (Mixxx's preferences and the
/// controller manager hold on to them), and only those plugged in are listed. Plugging one in or out says
/// devicesChanged: the controller manager sets the controllers up again then.
class AndroidMidiEnumerator : public ControllerEnumerator {
    Q_OBJECT
  public:
    explicit AndroidMidiEnumerator(UserSettingsPointer pConfig);
    ~AndroidMidiEnumerator() override;

    /// The one there is (Android), or nullptr
    static AndroidMidiEnumerator* instance();

    QList<Controller*> queryDevices() override;

    /// Those plugged in, for ZyDeck's pages: [{name, vendor, product, vendorId, productId, on, mapping}]
    QJsonArray plugged() const;
    /// The profiler's second: each plugged-in controller's takeStats() that has something
    QJsonArray takeStats();
    /// The mapping files that could be this controller's, the best first (the user's own before Mixxx's)
    QStringList mappingsFor(const QString& name) const;
    /// Use a controller with a mapping (empty: the best of mappingsFor), or stop using it
    bool use(const QString& name, const QString& mapping);
    void stopUsing(const QString& name);

    // JNI (ZydekMidiHost.java), any thread
    void jniAdded(int id, const AndroidMidiController::Info& info);
    void jniRemoved(int id);
    void jniReceive(int id, const QByteArray& bytes, qint64 midiNs, qint64 appNs);

  signals:
    void devicesChanged();

  private:
    AndroidMidiController* byId(int id) const;
    AndroidMidiController* byName(const QString& name) const;
    void changed();

    UserSettingsPointer m_pConfig;
    QList<AndroidMidiController*> m_all;   // (never deleted while the app runs)
    QTimer m_settle;                       // plugging in can bring a few at once: set up once
};
