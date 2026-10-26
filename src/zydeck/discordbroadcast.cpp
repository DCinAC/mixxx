#include "zydeck/discordbroadcast.h"

#ifdef __BROADCAST__   // (Mixxx built with Live Broadcasting: the fork always is)

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>

#include "broadcast/defs_broadcast.h"
#include "recording/defs_recording.h"
#include "preferences/broadcastprofile.h"
#include "preferences/broadcastsettingsmodel.h"

namespace zydeck {

namespace {

const QString kProfile = QStringLiteral("ZyDeck Discord");

void answer(const QString& path, const QJsonObject& done) {
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(done).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

} // namespace

void watchDiscordBroadcast(BroadcastSettingsPointer pBroadcast, UserSettingsPointer pConfig) {
    if (!pBroadcast) {
        return;
    }
    const QString handover = pConfig->getSettingsPath() + QStringLiteral("/zydeck-broadcast.json");
    const QString done = pConfig->getSettingsPath() + QStringLiteral("/zydeck-broadcast-done.json");
    auto* pTimer = new QTimer(QCoreApplication::instance());
    pTimer->setInterval(1000);
    QObject::connect(pTimer, &QTimer::timeout, pTimer, [pBroadcast, handover, done] {
        QFile f(handover);
        if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
            return;
        }
        const QJsonObject given = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        f.remove();   // (the key isn't left lying in a file: from here it's in the keychain)
        const QString host = given.value(QStringLiteral("host")).toString().trimmed();
        const int port = given.value(QStringLiteral("port")).toInt();
        const QString key = given.value(QStringLiteral("key")).toString();
        if (host.isEmpty() || port <= 0 || port > 65535) {
            answer(done, {{"ok", false}, {"error", "No address for the bot"}});
            return;
        }
        // as Mixxx's own preferences change them: a model of the profiles, changed, applied (that saves them)
        BroadcastSettingsModel model;
        model.resetFromSettings(pBroadcast);
        BroadcastProfilePtr pProfile = model.getProfileByName(kProfile);
        if (!pProfile) {
            pProfile = BroadcastProfilePtr(new BroadcastProfile(kProfile));
            model.addProfileToModel(pProfile);
        }
        pProfile->setHost(host);
        pProfile->setPort(port);
        pProfile->setServertype(QStringLiteral(BROADCAST_SERVER_ICECAST2));
        pProfile->setMountPoint(QStringLiteral("/zydeck"));
        pProfile->setLogin(QStringLiteral("source"));
        pProfile->setSecureCredentialStorage(true);   // (the key in the keychain, not in the profile's file)
        if (!key.isEmpty()) {
            pProfile->setPassword(key);
        }
        pProfile->setFormat(QStringLiteral(ENCODING_OPUS));   // 48 kHz, 20 ms packets: as Discord takes them
        pProfile->setBitrate(128);
        pProfile->setChannels(2);
        pProfile->setStreamName(QStringLiteral("ZyDeck"));
        pProfile->setStreamPublic(false);
        pProfile->setEnableMetadata(false);
        pProfile->setOggDynamicUpdate(false);   // one Ogg stream for the whole set
        pProfile->setEnableReconnect(true);     // the bot restarting, Wi-Fi dropping: keep trying
        pProfile->setNoDelayFirstReconnect(true);
        pProfile->setReconnectPeriod(5);
        pProfile->setLimitReconnects(false);
        pProfile->setEnabled(true);
        pBroadcast->applyModel(&model);
        answer(done, {{"ok", true}, {"profile", kProfile}, {"host", host}, {"port", port}, {"key", !key.isEmpty()}});
    });
    pTimer->start();
}

} // namespace zydeck

#endif // __BROADCAST__
