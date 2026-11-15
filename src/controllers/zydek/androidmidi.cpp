#include "controllers/zydek/androidmidi.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJniEnvironment>
#include <QJniObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <ctime>

#include "controllers/defs_controllers.h"
#include "moc_androidmidi.cpp"
#include "util/time.h"

namespace {

AndroidMidiEnumerator* s_pEnumerator = nullptr;
QMutex s_enumeratorLock;   // (JNI calls come from Android's threads)

qint64 nowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);   // (Java's System.nanoTime())
    return static_cast<qint64>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

constexpr int kKeepPerSecond = 2000;   // timings kept a second (a jog sends ~500)

void keep(QVector<float>& v, qint64 ns) {
    if (v.size() < kKeepPerSecond) {
        v.append(static_cast<float>(static_cast<double>(ns) / 1e6));
    }
}

/// [least, median, 95 %, most], ms to 0.01, as the page's latStats
QJsonValue quantiles(QVector<float> v) {
    if (v.isEmpty()) {
        return QJsonValue();
    }
    std::sort(v.begin(), v.end());
    const auto at = [&v](double p) {
        const float x = v[std::min<qsizetype>(v.size() - 1, static_cast<qsizetype>(p * static_cast<double>(v.size())))];
        return std::round(x * 100.0) / 100.0;
    };
    return QJsonArray{at(0), at(.5), at(.95), at(1)};
}

/// Every k-th one, at most n: the report's whole-session percentiles
QJsonArray sample(const QVector<float>& v, int n) {
    QJsonArray out;
    const qsizetype step = std::max<qsizetype>(1, (v.size() + n - 1) / n);
    for (qsizetype i = 0; i < v.size(); i += step) {
        out.append(std::round(v[i] * 100.0) / 100.0);
    }
    return out;
}

QString sanitized(QString name) {   // (as ControllerManager keeps its settings)
    return name.replace(" ", "_").replace("/", "_").replace("\\", "_");
}

/// "Hercules DJControl Inpulse 300mk2 - 4 deck" -> {hercules, djcontrol, inpulse, 300, mk2, 4, deck}
QSet<QString> words(const QString& s) {
    static const QRegularExpression split(QStringLiteral("[^a-z0-9]+|(?<=[a-z])(?=[0-9])|(?<=[0-9])(?=[a-z])"));
    QSet<QString> out;
    for (const QString& w : s.toLower().split(split, Qt::SkipEmptyParts)) {
        out.insert(w);
    }
    return out;
}

} // namespace

// ---- the controller ---------------------------------------------------------------------------------

AndroidMidiController::AndroidMidiController(int deviceId, const Info& info)
        : MidiController(info.name),
          m_info(info),
          m_deviceId(deviceId) {
    setInputDevice(true);
    setOutputDevice(true);
}

AndroidMidiController::~AndroidMidiController() {
    if (isOpen()) {
        close();
    }
}

int AndroidMidiController::open(const QString& resourcePath) {
    if (isOpen()) {
        return -1;
    }
    m_status = 0;
    m_have = 0;
    m_inSysex = false;
    startEngine();
    applyMapping(resourcePath);
    setOpen(true);
    return 0;
}

int AndroidMidiController::close() {
    if (!isOpen()) {
        return -1;
    }
    stopEngine();
    MidiController::close();
    setOpen(false);
    return 0;
}

void AndroidMidiController::sendShortMsg(unsigned char status, unsigned char byte1, unsigned char byte2) {
    const int size = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
    const char msg[3] = {static_cast<char>(status), static_cast<char>(byte1), static_cast<char>(byte2)};
    sendBytes(QByteArray(msg, size));
}

bool AndroidMidiController::sendBytes(const QByteArray& data) {
    if (m_deviceId < 0 || data.isEmpty()) {
        return false;
    }
    {
        QMutexLocker lock(&m_statsLock);
        ++m_outN;
    }
    QJniEnvironment env;
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(data.size()));
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(data.size()), reinterpret_cast<const jbyte*>(data.constData()));
    QJniObject::callStaticMethod<void>("org/mixxx/ZydekMidiHost", "send", "(I[B)V", static_cast<jint>(m_deviceId), bytes);
    env->DeleteLocalRef(bytes);
    return true;
}

