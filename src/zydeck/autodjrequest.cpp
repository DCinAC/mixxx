#include "zydeck/autodjrequest.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "library/autodj/autodjprocessor.h"
#include "library/dao/playlistdao.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/trackset/crate/crateid.h"

namespace zydeck {

namespace {

const QString kPrefs = QStringLiteral("[Auto DJ]");   // Mixxx's Auto DJ preferences
// Mixxx's TransitionMode, in its order
const QStringList kModes = {QStringLiteral("intro-outro"),
        QStringLiteral("outro-start"),
        QStringLiteral("full-track"),
        QStringLiteral("skip-silence"),
        QStringLiteral("skip-silence-full")};

QString errorText(AutoDJProcessor::AutoDJError e) {
    switch (e) {
    case AutoDJProcessor::ADJ_OK:
        return {};
    case AutoDJProcessor::ADJ_IS_INACTIVE:
        return QStringLiteral("Auto DJ is off");
    case AutoDJProcessor::ADJ_QUEUE_EMPTY:
        return QStringLiteral("The Auto DJ queue is empty: add tracks, or a source crate");
    case AutoDJProcessor::ADJ_BOTH_DECKS_PLAYING:
        return QStringLiteral("Decks 1 and 2 are both playing: stop one first");
    case AutoDJProcessor::ADJ_UNUSED_DECK_PLAYING:
        return QStringLiteral("Deck 3 or 4 is playing: Auto DJ uses decks 1 and 2");
    case AutoDJProcessor::ADJ_NOT_TWO_DECKS:
        return QStringLiteral("Auto DJ needs decks 1 and 2 on opposite sides of the crossfader");
    }
    return QStringLiteral("Auto DJ couldn't do that");
}

// Where Auto DJ's transition starts on group (seconds), as Mixxx works it out: the outro's start (intro + outro and
// outro start modes, when the track has one), else the transition's length before the end (the audible end, when
// silence is skipped or the outro ends earlier). -1: nothing loaded.
double transitionStart(const QString& group, AutoDJProcessor::TransitionMode mode, double seconds) {
    const double samples = ControlObject::get(ConfigKey(group, QStringLiteral("track_samples")));
    const double duration = ControlObject::get(ConfigKey(group, QStringLiteral("duration")));
    if (samples <= 0 || duration <= 0) {
        return -1;
    }
    const auto at = [&](const char* key) {
        const double v = ControlObject::get(ConfigKey(group, QString::fromLatin1(key)));
        return v >= 0 ? v / samples * duration : -1.0;
    };
    const bool outroMode = mode == AutoDJProcessor::TransitionMode::FullIntroOutro ||
            mode == AutoDJProcessor::TransitionMode::FadeAtOutroStart;
    double end = duration;
    if (mode != AutoDJProcessor::TransitionMode::FixedFullTrack && at("outro_end_position") > 0) {
        end = at("outro_end_position");
    }
    if (outroMode && at("outro_start_position") > 0) {
        return at("outro_start_position");
    }
    return std::max(0.0, end - std::max(0.0, seconds));
}

// Beatmatching for Auto DJ (its own crossfades don't): while it plays, the next track syncs to the playing one (Mixxx's
// sync: tempo and phase, key lock on) when their tempos are within 8 % (or at half or double); further apart, a
// plain crossfade. After a synced transition the deck left playing glides back to its own tempo over 16 beats, so a
// set doesn't stay at its first track's tempo. Every quarter of a second; only the sync it turned on is turned off.
struct Beatmatch {
    bool ours = false;        // sync turned on by this
    bool wasMixing = false;   // both decks played at the last look
    bool synced = false;      // the next transition is a synced one
    QString gliding;          // the deck going back to its own tempo ("": none)
    double step = 0;

    static double get(const QString& group, const char* key) {
        return ControlObject::get(ConfigKey(group, QString::fromLatin1(key)));
    }
    static void set(const QString& group, const char* key, double value) {
        if (get(group, key) != value) {
            ControlObject::set(ConfigKey(group, QString::fromLatin1(key)), value);
        }
    }

