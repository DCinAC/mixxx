#include "zydeck/loadrequest.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

#include "control/controlobject.h"
#include "mixer/playermanager.h"
#include "preferences/dialog/dlgprefdeck.h"

namespace zydeck {

namespace {

void answer(const QString& path, const QJsonObject& done) {
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(done).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

} // namespace

void watchLoadRequests(const std::shared_ptr<PlayerManager>& pPlayerManager, UserSettingsPointer pConfig) {
    const QString request = pConfig->getSettingsPath() + QStringLiteral("/zydeck-load.json");
    const QString done = pConfig->getSettingsPath() + QStringLiteral("/zydeck-load-done.json");
    QFile::remove(request);   // (one a previous run left: not now)
    const std::weak_ptr<PlayerManager> pWeak = pPlayerManager;
    auto* pTimer = new QTimer(QCoreApplication::instance());   // (a quarter of a second: a person's waiting for it)
    pTimer->setInterval(250);
    const QString quit = pConfig->getSettingsPath() + QStringLiteral("/zydeck-quit.json");
    QFile::remove(quit);
    QObject::connect(pTimer, &QTimer::timeout, pTimer, [pWeak, pConfig, request, done, quit] {
        if (QFile::exists(quit)) {   // Mixxx closes as its window would (it asks if a deck is playing), and so saves
            QFile::remove(quit);     // its settings: macOS's own quit (AppleScript's) ends it before it does
            QMetaObject::invokeMethod(qApp, &QApplication::closeAllWindows, Qt::QueuedConnection);
        }
        QFile f(request);
        if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
            return;
        }
        const QJsonObject ask = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        f.remove();
        const QString id = ask.value(QStringLiteral("id")).toString();
        const QString group = ask.value(QStringLiteral("group")).toString();
        const QString path = ask.value(QStringLiteral("path")).toString();
        const auto fail = [&](const QString& why) { answer(done, {{"id", id}, {"ok", false}, {"error", why}}); };
        static const QRegularExpression player(QStringLiteral(R"(^\[(Channel|Sampler|PreviewDeck)\d+\]$)"));
        const bool preview = group.startsWith(QLatin1String("[PreviewDeck"));
        if (preview && ask.value(QStringLiteral("stop")).toBool()) {   // the preview stops
            ControlObject::set(ConfigKey(group, QStringLiteral("play")), 0);
            return answer(done, {{"id", id}, {"ok", true}});
        }
        const std::shared_ptr<PlayerManager> pPlayers = pWeak.lock();
        if (!pPlayers || !player.match(group).hasMatch() || !ControlObject::exists(ConfigKey(group, QStringLiteral("play")))) {
            return fail(QStringLiteral("No such deck"));
        }
        if (!QFileInfo(path).isFile()) {
            return fail(QStringLiteral("The file isn't there"));
        }
        if (!preview && ControlObject::get(ConfigKey(group, QStringLiteral("play"))) > 0 &&
                !pConfig->getValue<bool>(kConfigKeyAllowTrackLoadToPlayingDeck)) {
            return fail(QStringLiteral("That deck is playing"));   // (as Mixxx's own loading: not into a playing deck)
        }
        pPlayers->slotLoadLocationToPlayer(path, group, preview);   // (a preview plays at once, in the headphones)
        answer(done, {{"id", id}, {"ok", true}});
    });
    pTimer->start();
}

} // namespace zydeck
