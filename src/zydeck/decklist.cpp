#include "zydeck/decklist.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "mixer/playerinfo.h"
#include "track/track.h"

namespace zydeck {

namespace {
QJsonObject g_decks;

void write(const QString& path) {
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(g_decks).toJson(QJsonDocument::Compact));
        f.commit();
    }
}
} // namespace

void watchDecks(UserSettingsPointer pConfig) {
    const QString path = pConfig->getSettingsPath() + QStringLiteral("/zydeck-decks.json");
    write(path);   // (nothing loaded yet: what a previous run left is gone)
    QObject::connect(&PlayerInfo::instance(), &PlayerInfo::trackChanged, &PlayerInfo::instance(),
            [path](const QString& group, TrackPointer pNew, TrackPointer) {
                g_decks.insert(group, pNew ? QJsonValue(QJsonObject{
                                                     {"id", pNew->getId().toVariant().toInt()},
                                                     {"title", pNew->getTitle()},
                                                     {"artist", pNew->getArtist()},
                                                     {"seconds", pNew->getDuration()}})
                                           : QJsonValue());
                write(path);
            });
}

} // namespace zydeck