    void tick(bool on) {
        const QString d1 = QStringLiteral("[Channel1]"), d2 = QStringLiteral("[Channel2]");
        if (!on || get(QStringLiteral("[AutoDJ]"), "enabled") <= 0) {
            if (ours) {
                set(d1, "sync_enabled", 0);
                set(d2, "sync_enabled", 0);
                ours = false;
            }
            gliding.clear();
            wasMixing = false;
            return;
        }
        const bool p1 = get(d1, "play") > 0, p2 = get(d2, "play") > 0;
        if (p1 && p2) {   // mixing
            wasMixing = true;
            gliding.clear();
            return;
        }
        if (!p1 && !p2) {
            return;
        }
        const QString from = p1 ? d1 : d2, to = p1 ? d2 : d1;
        if (wasMixing) {   // the transition just ended: the deck left playing goes back to its own tempo
            wasMixing = false;
            if (synced && std::abs(get(from, "rate")) > 1e-4) {
                gliding = from;
                const double bpm = get(from, "bpm");
                step = std::abs(get(from, "rate")) / std::max(1.0, (bpm > 0 ? 16 * 60.0 / bpm : 8.0) / 0.25);
                set(from, "sync_enabled", 0);   // (its own tempo now, not the last track's)
                set(from, "keylock", 1);
            }
            synced = false;
        }
        if (gliding == from) {
            const double r = get(from, "rate");
            const double next = std::abs(r) <= step ? 0.0 : r - std::copysign(step, r);
            set(from, "rate", next);
            if (next == 0.0) {
                gliding.clear();
            }
            return;   // (the next track syncs to it once it's back)
        }
        // the next track, waiting: synced to this one when their tempos are near
        if (get(to, "track_loaded") > 0 && get(to, "play") <= 0) {
            const double a = get(from, "bpm"), b = get(to, "file_bpm");
            double r = a > 0 && b > 0 ? b / a : 0;
            while (r > 0 && r < 0.75) {
                r *= 2;
            }
            while (r > 1.5) {
                r /= 2;
            }
            synced = r > 0 && std::abs(r - 1) <= 0.08;
            if (synced) {
                set(to, "keylock", 1);
                set(to, "quantize", 1);
                set(from, "sync_enabled", 1);   // (the playing one leads: its tempo doesn't change)
                set(to, "sync_enabled", 1);
                ours = true;
            } else if (ours) {
                set(to, "sync_enabled", 0);
            }
        }
    }
};

QString stateName(AutoDJProcessor::AutoDJState s) {
    switch (s) {
    case AutoDJProcessor::ADJ_DISABLED:
        return QStringLiteral("off");
    case AutoDJProcessor::ADJ_ENABLE_P1LOADED:
    case AutoDJProcessor::ADJ_ENABLE_P1PLAYING:
        return QStringLiteral("starting");
    case AutoDJProcessor::ADJ_IDLE:
        return QStringLiteral("playing");
    case AutoDJProcessor::ADJ_LEFT_FADING:
    case AutoDJProcessor::ADJ_RIGHT_FADING:
        return QStringLiteral("fading");
    }
    return QStringLiteral("off");
}

struct Watch {
    std::weak_ptr<Library> library;
    UserSettingsPointer config;
    QString dir;
    QJsonArray done;   // the last few requests' outcomes
    QByteArray written;   // the state as last written (it's written again only when it changes)
    QPointer<AutoDJProcessor> processor;   // (found once: Library › AutoDJFeature › AutoDJProcessor)
    Beatmatch beatmatch;

    AutoDJProcessor* autoDj() {
        if (!processor) {
            const std::shared_ptr<Library> pLibrary = library.lock();
            processor = pLibrary ? pLibrary->findChild<AutoDJProcessor*>() : nullptr;
        }
        return processor;
    }

