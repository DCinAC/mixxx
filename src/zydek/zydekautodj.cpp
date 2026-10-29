#include "zydek/zydekautodj.h"

#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "library/autodj/autodjprocessor.h"
#include "library/dao/playlistdao.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "qml/qmllibraryproxy.h"
#include "zydek/zydeklibrary.h"
#include "zydek/zydekmain.h"

namespace zydek {

namespace {

const QString kPrefs = QStringLiteral("[Auto DJ]");   // Mixxx's Auto DJ preferences (the controls are [AutoDJ])
const QString kZyDeck = QStringLiteral("[ZyDeck]");    // autoplay's pool
constexpr int kAhead = 2;                             // tracks autoplay keeps queued
// Mixxx's TransitionMode, in its order
const QStringList kModes = {QStringLiteral("intro-outro"),
        QStringLiteral("outro-start"),
        QStringLiteral("full-track"),
        QStringLiteral("skip-silence"),
        QStringLiteral("skip-silence-full")};

QJsonObject error(const QString& message) {
    return {{"ok", false}, {"error", message}};
}

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

Beatmatch& beatmatch() {   // (one Auto DJ: the hub's thread's)
    static Beatmatch b;
    return b;
}

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

// Mixxx's Auto DJ, which its Library made (Library › AutoDJFeature › AutoDJProcessor): found, not kept, as Mixxx
// may make a new one
AutoDJProcessor* processor() {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    return pLibrary ? pLibrary->findChild<AutoDJProcessor*>() : nullptr;
}

TrackCollection* collection() {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    return pLibrary && pLibrary->trackCollectionManager() ? pLibrary->trackCollectionManager()->internalCollection() : nullptr;
}

} // namespace

AutoDj::AutoDj(UserSettingsPointer pConfig, Library* pLibrary)
        : m_pConfig(pConfig),
          m_pLibrary(pLibrary) {
}

QJsonObject AutoDj::autoplayJson() const {
    QJsonObject o = m_pool.toJson();
    o.insert(QStringLiteral("on"), m_autoplay);
    return o;
}

void AutoDj::readPool() {
    if (!m_poolRead) {   // (the pool as it was last set; autoplay itself starts off)
        m_poolRead = true;
        m_pool = zydeck::library::Pool::fromJson(
                QJsonDocument::fromJson(m_pConfig->getValueString(ConfigKey(kZyDeck, QStringLiteral("AutoplayPool"))).toUtf8()).object());
    }
}

QJsonObject AutoDj::status() {
    readPool();
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    QString state = QStringLiteral("off");
    double seconds = 10;
    int mode = 0;
    const bool ready = runOnMain(pLibrary, [&] {
        if (AutoDJProcessor* p = processor()) {
            state = stateName(p->getState());
            seconds = p->getTransitionTime();
            mode = static_cast<int>(p->getTransitionMode());
        }
    }, 3000);
    const QJsonObject views = m_pLibrary->views();
    QJsonArray crates;
    for (const QJsonValue& c : views.value(QStringLiteral("crates")).toArray()) {
        crates.append(c);
    }
    const QJsonObject queue = views.value(QStringLiteral("autodj")).toObject();
    return {{"ok", ready},
            {"on", state != QLatin1String("off")},
            {"state", state},
            {"queue", QJsonObject{{"id", queue.value(QStringLiteral("id")).toInt()}, {"count", queue.value(QStringLiteral("count")).toInt()}}},
            {"transition", QJsonObject{{"seconds", seconds}, {"mode", kModes.value(mode, kModes.first())}}},
            {"random", QJsonObject{
                    {"on", m_pConfig->getValueString(ConfigKey(kPrefs, QStringLiteral("EnableRandomQueue"))).toInt() == 1},
                    {"min", m_pConfig->getValue(ConfigKey(kPrefs, QStringLiteral("RandomQueueMinimumAllowed")), 5)}}},
            {"requeue", m_pConfig->getValueString(ConfigKey(kPrefs, QStringLiteral("Requeue"))).toInt() == 1},
            {"crates", crates},
            {"beatmatch", m_pConfig->getValue(ConfigKey(kZyDeck, QStringLiteral("AutoDjBeatmatch")), 0) == 1},
            {"autoplay", autoplayJson()}};
}

QJsonObject AutoDj::setOn(bool on) {
    AutoDJProcessor::AutoDJError e = AutoDJProcessor::ADJ_OK;
    bool found = false;
    if (!runOnMain(mixxx::qml::QmlLibraryProxy::get(), [&] {
            if (AutoDJProcessor* p = processor()) {
                found = true;
                e = p->toggleAutoDJ(on);
            }
        }) || !found) {
        return error(QStringLiteral("Mixxx's Auto DJ isn't ready"));
    }
    if (e != AutoDJProcessor::ADJ_OK) {
        return error(errorText(e));
    }
    return {{"ok", true}};
}

QJsonObject AutoDj::act(const QString& action) {
    AutoDJProcessor::AutoDJError e = AutoDJProcessor::ADJ_OK;
    bool found = false, known = true;
    if (!runOnMain(mixxx::qml::QmlLibraryProxy::get(), [&] {
            AutoDJProcessor* p = processor();
            if (!p) {
                return;
            }
            found = true;
            if (action == QLatin1String("fade")) {
                if (p->getState() == AutoDJProcessor::ADJ_DISABLED) {
                    e = AutoDJProcessor::ADJ_IS_INACTIVE;
                } else {
                    p->fadeNow();   // (does nothing while it's already fading)
                }
            } else if (action == QLatin1String("skip")) {
                e = p->skipNext();
            } else if (action == QLatin1String("random")) {   // as its add_random_track: it asks its feature for one
                emit p->randomTrackRequested(1);
            } else if (action == QLatin1String("skiptrack")) {
                // the playing track, skipped gracefully: to 16 beats before its transition, which then goes as usual
                if (p->getState() != AutoDJProcessor::ADJ_IDLE) {
                    e = p->getState() == AutoDJProcessor::ADJ_DISABLED ? AutoDJProcessor::ADJ_IS_INACTIVE : AutoDJProcessor::ADJ_OK;
                    return;
                }
                QString group;
                for (const QString g : {QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]")}) {
                    if (ControlObject::get(ConfigKey(g, QStringLiteral("play"))) > 0) {
                        group = g;
                    }
                }
                const double start = group.isEmpty() ? -1 : transitionStart(group, p->getTransitionMode(), p->getTransitionTime());
                const double duration = ControlObject::get(ConfigKey(group, QStringLiteral("duration")));
                if (start < 0 || duration <= 0) {
                    return;
                }
                const double bpm = ControlObject::get(ConfigKey(group, QStringLiteral("bpm")));
                const double lead = bpm > 0 ? 16 * 60.0 / bpm : 8.0;
                const double now = ControlObject::get(ConfigKey(group, QStringLiteral("playposition"))) * duration;
                if (start - lead <= now + 1) {
                    p->fadeNow();   // (that close already: the transition now)
                } else {
                    ControlObject::set(ConfigKey(group, QStringLiteral("playposition")), (start - lead) / duration);
                }
                e = p->shufflePlaylist(QModelIndexList());   // (all of it but the track that's loaded next)
            } else {
                known = false;
            }
        }) || !found) {
        return error(QStringLiteral("Mixxx's Auto DJ isn't ready"));
    }
    if (!known) {
        return error(QStringLiteral("Unknown Auto DJ action"));
    }
    if (e != AutoDJProcessor::ADJ_OK) {
        return error(errorText(e));
    }
    return {{"ok", true}};
}

QJsonObject AutoDj::setTransition(const QString& seconds, const QString& mode) {
    const int m = kModes.indexOf(mode);
    if (!mode.isEmpty() && m < 0) {
        return error(QStringLiteral("Unknown transition"));
    }
    bool ok = false;
    const int s = seconds.toInt(&ok);
    if (!seconds.isEmpty() && (!ok || s < -30 || s > 60)) {
        return error(QStringLiteral("A transition is -30 to 60 seconds"));
    }
    bool found = false;
    if (!runOnMain(mixxx::qml::QmlLibraryProxy::get(), [&] {
            if (AutoDJProcessor* p = processor()) {
                found = true;
                if (!mode.isEmpty()) {
                    p->setTransitionMode(static_cast<AutoDJProcessor::TransitionMode>(m));
                }
                if (!seconds.isEmpty()) {
                    p->setTransitionTime(s);
                }
            }
        }) || !found) {
        return error(QStringLiteral("Mixxx's Auto DJ isn't ready"));
    }
    return {{"ok", true}};
}

QJsonObject AutoDj::setOptions(const QString& random, const QString& min, const QString& requeue, const QString& beatmatch) {
    if (!beatmatch.isEmpty()) {
        m_pConfig->setValue(ConfigKey(kZyDeck, QStringLiteral("AutoDjBeatmatch")), beatmatch == QLatin1String("1") ? 1 : 0);
    }
    // (Mixxx reads these each time it takes a track from the queue)
    if (!random.isEmpty()) {
        m_pConfig->setValue(ConfigKey(kPrefs, QStringLiteral("EnableRandomQueue")), random == QLatin1String("1") ? 1 : 0);
    }
    if (!min.isEmpty()) {
        m_pConfig->setValue(ConfigKey(kPrefs, QStringLiteral("RandomQueueMinimumAllowed")), std::clamp(min.toInt(), 1, 50));
    }
    if (!requeue.isEmpty()) {
        m_pConfig->setValue(ConfigKey(kPrefs, QStringLiteral("Requeue")), requeue == QLatin1String("1") ? 1 : 0);
    }
    if (random == QLatin1String("1")) {   // a short queue fills now, not only when the next track is taken
        const int count = m_pLibrary->views().value(QStringLiteral("autodj")).toObject().value(QStringLiteral("count")).toInt();
        const int wanted = m_pConfig->getValue(ConfigKey(kPrefs, QStringLiteral("RandomQueueMinimumAllowed")), 5);
        if (count < wanted) {
            runOnMain(mixxx::qml::QmlLibraryProxy::get(), [&] {
                if (AutoDJProcessor* p = processor()) {
                    emit p->randomTrackRequested(wanted - count);
                }
            });
        }
    }
    return {{"ok", true}};
}

QJsonObject AutoDj::setSource(int crateId, bool source) {
    bool ok = false;
    runOnMain(mixxx::qml::QmlLibraryProxy::get(), [&] {   // (Mixxx's Auto DJ hears of it: its crate list follows)
        if (TrackCollection* pTracks = collection()) {
            ok = pTracks->updateAutoDjCrate(CrateId(QVariant(crateId)), source);
        }
    });
    return ok ? QJsonObject{{"ok", true}} : error(QStringLiteral("Couldn't change the crate"));
}

QJsonObject AutoDj::add(int trackId, const QString& where) {
    const PlaylistDAO::AutoDJSendLoc loc = where == QLatin1String("top") ? PlaylistDAO::AutoDJSendLoc::TOP
            : where == QLatin1String("replace")                           ? PlaylistDAO::AutoDJSendLoc::REPLACE
                                                                          : PlaylistDAO::AutoDJSendLoc::BOTTOM;
    bool ok = false;
    runOnMain(mixxx::qml::QmlLibraryProxy::get(), [&] {
        if (TrackCollection* pTracks = collection()) {
            pTracks->getPlaylistDAO().addTracksToAutoDJQueue({TrackId(QVariant(trackId))}, loc);
            ok = true;
        }
    });
    return ok ? QJsonObject{{"ok", true}} : error(QStringLiteral("Mixxx isn't ready"));
}

QJsonObject AutoDj::remove(int trackId) {
    bool ok = false;
    runOnMain(mixxx::qml::QmlLibraryProxy::get(), [&] {
        if (TrackCollection* pTracks = collection()) {
            PlaylistDAO& dao = pTracks->getPlaylistDAO();
            const QList<QPair<int, QString>> queue = dao.getPlaylists(PlaylistDAO::PLHT_AUTO_DJ);
            if (!queue.isEmpty()) {
                dao.removeTracksFromPlaylistById(queue.first().first, TrackId(QVariant(trackId)));
                ok = true;
            }
        }
    });
    return ok ? QJsonObject{{"ok", true}} : error(QStringLiteral("Mixxx isn't ready"));
}

QJsonObject AutoDj::clear() {
    bool ok = false;
    runOnMain(mixxx::qml::QmlLibraryProxy::get(), [&] {
        if (TrackCollection* pTracks = collection()) {
            pTracks->getPlaylistDAO().clearAutoDJQueue();
            ok = true;
        }
    });
    return ok ? QJsonObject{{"ok", true}} : error(QStringLiteral("Mixxx isn't ready"));
}

QJsonObject AutoDj::setAutoplay(bool on, const QJsonObject& pool) {
    readPool();
    if (!pool.isEmpty()) {
        m_pool = zydeck::library::Pool::fromJson(pool);
        m_pConfig->setValue(ConfigKey(kZyDeck, QStringLiteral("AutoplayPool")),
                QString::fromUtf8(QJsonDocument(m_pool.toJson()).toJson(QJsonDocument::Compact)));
    }
    m_autoplay = on;
    if (!on) {
        return {{"ok", true}};
    }
    // autoplay picks: Mixxx's own top-up from the crates would add others
    m_pConfig->setValue(ConfigKey(kPrefs, QStringLiteral("EnableRandomQueue")), 0);
    topUp();
    if (m_pLibrary->autoDjQueueLength() == 0) {
        m_autoplay = false;
        return error(QStringLiteral("Nothing in the library fits (or its files aren't there)"));
    }
    if (ControlObject::get(ConfigKey(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled"))) > 0) {
        return {{"ok", true}};
    }
    return setOn(true);   // (it says why not: decks 1 and 2 both playing, say; autoplay stays on for when it starts)
}

void AutoDj::topUp() {
    const int queued = m_pLibrary->autoDjQueueLength();
    if (queued >= kAhead) {
        return;
    }
    const QList<int> picked = m_pLibrary->pick(m_pool, kAhead - queued);
    if (picked.isEmpty()) {
        return;
    }
    autoplayHistory();
    for (int id : picked) {
        add(id, QStringLiteral("bottom"));
        m_history.removeAll(id);
        m_history.prepend(id);
    }
    while (m_history.size() > 500) {
        m_history.removeLast();
    }
    QJsonArray ids;
    for (int id : std::as_const(m_history)) {
        ids.append(id);
    }
    QSaveFile f(QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("zydeck-autoplay-history.json")));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(ids).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

QList<int> AutoDj::autoplayHistory() {
    if (!m_historyRead) {
        m_historyRead = true;
        QFile f(QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("zydeck-autoplay-history.json")));
        if (f.open(QIODevice::ReadOnly)) {
            for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).array()) {
                if (v.toInt() > 0) {
                    m_history.append(v.toInt());
                }
            }
        }
    }
    return m_history;
}

void AutoDj::autoplayTick() {
    if (m_autoplay && ControlObject::get(ConfigKey(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled"))) > 0) {
        topUp();
    }
}

void AutoDj::beatmatchTick() {
    beatmatch().tick(m_pConfig->getValue(ConfigKey(kZyDeck, QStringLiteral("AutoDjBeatmatch")), 0) == 1);
}

} // namespace zydek
