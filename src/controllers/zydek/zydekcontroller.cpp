#include "controllers/zydek/zydekcontroller.h"

#include "moc_zydekcontroller.cpp"
#include "util/time.h"
#include "zydek/zydekhub.h"

namespace {

// Mixxx keeps per-device settings under the device name with spaces replaced
// (ControllerManager's sanitizeDeviceName()).
const QString kConfigName = QStringLiteral("Zydek_Tablet");
const QString kMappingFile = QStringLiteral("Zydek-Tablet.midi.xml");

} // namespace

ZydekController::ZydekController(zydek::Hub* pHub)
        : MidiController(QString::fromLatin1(kName)),
          m_pHub(pHub) {
    setInputDevice(true);
    setOutputDevice(true);
}

ZydekController::~ZydekController() {
    if (isOpen()) {
        close();
    }
}

int ZydekController::open(const QString& resourcePath) {
    if (isOpen()) {
        return -1;
    }
    startEngine();
    applyMapping(resourcePath);
    setOpen(true);
    m_pHub->setController(this);
    return 0;
}

int ZydekController::close() {
    if (!isOpen()) {
        return -1;
    }
    m_pHub->setController(nullptr);
    stopEngine();
    MidiController::close();
    setOpen(false);
    return 0;
}

void ZydekController::sendShortMsg(unsigned char status, unsigned char byte1, unsigned char byte2) {
    const char msg[3] = {static_cast<char>(status), static_cast<char>(byte1), static_cast<char>(byte2)};
    m_pHub->fromMixxx(QByteArray(msg, 3));
}

bool ZydekController::sendBytes(const QByteArray& data) {
    m_pHub->fromMixxx(data);
    return true;
}

void ZydekController::injectShortMessage(
        unsigned char status, unsigned char control, unsigned char value) {
    if (isOpen()) {
        receivedShortMessage(status, control, value, mixxx::Time::elapsed());
    }
}

void ZydekController::injectSysex(const QByteArray& data) {
    if (isOpen()) {
        receive(data, mixxx::Time::elapsed());
    }
}

ZydekEnumerator::ZydekEnumerator(UserSettingsPointer pConfig)
        : m_pConfig(pConfig),
          m_pHub(new zydek::Hub(pConfig, this)),
          m_pController(new ZydekController(m_pHub)) {
    // First run: switch the controller on with the Zydek mapping. Afterwards it's an ordinary controller
    // that can be disabled or remapped in Preferences > Controllers.
    if (!m_pConfig->exists(ConfigKey("[Controller]", kConfigName))) {
        m_pConfig->setValue(ConfigKey("[Controller]", kConfigName), 1);
    }
    if (m_pConfig->getValueString(ConfigKey("[ControllerPreset]", kConfigName)).isEmpty()) {
        m_pConfig->setValue(ConfigKey("[ControllerPreset]", kConfigName), kMappingFile);
    }
    m_pHub->start();
}

ZydekEnumerator::~ZydekEnumerator() {
    delete m_pController;
}

QList<Controller*> ZydekEnumerator::queryDevices() {
    return {m_pController};
}