    // One request, carried out; "" or why not
    QString carryOut(const QJsonObject& ask) {
        const std::shared_ptr<Library> pLibrary = library.lock();
        AutoDJProcessor* p = autoDj();
        TrackCollection* pTracks = pLibrary && pLibrary->trackCollectionManager()
                ? pLibrary->trackCollectionManager()->internalCollection()
                : nullptr;
        if (!p || !pTracks) {
            return QStringLiteral("Mixxx's Auto DJ isn't ready");
        }
        const QString what = ask.value(QStringLiteral("do")).toString();
        if (what == QLatin1String("on") || what == QLatin1String("off")) {
            return errorText(p->toggleAutoDJ(what == QLatin1String("on")));
        }
        if (what == QLatin1String("fade")) {
            if (p->getState() == AutoDJProcessor::ADJ_DISABLED) {
                return errorText(AutoDJProcessor::ADJ_IS_INACTIVE);
            }
            p->fadeNow();   // (nothing while it's already fading)
            return {};
        }
        if (what == QLatin1String("skip")) {
            return errorText(p->skipNext());
        }
        if (what == QLatin1String("skiptrack")) {   // the playing one, gracefully: 16 beats before its transition
            if (p->getState() == AutoDJProcessor::ADJ_DISABLED) {
                return errorText(AutoDJProcessor::ADJ_IS_INACTIVE);
            }
            if (p->getState() != AutoDJProcessor::ADJ_IDLE) {
                return {};   // (it's mixing already)
            }
            QString group;
            for (const QString& g : {QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]")}) {
                if (ControlObject::get(ConfigKey(g, QStringLiteral("play"))) > 0) {
                    group = g;
                }
            }
            const double start = group.isEmpty() ? -1 : transitionStart(group, p->getTransitionMode(), p->getTransitionTime());
            const double duration = group.isEmpty() ? 0 : ControlObject::get(ConfigKey(group, QStringLiteral("duration")));
            if (start < 0 || duration <= 0) {
                return QStringLiteral("Nothing playing to skip");
            }
            const double bpm = ControlObject::get(ConfigKey(group, QStringLiteral("bpm")));
            const double lead = bpm > 0 ? 16 * 60.0 / bpm : 8.0;
            const double now = ControlObject::get(ConfigKey(group, QStringLiteral("playposition"))) * duration;
            if (start - lead <= now + 1) {
                p->fadeNow();   // (that close already: the transition now)
            } else {
                ControlObject::set(ConfigKey(group, QStringLiteral("playposition")), (start - lead) / duration);
            }
            return {};
        }
        if (what == QLatin1String("random")) {   // as its add_random_track: it asks its feature for one
            emit p->randomTrackRequested(1);
            return {};
        }
        if (what == QLatin1String("shuffle")) {
            return errorText(p->shufflePlaylist(QModelIndexList()));
        }
        if (what == QLatin1String("clear")) {
            pTracks->getPlaylistDAO().clearAutoDJQueue();
            return {};
        }
        if (what == QLatin1String("transition")) {
            const QString mode = ask.value(QStringLiteral("mode")).toString();
            if (!mode.isEmpty()) {
                const int m = kModes.indexOf(mode);
                if (m < 0) {
                    return QStringLiteral("Unknown transition");
                }
                p->setTransitionMode(static_cast<AutoDJProcessor::TransitionMode>(m));
            }
            if (ask.contains(QStringLiteral("seconds"))) {
                const int s = ask.value(QStringLiteral("seconds")).toInt();
                if (s < -30 || s > 60) {
                    return QStringLiteral("A transition is -30 to 60 seconds");
                }
                p->setTransitionTime(s);
            }
            return {};
        }
        if (what == QLatin1String("options")) {   // (Mixxx reads these each time it takes a track from the queue)
            if (ask.contains(QStringLiteral("random"))) {
                config->setValue(ConfigKey(kPrefs, QStringLiteral("EnableRandomQueue")), ask.value(QStringLiteral("random")).toInt() ? 1 : 0);
            }
            if (ask.contains(QStringLiteral("min"))) {
                config->setValue(ConfigKey(kPrefs, QStringLiteral("RandomQueueMinimumAllowed")),
                        std::clamp(ask.value(QStringLiteral("min")).toInt(), 1, 50));
            }
            if (ask.contains(QStringLiteral("beatmatch"))) {
                config->setValue(ConfigKey(QStringLiteral("[ZyDeck]"), QStringLiteral("AutoDjBeatmatch")), ask.value(QStringLiteral("beatmatch")).toInt() ? 1 : 0);
            }
            if (ask.contains(QStringLiteral("requeue"))) {
                config->setValue(ConfigKey(kPrefs, QStringLiteral("Requeue")), ask.value(QStringLiteral("requeue")).toInt() ? 1 : 0);
            }
            if (config->getValue(ConfigKey(kPrefs, QStringLiteral("EnableRandomQueue")), 0) == 1) {   // a short queue fills now
                const QList<QPair<int, QString>> queue = pTracks->getPlaylistDAO().getPlaylists(PlaylistDAO::PLHT_AUTO_DJ);
                const int count = queue.isEmpty() ? 0 : pTracks->getPlaylistDAO().tracksInPlaylist(queue.first().first);
                const int wanted = config->getValue(ConfigKey(kPrefs, QStringLiteral("RandomQueueMinimumAllowed")), 5);
                if (count < wanted) {
                    emit p->randomTrackRequested(wanted - count);
                }
            }
            return {};
        }
        if (what == QLatin1String("source")) {   // (Mixxx's Auto DJ hears of it: its crate list follows)
            const CrateId crate{QVariant(ask.value(QStringLiteral("crate")).toInt())};
            return pTracks->updateAutoDjCrate(crate, ask.value(QStringLiteral("on")).toInt() != 0)
                    ? QString()
                    : QStringLiteral("Couldn't change the crate");
        }
        const TrackId track{QVariant(ask.value(QStringLiteral("track")).toInt())};
        if (what == QLatin1String("add")) {
            const QString where = ask.value(QStringLiteral("where")).toString();
            pTracks->getPlaylistDAO().addTracksToAutoDJQueue({track},
                    where == QLatin1String("top")           ? PlaylistDAO::AutoDJSendLoc::TOP
                            : where == QLatin1String("replace") ? PlaylistDAO::AutoDJSendLoc::REPLACE
                                                                : PlaylistDAO::AutoDJSendLoc::BOTTOM);
            return {};
        }
        if (what == QLatin1String("remove")) {
            const QList<QPair<int, QString>> queue = pTracks->getPlaylistDAO().getPlaylists(PlaylistDAO::PLHT_AUTO_DJ);
            if (!queue.isEmpty()) {
                pTracks->getPlaylistDAO().removeTracksFromPlaylistById(queue.first().first, track);
            }
            return {};
        }
        return QStringLiteral("Unknown Auto DJ request");
    }