void AndroidMidiController::deliver(const QByteArray& bytes, qint64 midiNs, qint64 appNs) {
    const qint64 inThread = nowNs();
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        if (b >= 0xF8) {   // (real time: between any bytes; Mixxx ignores the clock)
            handle(b, 0, 0);
            continue;
        }
        if (m_inSysex) {
            if (b == 0xF7 || b < 0x80) {
                m_sysex.append(static_cast<char>(b));
                if (b == 0xF7) {
                    m_inSysex = false;
                    if (isOpen()) {
                        receive(m_sysex, mixxx::Time::elapsed());
                    }
                    QMutexLocker lock(&m_statsLock);
                    ++m_sysexN;
                }
                continue;
            }
            m_inSysex = false;   // (cut short by a new message)
        }
        if (b == 0xF0) {
            m_inSysex = true;
            m_sysex = QByteArray(1, static_cast<char>(b));
            continue;
        }
        if (b & 0x80) {
            m_status = b;
            m_have = 0;
            if (b == 0xF6 || b == 0xF7 || b == 0xF4 || b == 0xF5) {   // (no data bytes)
                handle(b, 0, 0);
                m_status = 0;
            }
            continue;
        }
        if (!m_status) {
            continue;   // (data without a status: the start of a stream cut in two)
        }
        m_data[m_have++] = b;
        const unsigned char kind = m_status & 0xF0;
        const int needs = kind == 0xC0 || kind == 0xD0 || m_status == 0xF1 || m_status == 0xF3 ? 1 : 2;
        if (m_have == needs) {
            handle(m_status, m_data[0], needs > 1 ? m_data[1] : 0);
            m_have = 0;
            if (m_status >= 0xF0) {
                m_status = 0;   // (system messages have no running status)
            }
        }
    }
    const qint64 done = nowNs();
    QMutexLocker lock(&m_statsLock);
    if (midiNs > 0 && appNs >= midiNs) {
        keep(m_svc, appNs - midiNs);
        keep(m_total, done - midiNs);
    }
    keep(m_toThread, inThread - appNs);
    keep(m_script, done - inThread);
}

void AndroidMidiController::handle(unsigned char status, unsigned char d1, unsigned char d2) {
    if (isOpen()) {
        receivedShortMessage(status, d1, d2, mixxx::Time::elapsed());
    }
    const qint64 now = nowNs();
    const int key = status < 0xF0 ? (status << 8) | d1 : status << 8;
    QMutexLocker lock(&m_statsLock);
    ++m_n;
    if ((status & 0xF0) == 0xB0) {
        ++m_cc;
    } else if ((status & 0xF0) == 0x90 || (status & 0xF0) == 0x80) {
        ++m_notes;
    }
    const auto last = m_lastAt.constFind(key);
    if (last != m_lastAt.constEnd() && now - last.value() < 1000000000) {
        keep(m_gaps[key], now - last.value());
    }
    m_lastAt.insert(key, now);
}

QJsonObject AndroidMidiController::takeStats() {
    QMutexLocker lock(&m_statsLock);
    const qint64 now = nowNs();
    long long sends[3] = {0, 0, 0};
    if (m_deviceId >= 0) {
        const QJniObject out = QJniObject::callStaticObjectMethod("org/mixxx/ZydekMidiHost", "takeSendStats", "(I)[J",
                static_cast<jint>(m_deviceId));
        if (out.isValid()) {
            QJniEnvironment env;
            const auto arr = static_cast<jlongArray>(out.object());
            if (env->GetArrayLength(arr) == 3) {
                env->GetLongArrayRegion(arr, 0, 3, reinterpret_cast<jlong*>(sends));
            }
        }
    }
    QJsonObject o;
    if (m_n || m_sysexN || m_outN || sends[0]) {
        // the busiest kind of message (turning a jog: its moves) and how evenly they came
        int busiest = -1;
        for (auto it = m_gaps.constBegin(); it != m_gaps.constEnd(); ++it) {
            if (busiest < 0 || it.value().size() > m_gaps.value(busiest).size()) {
                busiest = it.key();
            }
        }
        o = QJsonObject{{"name", m_info.name}, {"open", isOpen()}, {"n", m_n}, {"cc", m_cc}, {"notes", m_notes},
                {"sysex", m_sysexN}, {"svc", quantiles(m_svc)}, {"toThread", quantiles(m_toThread)},
                {"script", quantiles(m_script)}, {"total", quantiles(m_total)}, {"totalAll", sample(m_total, 100)},
                {"out", m_outN}, {"sent", static_cast<double>(sends[0])},
                {"sendMs", sends[0] ? std::round(static_cast<double>(sends[1]) / static_cast<double>(sends[0]) / 1e4) / 100 : 0.0},
                {"sendMax", std::round(static_cast<double>(sends[2]) / 1e4) / 100}};
        if (busiest >= 0) {
            o.insert(QStringLiteral("key"), QString::asprintf("%02X %02X", busiest >> 8, busiest & 0xFF));
            o.insert(QStringLiteral("keyN"), m_gaps.value(busiest).size() + 1);
            o.insert(QStringLiteral("gap"), quantiles(m_gaps.value(busiest)));
        }
    }
    m_n = m_cc = m_notes = m_sysexN = m_outN = 0;
    m_svc.clear();
    m_toThread.clear();
    m_script.clear();
    m_total.clear();
    m_gaps.clear();
    for (auto it = m_lastAt.begin(); it != m_lastAt.end();) {   // (keep the recent ones: a gap can span seconds)
        it = now - it.value() > 1000000000 ? m_lastAt.erase(it) : std::next(it);
    }
    return o;
}

