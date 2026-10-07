#pragma once

#include <QByteArray>
#include <QList>

#include "controllers/controllerenumerator.h"
#include "controllers/midi/midicontroller.h"
#include "preferences/usersettings.h"

namespace zydek {
class Hub;
}

/// The Zydek tablet controller: a MIDI controller whose "device" is the tablet (or phone) page
/// connected over WiFi, through zydek::Hub's WebSocket server.
///
/// It runs the same mapping as the desktop setup (res/controllers/Zydek-Tablet.*), so pads,
/// scratching, tempo and all the feedback work exactly as with the Tablet Pads MIDI bridge:
/// the hub turns page messages into the MIDI/SysEx the mapping expects and back.
class ZydekController : public MidiController {
    Q_OBJECT
  public:
    static constexpr const char* kName = "Zydek Tablet";

    explicit ZydekController(zydek::Hub* pHub);
    ~ZydekController() override;

    PhysicalTransportProtocol getPhysicalTransportProtocol() const override {
        return PhysicalTransportProtocol::UNKNOWN;
    }
    QString getVendorString() const override {
        return QStringLiteral("Zydek");
    }
    QString getProductString() const override {
        return QStringLiteral("Tablet controller (WiFi)");
    }
    std::optional<uint16_t> getVendorId() const override {
        return std::nullopt;
    }
    std::optional<uint16_t> getProductId() const override {
        return std::nullopt;
    }
    QString getSerialNumber() const override {
        return QString();
    }
    std::optional<uint8_t> getUsbInterfaceNumber() const override {
        return std::nullopt;
    }

    /// From the page (via the hub): a MIDI short message or a SysEx message for the mapping.
    void injectShortMessage(unsigned char status, unsigned char control, unsigned char value);
    void injectSysex(const QByteArray& data);

  protected:
    void sendShortMsg(unsigned char status, unsigned char byte1, unsigned char byte2) override;

  private:
    int open(const QString& resourcePath) override;
    int close() override;
    bool sendBytes(const QByteArray& data) override;

    zydek::Hub* m_pHub;
};

/// Provides the one ZydekController and starts the hub (web server) next to it. On first run it also
/// enables the controller with the Zydek mapping, so a fresh install works without any setup.
class ZydekEnumerator : public ControllerEnumerator {
    Q_OBJECT
  public:
    explicit ZydekEnumerator(UserSettingsPointer pConfig);
    ~ZydekEnumerator() override;

    QList<Controller*> queryDevices() override;

  private:
    UserSettingsPointer m_pConfig;
    zydek::Hub* m_pHub;
    ZydekController* m_pController;
};