    void writeState() {
        AutoDJProcessor* p = autoDj();
        const QString state = p ? stateName(p->getState()) : QStringLiteral("off");
        const QJsonObject out{{"ready", p != nullptr},
                {"on", state != QLatin1String("off")},
                {"state", state},
                {"transition", QJsonObject{{"seconds", p ? p->getTransitionTime() : 10.0},
                                       {"mode", kModes.value(p ? static_cast<int>(p->getTransitionMode()) : 0, kModes.first())}}},
                {"random", QJsonObject{{"on", config->getValue(ConfigKey(kPrefs, QStringLiteral("EnableRandomQueue")), 0) == 1},
                                   {"min", config->getValue(ConfigKey(kPrefs, QStringLiteral("RandomQueueMinimumAllowed")), 5)}}},
                {"requeue", config->getValue(ConfigKey(kPrefs, QStringLiteral("Requeue")), 0) == 1},
                {"beatmatch", config->getValue(ConfigKey(QStringLiteral("[ZyDeck]"), QStringLiteral("AutoDjBeatmatch")), 0) == 1},
                {"done", done}};
        const QByteArray json = QJsonDocument(out).toJson(QJsonDocument::Compact);
        if (json == written) {
            return;
        }
        QSaveFile f(QDir(dir).filePath(QStringLiteral("zydeck-autodj-state.json")));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(json);
            if (f.commit()) {
                written = json;
            }
        }
    }

    void tick() {
        const QStringList asks = QDir(dir).entryList({QStringLiteral("zydeck-autodj-*.json")}, QDir::Files, QDir::Name);
        for (const QString& name : asks) {
            if (name == QLatin1String("zydeck-autodj-state.json")) {
                continue;
            }
            QFile f(QDir(dir).filePath(name));
            if (!f.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QJsonObject ask = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            f.remove();
            const QString error = carryOut(ask);
            done.append(QJsonObject{{"id", ask.value(QStringLiteral("id"))}, {"ok", error.isEmpty()}, {"error", error}});
            while (done.size() > 8) {
                done.removeFirst();
            }
        }
        beatmatch.tick(config->getValue(ConfigKey(QStringLiteral("[ZyDeck]"), QStringLiteral("AutoDjBeatmatch")), 0) == 1);
        writeState();
    }
};

} // namespace

void watchAutoDj(const std::shared_ptr<Library>& pLibrary, UserSettingsPointer pConfig) {
    auto pWatch = std::make_shared<Watch>();
    pWatch->library = pLibrary;
    pWatch->config = pConfig;
    pWatch->dir = pConfig->getSettingsPath();
    for (const QString& stale : QDir(pWatch->dir).entryList({QStringLiteral("zydeck-autodj-*.json")}, QDir::Files)) {
        QFile::remove(QDir(pWatch->dir).filePath(stale));   // (requests a previous run left: not now)
    }
    auto* pTimer = new QTimer(QCoreApplication::instance());   // (a quarter of a second: a person's waiting for it)
    pTimer->setInterval(250);
    QObject::connect(pTimer, &QTimer::timeout, pTimer, [pWatch] { pWatch->tick(); });
    pTimer->start();
}

} // namespace zydeck