// ---- JNI --------------------------------------------------------------------------------------------

namespace {

QString jstr(JNIEnv* env, jstring s) {
    if (!s) {
        return QString();
    }
    const char* c = env->GetStringUTFChars(s, nullptr);
    QString out = QString::fromUtf8(c);
    env->ReleaseStringUTFChars(s, c);
    return out;
}

void JNICALL jniAdded(JNIEnv* env, jclass, jint id, jstring name, jstring vendor, jstring product, jint vid, jint pid,
        jstring serial) {
    AndroidMidiController::Info info{jstr(env, name), jstr(env, vendor), jstr(env, product), jstr(env, serial), vid, pid};
    QMutexLocker lock(&s_enumeratorLock);
    if (s_pEnumerator) {
        s_pEnumerator->jniAdded(id, info);
    }
}

void JNICALL jniRemoved(JNIEnv*, jclass, jint id) {
    QMutexLocker lock(&s_enumeratorLock);
    if (s_pEnumerator) {
        s_pEnumerator->jniRemoved(id);
    }
}

void JNICALL jniReceive(JNIEnv* env, jclass, jint id, jbyteArray data, jlong midiNs, jlong appNs) {
    const jsize n = env->GetArrayLength(data);
    QByteArray bytes(n, Qt::Uninitialized);
    env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
    QMutexLocker lock(&s_enumeratorLock);
    if (s_pEnumerator) {
        s_pEnumerator->jniReceive(id, bytes, midiNs, appNs);
    }
}

} // namespace

// ---- the enumerator ---------------------------------------------------------------------------------

AndroidMidiEnumerator::AndroidMidiEnumerator(UserSettingsPointer pConfig)
        : m_pConfig(pConfig) {
    m_settle.setSingleShot(true);
    m_settle.setInterval(300);
    connect(&m_settle, &QTimer::timeout, this, &AndroidMidiEnumerator::devicesChanged);
    {
        QMutexLocker lock(&s_enumeratorLock);
        s_pEnumerator = this;
    }
    const JNINativeMethod methods[] = {
            {"nativeAdded", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;IILjava/lang/String;)V",
                    reinterpret_cast<void*>(jniAdded)},
            {"nativeRemoved", "(I)V", reinterpret_cast<void*>(jniRemoved)},
            {"nativeReceive", "(I[BJJ)V", reinterpret_cast<void*>(jniReceive)}};
    QJniEnvironment env;
    if (!env.registerNativeMethods("org/mixxx/ZydekMidiHost", methods, 3)) {
        qWarning() << "AndroidMidiEnumerator: no MIDI controllers (couldn't reach ZydekMidiHost)";
        return;
    }
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    QJniObject::callStaticMethod<void>("org/mixxx/ZydekMidiHost", "start", "(Landroid/content/Context;)V",
            context.object<jobject>());
}

AndroidMidiEnumerator::~AndroidMidiEnumerator() {
    {
        QMutexLocker lock(&s_enumeratorLock);
        s_pEnumerator = nullptr;
    }
    QJniObject::callStaticMethod<void>("org/mixxx/ZydekMidiHost", "stop", "()V");
    qDeleteAll(m_all);
}

AndroidMidiEnumerator* AndroidMidiEnumerator::instance() {
    QMutexLocker lock(&s_enumeratorLock);
    return s_pEnumerator;
}

QList<Controller*> AndroidMidiEnumerator::queryDevices() {
    QList<Controller*> out;
    for (AndroidMidiController* p : std::as_const(m_all)) {
        if (p->deviceId() >= 0) {
            out.append(p);
        }
    }
    return out;
}

AndroidMidiController* AndroidMidiEnumerator::byId(int id) const {
    for (AndroidMidiController* p : m_all) {
        if (p->deviceId() == id) {
            return p;
        }
    }
    return nullptr;
}

AndroidMidiController* AndroidMidiEnumerator::byName(const QString& name) const {
    for (AndroidMidiController* p : m_all) {
        if (p->getName() == name) {
            return p;
        }
    }
    return nullptr;
}

void AndroidMidiEnumerator::changed() {
    m_settle.start();
}

