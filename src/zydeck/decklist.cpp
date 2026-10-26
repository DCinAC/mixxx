#include "zydeck/decklist.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "mixer/playerinfo.h"
#include "track/cue.h"
#include "track/track.h"

namespace zydeck {

namespace {
QJsonObject g_decks;
QHash<QString, QMetaObject::Connection> g_cueWatch;   // group -> its track's cues, watched

// what's on a deck: the track, and its hot cues' names (1 to 8; "" none), which a controller can't read itself
QJsonObject entry(const TrackPointer& pTrack) {
    QJsonArray labels;
    for (int i = 0; i < 8; ++i) {
        labels.append(QString());
    }
    for (const CuePointer& pCue : pTrack->getCuePoints()) {
        const int n = pCue->getHotCue();
        if (pCue->getType() == mixxx::CueType::HotCue && n >= 0 && n < 8) {
            labels[n] = pCue->getLabel();
        }
    }
    return {{"id", pTrack->getId().toVariant().toInt()},
            {"title", pTrack->getTitle()},
            {"artist", pTrack->getArtist()},
            {"seconds", pTrack->getDuration()},
            {"cues", labels}};
}

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
                QObject::disconnect(g_cueWatch.take(group));
                g_decks.insert(group, pNew ? QJsonValue(entry(pNew)) : QJsonValue());
                write(path);
                if (pNew) {   // (a hot cue named, set or cleared while it's loaded)
                    Track* const pTrack = pNew.get();
                    g_cueWatch.insert(group, QObject::connect(pTrack, &Track::cuesUpdated, &PlayerInfo::instance(), [path, group, pTrack] {
                        const TrackPointer pNow = PlayerInfo::instance().getTrackInfo(group);
                        if (pNow.get() != pTrack) {
                            return;
                        }
                        const QJsonObject now = entry(pNow);
                        if (g_decks.value(group) != now) {   // (only a hot cue's name: moving cues changes nothing here)
                            g_decks.insert(group, now);
                            write(path);
                        }
                    }));
                }
            });
}

} // namespace zydeck
