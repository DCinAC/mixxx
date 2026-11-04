#include "zydeck/decklist.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>

#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"

namespace zydeck {

namespace {
QJsonObject g_decks;
QHash<QString, QMetaObject::Connection> g_cueWatch;   // group -> its track's cues, watched
QHash<QString, QMetaObject::Connection> g_gridWatch;   // and its beat grid
QHash<QString, QTimer*> g_gridSave;   // (saving it once the edits stop)
QHash<QString, QString> g_grid;   // group -> its grid's signature, as last saved
TrackCollectionManager* g_tracks = nullptr;

QString gridOf(const TrackPointer& pTrack) {
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    return pBeats ? QString::number(qHash(pBeats->toByteArray()), 16) : QString();
}

// what's on a deck: the track, and its hot cues' names (1 to 8; "" none), which a controller can't read itself
QJsonObject entry(const QString& group, const TrackPointer& pTrack) {
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
            {"cues", labels},
            {"grid", g_grid.value(group)}};
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
                QObject::disconnect(g_gridWatch.take(group));
                QTimer*& pSave = g_gridSave[group];
                if (!pSave) {
                    pSave = new QTimer(&PlayerInfo::instance());
                    pSave->setSingleShot(true);
                    pSave->setInterval(1500);
                    QObject::connect(pSave, &QTimer::timeout, &PlayerInfo::instance(), [path, group] {
                        const TrackPointer pNow = PlayerInfo::instance().getTrackInfo(group);
                        if (!pNow) {
                            return;
                        }
                        if (g_tracks) {   // (first: Connect reads the grid from the library as it sees the entry change)
                            g_tracks->saveTrack(pNow);
                        }
                        g_grid.insert(group, gridOf(pNow));
                        const QJsonObject now = entry(group, pNow);
                        if (g_decks.value(group) != now) {
                            g_decks.insert(group, now);
                            write(path);
                        }
                    });
                }
                pSave->stop();
                g_grid.insert(group, pNew ? gridOf(pNew) : QString());
                g_decks.insert(group, pNew ? QJsonValue(entry(group, pNew)) : QJsonValue());
                write(path);
                if (pNew) {   // (a hot cue named, set or cleared while it's loaded)
                    Track* const pTrack = pNew.get();
                    g_gridWatch.insert(group, QObject::connect(pTrack, &Track::beatsUpdated, pSave, qOverload<>(&QTimer::start)));
                    g_cueWatch.insert(group, QObject::connect(pTrack, &Track::cuesUpdated, &PlayerInfo::instance(), [path, group, pTrack] {
                        const TrackPointer pNow = PlayerInfo::instance().getTrackInfo(group);
                        if (pNow.get() != pTrack) {
                            return;
                        }
                        const QJsonObject now = entry(group, pNow);
                        if (g_decks.value(group) != now) {   // (only a hot cue's name: moving cues changes nothing here)
                            g_decks.insert(group, now);
                            write(path);
                        }
                    }));
                }
            });
}

void saveGridsTo(TrackCollectionManager* pTracks) {
    g_tracks = pTracks;
}

} // namespace zydeck