void AndroidMidiEnumerator::jniAdded(int id, const AndroidMidiController::Info& info) {
    QMetaObject::invokeMethod(this, [this, id, info] {
        AndroidMidiController::Info named = info;
        if (named.name.isEmpty()) {
            named.name = QStringLiteral("MIDI controller");
        }
        if (AndroidMidiController* p = byName(named.name); p && p->deviceId() < 0) {
            p->setDeviceId(id);   // (plugged in again)
        } else {
            for (int n = 2; byName(named.name); ++n) {   // (two of the same: Mixxx keeps settings by name)
                named.name = QStringLiteral("%1 (%2)").arg(info.name).arg(n);
            }
            auto* pController = new AndroidMidiController(id, named);
            pController->moveToThread(thread());
            m_all.append(pController);
        }
        qInfo() << "AndroidMidiEnumerator: plugged in" << named.name;
        changed();
    }, Qt::QueuedConnection);
}

void AndroidMidiEnumerator::jniRemoved(int id) {
    QMetaObject::invokeMethod(this, [this, id] {
        if (AndroidMidiController* p = byId(id)) {
            qInfo() << "AndroidMidiEnumerator: unplugged" << p->getName();
            if (p->isOpen()) {
                p->close();
            }
            p->setDeviceId(-1);
            changed();
        }
    }, Qt::QueuedConnection);
}

void AndroidMidiEnumerator::jniReceive(int id, const QByteArray& bytes, qint64 midiNs, qint64 appNs) {
    QMetaObject::invokeMethod(this, [this, id, bytes, midiNs, appNs] {
        if (AndroidMidiController* p = byId(id)) {
            p->deliver(bytes, midiNs, appNs);
        }
    }, Qt::QueuedConnection);
}

QJsonArray AndroidMidiEnumerator::plugged() const {
    QJsonArray out;
    for (AndroidMidiController* p : m_all) {
        if (p->deviceId() < 0) {
            continue;
        }
        const QString key = sanitized(p->getName());
        const auto& i = p->info();
        out.append(QJsonObject{{"name", p->getName()}, {"vendor", i.vendor}, {"product", i.product},
                {"vendorId", i.vendorId}, {"productId", i.productId},
                {"on", m_pConfig->getValue(ConfigKey(QStringLiteral("[Controller]"), key), 0) != 0},
                {"open", p->isOpen()},
                {"mapping", QFileInfo(m_pConfig->getValueString(ConfigKey(QStringLiteral("[ControllerPreset]"), key))).completeBaseName()}});
    }
    return out;
}

QJsonArray AndroidMidiEnumerator::takeStats() {
    QJsonArray out;
    for (AndroidMidiController* p : std::as_const(m_all)) {
        const QJsonObject o = p->takeStats();
        if (!o.isEmpty()) {
            out.append(o);
        }
    }
    return out;
}

QStringList AndroidMidiEnumerator::mappingsFor(const QString& name) const {
    const QSet<QString> want = words(name);
    QList<std::pair<double, QString>> scored;
    const QStringList dirs{userMappingsPath(m_pConfig), resourceMappingsPath(m_pConfig)};
    for (int d = 0; d < dirs.size(); ++d) {
        for (const QFileInfo& f : QDir(dirs[d]).entryInfoList({QStringLiteral("*") + MIDI_MAPPING_EXTENSION}, QDir::Files)) {
            const QString base = f.fileName().chopped(static_cast<int>(strlen(MIDI_MAPPING_EXTENSION)));
            const QSet<QString> have = words(base);
            int shared = 0;
            for (const QString& w : want) {
                shared += have.contains(w) ? 1 : 0;
            }
            if (shared < std::max(2, static_cast<int>(want.size()) / 2)) {
                continue;
            }
            // most of the controller's words, then the user's own folder, then fewer extra words
            scored.append({shared * 10.0 + (d == 0 ? 5 : 0) - 0.1 * (have.size() - shared), f.absoluteFilePath()});
        }
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    QStringList out;
    for (const auto& s : scored) {
        out.append(s.second);
    }
    return out;
}

bool AndroidMidiEnumerator::use(const QString& name, const QString& mapping) {
    AndroidMidiController* p = byName(name);
    if (!p) {
        return false;
    }
    QString file = mapping;
    if (file.isEmpty()) {
        const QStringList found = mappingsFor(name);
        if (found.isEmpty()) {
            return false;
        }
        file = found.first();
    }
    const QString key = sanitized(name);
    m_pConfig->setValue(ConfigKey(QStringLiteral("[ControllerPreset]"), key), file);
    m_pConfig->setValue(ConfigKey(QStringLiteral("[Controller]"), key), 1);
    changed();
    return true;
}

void AndroidMidiEnumerator::stopUsing(const QString& name) {
    m_pConfig->setValue(ConfigKey(QStringLiteral("[Controller]"), sanitized(name)), 0);
    if (AndroidMidiController* p = byName(name); p && p->isOpen()) {
        p->close();
    }
    changed();
}
