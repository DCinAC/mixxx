#include "zydek/zydekautodj.h"

#include <QJsonArray>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <functional>
#include <atomic>
#include <memory>
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
#include "mixer/playerinfo.h"
#include "track/track.h"
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

// Beatmatching for Auto DJ (its own crossfades don't): when the next track's tempo is within 8 % of the playing one's
// (or at half or double), the playing one goes to the next one's tempo before the mix (from 32 beats before it, over
// 16 beats: the old track to the new one, so the new one plays at its own tempo, in the mix and after), and once
// they match the two sync for the mix (Mixxx's sync: tempo and phase; key lock on). Further apart: a plain crossfade.
// A mix that comes before the old track got there (fade now, a slider whose range won't reach): synced anyway, and
// the track left playing goes back to its own tempo after, over 16 beats. Every quarter of a second; only the sync it
// turned on is turned off. transitionAt: where the mix starts, seconds into the playing track (-1: not known).
struct Beatmatch {
    bool ours = false;        // sync turned on by this
    bool wasMixing = false;   // both decks played at the last look
    bool synced = false;      // this pair mixes synced
    QString gliding;          // after a mix: the deck going back to its own tempo ("": none)
    double step = 0;
    QString heading;          // before a mix: the playing deck going to the next one's tempo
    double headStep = 0, target = 0;

    static double get(const QString& group, const char* key) {
        return ControlObject::get(ConfigKey(group, QString::fromLatin1(key)));
    }
    static void set(const QString& group, const char* key, double value) {
        if (get(group, key) != value) {
            ControlObject::set(ConfigKey(group, QString::fromLatin1(key)), value);
        }
    }
    // the rate slider's value for a tempo (as far as its range goes)
    static double rateFor(const QString& group, double bpm) {
        const double file = get(group, "file_bpm"), range = get(group, "rateRange"), dir = get(group, "rate_dir");
        if (file <= 0 || range <= 0) {
            return get(group, "rate");
        }
        return std::clamp((bpm / file - 1) / (range * (dir < 0 ? -1 : 1)), -1.0, 1.0);
    }

    void tick(bool on, double transitionAt) {
        const QString d1 = QStringLiteral("[Channel1]"), d2 = QStringLiteral("[Channel2]");
        if (!on || get(QStringLiteral("[AutoDJ]"), "enabled") <= 0) {
            if (ours) {
                set(d1, "sync_enabled", 0);
                set(d2, "sync_enabled", 0);
                ours = false;
            }
            gliding.clear();
            heading.clear();
            wasMixing = false;
            return;
        }
        const bool p1 = get(d1, "play") > 0, p2 = get(d2, "play") > 0;
        if (p1 && p2) {   // mixing
            wasMixing = true;
            gliding.clear();
            heading.clear();
            return;
        }
        if (!p1 && !p2) {
            return;
        }
        const QString from = p1 ? d1 : d2, to = p1 ? d2 : d1;
        if (wasMixing) {   // the mix is over: each at its own tempo from here
            wasMixing = false;
            if (ours) {
                set(d1, "sync_enabled", 0);
                set(d2, "sync_enabled", 0);
                ours = false;
            }
            if (synced && std::abs(get(from, "rate") - rateFor(from, get(from, "file_bpm"))) > 0.002) {
                gliding = from;   // (it came in stretched: back to its own tempo)
                const double bpm = get(from, "bpm");
                step = std::abs(get(from, "rate") - rateFor(from, get(from, "file_bpm"))) /
                        std::max(1.0, (bpm > 0 ? 16 * 60.0 / bpm : 8.0) / 0.25);
            }
            synced = false;
            target = 0;
        }
        if (gliding == from) {
            const double r = get(from, "rate"), goal = rateFor(from, get(from, "file_bpm"));
            const double next = std::abs(r - goal) <= step ? goal : r - std::copysign(step, r - goal);
            set(from, "rate", next);
            if (next == goal) {
                gliding.clear();
            }
            return;
        }
        if (get(to, "track_loaded") <= 0 || get(to, "play") > 0) {
            return;
        }
        // the next track's tempo, at the playing one's (half or double counted)
        const double a = get(from, "bpm");
        double want = get(to, "file_bpm");
        while (want > 0 && a > 0 && want < a * 0.75) {
            want *= 2;
        }
        while (a > 0 && want > a * 1.5) {
            want /= 2;
        }
        synced = a > 0 && want > 0 && std::abs(want / a - 1) <= 0.08;
        if (!synced) {
            if (ours) {
                set(to, "sync_enabled", 0);
            }
            heading.clear();
            return;
        }
        set(to, "keylock", 1);
        set(to, "quantize", 1);
        set(from, "keylock", 1);
        const double duration = get(from, "duration"), now = get(from, "playposition") * duration;
        const double fileFrom = get(from, "file_bpm");
        const double beat = fileFrom > 0 ? 60 / fileFrom : 0.5;   // (in the playing track's own seconds)
        const bool there = std::abs(a - want) < 0.05;
        if (!there && heading.isEmpty() && target != want && (transitionAt < 0 || now >= transitionAt - 32 * beat)) {
            heading = from;   // to the next one's tempo, over 16 beats
            target = want;
            headStep = (rateFor(from, want) - get(from, "rate")) / std::max(1.0, 16 * 60.0 / a / 0.25);
            set(from, "sync_enabled", 0);
        }
        if (heading == from) {
            const double goal = rateFor(from, target);
            double r = get(from, "rate") + headStep;
            if ((headStep >= 0 && r >= goal) || (headStep < 0 && r <= goal)) {
                r = goal;
                heading.clear();   // (there, or as near as its slider goes)
            }
            set(from, "rate", r);
            return;
        }
        // matched (or the mix about to start): the two synced, the playing one leading, so the next one starts at its
        // own tempo, its beats on the playing one's
        if (there || (transitionAt >= 0 && now >= transitionAt - 2 * beat)) {
            set(from, "sync_enabled", 1);
            set(to, "sync_enabled", 1);
            ours = true;
        }
    }
};

// Auto DJ's phrase mixing, from the tracks' structure (zydeckstructure.h: their sections): where the next track comes
// in, where the playing one goes out, the mix's length in bars, and how it's mixed; Mixxx's Auto DJ does the rest.
// The playing track goes out by Mixxx's "fade now" at a section's start (the exit); the next one is cued before that
// so its chosen section lands as the mix ends (Mixxx starts it from where it's cued). A choice left as Mixxx's
// (intro, outro, blend) leaves that part to Mixxx; tracks whose sections aren't known (no beat grid) are mixed as
// Mixxx mixes them. Settings, [ZyDeck] in Mixxx's: MixEntry, MixExit, MixBars, MixStyle.
// (The same in the device's hub, zydek/zydekautodj.cpp, and Mixxx ZyDeck's, zydeck/autodjrequest.cpp.)
struct MixSettings {
    QString entry = QStringLiteral("land-chorus");   // intro | land-chorus | land-verse | chorus | adaptive
    QString exit = QStringLiteral("outro");          // outro | last-chorus | second-chorus | minutes:N
    int bars = 16;                                   // 4 | 8 | 16 | 32
    QString style = QStringLiteral("adaptive");      // blend | bassswap | filter | cut | adaptive

    static MixSettings read(const UserSettingsPointer& config) {
        const QString g = QStringLiteral("[ZyDeck]");
        MixSettings s;
        s.entry = config->getValue(ConfigKey(g, QStringLiteral("MixEntry")), s.entry);
        s.exit = config->getValue(ConfigKey(g, QStringLiteral("MixExit")), s.exit);
        s.bars = std::clamp(config->getValue(ConfigKey(g, QStringLiteral("MixBars")), s.bars), 1, 64);
        s.style = config->getValue(ConfigKey(g, QStringLiteral("MixStyle")), s.style);
        return s;
    }
    // Each of them that's given (empty: as it is)
    static void write(const UserSettingsPointer& config, const QJsonObject& change) {
        const QString g = QStringLiteral("[ZyDeck]");
        static const QStringList kEntries{"intro", "land-chorus", "land-verse", "chorus", "adaptive"};
        static const QStringList kStyles{"blend", "bassswap", "filter", "cut", "adaptive"};
        static const QRegularExpression kExit(QStringLiteral(R"(^(outro|last-chorus|second-chorus|minutes:\d{1,2})$)"));
        const QString entry = change.value(QStringLiteral("entry")).toString();
        const QString exit = change.value(QStringLiteral("exit")).toString();
        const QString style = change.value(QStringLiteral("style")).toString();
        const int bars = change.value(QStringLiteral("bars")).toInt();
        if (kEntries.contains(entry)) {
            config->setValue(ConfigKey(g, QStringLiteral("MixEntry")), entry);
        }
        if (kExit.match(exit).hasMatch()) {
            config->setValue(ConfigKey(g, QStringLiteral("MixExit")), exit);
        }
        if (kStyles.contains(style)) {
            config->setValue(ConfigKey(g, QStringLiteral("MixStyle")), style);
        }
        if (bars >= 1 && bars <= 64) {
            config->setValue(ConfigKey(g, QStringLiteral("MixBars")), bars);
        }
    }
    QJsonObject json() const {
        return {{"entry", entry}, {"exit", exit}, {"bars", bars}, {"style", style}};
    }
};

struct MixSection {
    double start = 0, end = 0;   // seconds
    QString label;               // Intro | Verse | Pre-Chorus | Chorus | Bridge | Interlude | Outro
    double energy = 0;
};

inline QList<MixSection> mixSections(const QJsonObject& structure) {
    QList<MixSection> out;
    if (!structure.value(QStringLiteral("ok")).toBool() || structure.value(QStringLiteral("pending")).toBool()) {
        return out;
    }
    for (const QJsonValue& v : structure.value(QStringLiteral("sections")).toArray()) {
        const QJsonObject s = v.toObject();
        out.append({s.value(QStringLiteral("s")).toDouble(), s.value(QStringLiteral("e")).toDouble(),
                s.value(QStringLiteral("label")).toString(), s.value(QStringLiteral("energy")).toDouble()});
    }
    return out;
}

class MixPlanner {
  public:
    using Sections = std::function<QList<MixSection>(int trackId)>;   // (empty: not known, yet or at all)

    // Four times a second, on Mixxx's main thread
    void tick(AutoDJProcessor* p, const MixSettings& set, const Sections& sections, QObject* context) {
        if (!p || p->getState() == AutoDJProcessor::ADJ_DISABLED) {
            if (m_fade.active) {
                endFade();
            }
            if (p && m_userTime > -1000) {   // (the length the user set, back)
                p->setTransitionTime(static_cast<int>(m_userTime));
            }
            m_userTime = -1000;
            m_fromId = m_toId = -1;
            setPlan({});
            return;
        }
        const AutoDJProcessor::AutoDJState state = p->getState();
        if (state == AutoDJProcessor::ADJ_LEFT_FADING || state == AutoDJProcessor::ADJ_RIGHT_FADING) {
            automate(state);
            return;
        }
        if (m_fade.active) {
            endFade();
        }
        if (state != AutoDJProcessor::ADJ_IDLE) {
            return;
        }
        const bool p1 = get(kD1, "play") > 0, p2 = get(kD2, "play") > 0;
        if (p1 == p2) {
            return;   // (none playing, or both: the user's)
        }
        const QString from = p1 ? kD1 : kD2, to = p1 ? kD2 : kD1;
        const int fromId = trackId(from), toId = trackId(to);
        if (fromId <= 0 || toId <= 0 || get(to, "play") > 0) {
            return;
        }
        if (fromId != m_fromId || toId != m_toId) {   // a new pair: planned once its sections are known
            m_fromId = fromId;
            m_toId = toId;
            m_from = from;
            m_to = to;
            m_planned = m_fired = false;
            m_since.start();
        }
        if (!m_planned) {
            const QList<MixSection> f = sections(fromId), t = sections(toId);
            if ((f.isEmpty() || t.isEmpty()) && m_since.elapsed() < 20000) {
                return;   // (being worked out: a few seconds; past that, planned with what's known)
            }
            plan(p, set, f, t);
            m_planned = true;
        }
        if (m_exit < 0 || m_fired) {
            return;
        }
        const double duration = get(from, "duration");
        const double pos = get(from, "playposition") * duration;
        const double remaining = (m_exit - pos) / speed(from);
        if (remaining > 0.3) {
            return;
        }
        m_fired = true;
        if (remaining < -1.0) {   // (jumped past it: Mixxx's own transition, then)
            return;
        }
        // to the millisecond, not the next look
        QPointer<AutoDJProcessor> pp(p);
        QTimer::singleShot(std::max(0, static_cast<int>(remaining * 1000)), context, [this, pp] {
            if (pp && pp->getState() == AutoDJProcessor::ADJ_IDLE && trackId(m_from) == m_fromId) {
                // (Mixxx cues a track it's just loaded back to its intro: the cue again, if it was moved)
                const double toDuration = get(m_to, "duration");
                if (m_cue >= 0 && toDuration > 0 && std::abs(get(m_to, "playposition") * toDuration - m_cue) > 0.5) {
                    ControlObject::set(ConfigKey(m_to, QStringLiteral("playposition")), m_cue / toDuration);
                }
                pp->setTransitionTime(m_style == QLatin1String("cut") ? 0 : static_cast<int>(std::lround(m_length)));
                pp->fadeNow();
            }
        });
    }

    // Where the planned mix starts, seconds into the playing track (-1: none planned: Mixxx's own)
    double exitAt() const {
        return m_planned && !m_fired ? m_exit : -1;
    }

    // The next transition as planned, for the pages: {out, outAt, in, inAt, style, bars}
    QJsonObject plan() const {
        QMutexLocker lock(&m_planMutex);
        return m_plan;
    }

  private:
    static constexpr const char* kD1 = "[Channel1]";
    static constexpr const char* kD2 = "[Channel2]";

    struct Fade {
        bool active = false;
        QString from, to, style;
        double seconds = 0;   // its length, in real time
        QElapsedTimer clock;
        bool swapped = false;
        double fromLow = -1, toLow = -1, fromFilter = -1;   // as they were (-1: not touched)
    };

    static double get(const QString& group, const char* key) {
        return ControlObject::get(ConfigKey(group, QString::fromLatin1(key)));
    }
    static int trackId(const QString& group) {
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
        return pTrack ? pTrack->getId().toVariant().toInt() : -1;
    }
    // how fast the deck plays its track (its tempo over the track's own)
    static double speed(const QString& group) {
        const double bpm = get(group, "bpm"), file = get(group, "file_bpm");
        return bpm > 0 && file > 0 ? bpm / file : 1.0;
    }
    static ConfigKey eqLow(const QString& group) {
        return ConfigKey(QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(group), QStringLiteral("parameter1"));
    }
    static ConfigKey filter(const QString& group) {
        return ConfigKey(QStringLiteral("[QuickEffectRack1_%1]").arg(group), QStringLiteral("super1"));
    }
    // Camelot: keys that mix (the same, a step round the wheel, the relative major / minor); Mixxx's key ids 1-24
    static bool keysMix(int a, int b) {
        if (a < 1 || a > 24 || b < 1 || b > 24) {
            return true;   // (unknown: no reason not to)
        }
        const auto camelot = [](int key) {
            const bool minor = key > 12;
            const int p = (key - 1) % 12;
            return std::pair<int, bool>{(((minor ? p + 3 : p) * 7) % 12 + 1 + 6) % 12 + 1, minor};
        };
        const auto [n, am] = camelot(a);
        const auto [m, bm] = camelot(b);
        const int step = (m - n + 12) % 12;
        return am == bm ? (step == 0 || step == 1 || step == 11) : step == 0;
    }
    static bool tempoMix(const QString& from, const QString& to) {
        const double a = get(from, "bpm"), b = get(to, "file_bpm");
        double r = a > 0 && b > 0 ? b / a : 0;
        while (r > 0 && r < 0.75) {
            r *= 2;
        }
        while (r > 1.5) {
            r /= 2;
        }
        return r > 0 && std::abs(r - 1) <= 0.08;
    }

    void plan(AutoDJProcessor* p, const MixSettings& set, const QList<MixSection>& f, const QList<MixSection>& t) {
        const double fromBpm = get(m_from, "file_bpm"), toBpm = get(m_to, "file_bpm");
        const double barsSeconds = [](double bpm, int bars) { return bpm > 0 ? bars * 4 * 60.0 / bpm : bars * 2.0; }(fromBpm, set.bars);
        const double toBars = toBpm > 0 ? set.bars * 4 * 60.0 / toBpm : barsSeconds;
        // how it's mixed: near tempos and keys that mix, a bass swap; near tempos, keys that don't, a filter over half
        // the length; tempos far apart, a cut on the downbeat
        m_style = set.style;
        double length = barsSeconds, lengthIn = toBars;
        if (m_style == QLatin1String("adaptive")) {
            const bool tempo = tempoMix(m_from, m_to);
            const bool keys = keysMix(static_cast<int>(std::lround(get(m_from, "key"))), static_cast<int>(std::lround(get(m_to, "key"))));
            m_style = !tempo ? QStringLiteral("cut") : keys ? QStringLiteral("bassswap") : QStringLiteral("filter");
            if (m_style == QLatin1String("filter")) {
                length /= 2;
                lengthIn /= 2;
            }
        }
        if (m_style == QLatin1String("cut")) {
            length = lengthIn = 0;
        }
        m_length = length;
        // the exit: a section's start in the playing track
        const double duration = get(m_from, "duration");
        const double now = get(m_from, "playposition") * duration;
        QList<std::pair<double, double>> choruses;   // runs of chorus sections
        for (const MixSection& s : f) {
            if (s.label == QLatin1String("Chorus")) {
                if (!choruses.isEmpty() && std::abs(choruses.last().second - s.start) < 0.05) {
                    choruses.last().second = s.end;
                } else {
                    choruses.append({s.start, s.end});
                }
            }
        }
        QString outWhat = QStringLiteral("Mixxx's");
        m_exit = -1;
        if (set.exit == QLatin1String("outro")) {
            for (const MixSection& s : f) {
                if (s.label == QLatin1String("Outro") && m_exit < 0) {
                    m_exit = s.start;
                    outWhat = QStringLiteral("at the outro");
                }
            }
        } else if (set.exit == QLatin1String("last-chorus") && !choruses.isEmpty()) {
            m_exit = choruses.last().second;
            outWhat = QStringLiteral("after the last chorus");
        } else if (set.exit == QLatin1String("second-chorus") && choruses.size() >= 2) {
            m_exit = choruses[1].second;
            outWhat = QStringLiteral("after the second chorus");
        } else if (set.exit.startsWith(QLatin1String("minutes:"))) {
            const double after = set.exit.mid(8).toDouble() * 60;
            for (const MixSection& s : f) {
                if (s.start >= after && m_exit < 0) {
                    m_exit = s.start;
                    outWhat = QStringLiteral("after %1 min").arg(set.exit.mid(8));
                }
            }
        }
        if (m_exit >= 0 && (m_exit < now + 2 || m_exit > duration - 1)) {
            m_exit = -1;   // (passed already, or the very end: Mixxx's own)
            outWhat = QStringLiteral("Mixxx's");
        }
        // the entry: the next track cued so its section lands as the mix ends (a cut: on it)
        QString entry = set.entry;
        if (entry == QLatin1String("adaptive")) {   // the energy carried on: out of a chorus, into one; else its intro
            double energy = 0;
            const double at = m_exit >= 0 ? m_exit - 0.1 : duration - 1;
            for (const MixSection& s : f) {
                if (s.start <= at && at < s.end) {
                    energy = s.label == QLatin1String("Chorus") ? 1.0 : s.energy;
                }
            }
            entry = energy >= 0.6 ? QStringLiteral("land-chorus") : QStringLiteral("intro");
        }
        const auto first = [&t](const QString& label) {
            for (const MixSection& s : t) {
                if (s.label == label) {
                    return s.start;
                }
            }
            return -1.0;
        };
        QString inWhat = QStringLiteral("at its intro");
        double cue = -1;
        if (entry == QLatin1String("land-chorus") && first(QStringLiteral("Chorus")) >= 0) {
            cue = std::max(0.0, first(QStringLiteral("Chorus")) - lengthIn);
            inWhat = QStringLiteral("landing on its chorus");
        } else if (entry == QLatin1String("land-verse") && first(QStringLiteral("Verse")) >= 0) {
            cue = std::max(0.0, first(QStringLiteral("Verse")) - lengthIn);
            inWhat = QStringLiteral("landing on its verse");
        } else if (entry == QLatin1String("chorus") && first(QStringLiteral("Chorus")) >= 0) {
            cue = first(QStringLiteral("Chorus"));
            inWhat = QStringLiteral("at its chorus");
        }
        const double toDuration = get(m_to, "duration");
        m_cue = cue;
        if (cue >= 0 && toDuration > 0) {   // (Mixxx works its transition out again from there, as for a user's cueing)
            ControlObject::set(ConfigKey(m_to, QStringLiteral("playposition")), cue / toDuration);
        }
        // the length, for Mixxx's own transitions too (the user's put back when Auto DJ stops)
        if (m_userTime <= -1000) {
            m_userTime = p->getTransitionTime();
        }
        if (m_exit >= 0 || cue >= 0) {
            p->setTransitionTime(m_style == QLatin1String("cut") ? 0 : static_cast<int>(std::lround(m_length)));
        }
        static const QHash<QString, QString> kStyleNames{{"blend", "blend"}, {"bassswap", "bass swap"},
                {"filter", "filter out"}, {"cut", "cut on the downbeat"}};
        setPlan({{"out", outWhat}, {"outAt", m_exit}, {"in", inWhat}, {"inAt", cue},
                {"style", kStyleNames.value(m_style, m_style)}, {"bars", m_style == QLatin1String("cut") ? 0 : set.bars}});
    }

    // During the mix: the bass swapped halfway (the next track's low end held back until then), or the playing
    // track filtered out (high-pass) over it
    void automate(AutoDJProcessor::AutoDJState state) {
        if (!m_fade.active) {
            m_fade = Fade();
            m_fade.active = true;
            m_fade.from = QString::fromLatin1(state == AutoDJProcessor::ADJ_LEFT_FADING ? kD1 : kD2);   // (deck 1 on the left)
            m_fade.to = m_fade.from == QLatin1String(kD1) ? QString::fromLatin1(kD2) : QString::fromLatin1(kD1);
            m_fade.style = m_planned ? m_style : QStringLiteral("blend");
            m_fade.seconds = std::max(0.5, m_length / speed(m_fade.from));
            m_fade.clock.start();
            if (m_fade.style == QLatin1String("bassswap") && ControlObject::exists(eqLow(m_fade.to))) {
                m_fade.toLow = ControlObject::get(eqLow(m_fade.to));
                ControlObject::set(eqLow(m_fade.to), 0);
            }
            if (m_fade.style == QLatin1String("filter") && ControlObject::exists(filter(m_fade.from))) {
                m_fade.fromFilter = ControlObject::get(filter(m_fade.from));
            }
        }
        const double progress = std::min(1.0, m_fade.clock.elapsed() / 1000.0 / m_fade.seconds);
        if (m_fade.style == QLatin1String("bassswap") && progress >= 0.5 && !m_fade.swapped && m_fade.toLow >= 0) {
            m_fade.swapped = true;
            m_fade.fromLow = ControlObject::get(eqLow(m_fade.from));
            ControlObject::set(eqLow(m_fade.from), 0);
            ControlObject::set(eqLow(m_fade.to), m_fade.toLow);
        }
        if (m_fade.style == QLatin1String("filter") && m_fade.fromFilter >= 0) {   // (0.5: open; towards 1: high-pass)
            ControlObject::set(filter(m_fade.from), m_fade.fromFilter + (0.9 - m_fade.fromFilter) * progress);
        }
    }

    // The mix is over: what was changed goes back (the deck that played out is stopped by then)
    void endFade() {
        if (m_fade.toLow >= 0) {
            ControlObject::set(eqLow(m_fade.to), m_fade.toLow);
        }
        if (m_fade.fromLow >= 0) {
            ControlObject::set(eqLow(m_fade.from), m_fade.fromLow);
        }
        if (m_fade.fromFilter >= 0) {
            ControlObject::set(filter(m_fade.from), m_fade.fromFilter);
        }
        m_fade = Fade();
        m_fromId = m_toId = -1;   // (the next pair's plan)
    }

    void setPlan(const QJsonObject& plan) {
        QMutexLocker lock(&m_planMutex);
        m_plan = plan;
    }

    int m_fromId = -1, m_toId = -1;
    QString m_from, m_to;
    bool m_planned = false, m_fired = false;
    QElapsedTimer m_since;
    double m_exit = -1;     // seconds into the playing track
    double m_cue = -1;      // where the next one's cued (seconds; -1: where Mixxx cues it)
    double m_length = 0;    // the mix, seconds of the playing track
    QString m_style;
    double m_userTime = -1000;
    Fade m_fade;
    mutable QMutex m_planMutex;
    QJsonObject m_plan;
};

// Player history: what played while Auto DJ or player mode ran, newest first (the last 500, kept between sessions in
// zydeck-player-history.json): tracks player mode picked, and ones the user cut in. Written on Mixxx's main thread
// (as a track starts), read on the hub's.
struct Played {
    QMutex mutex;
    QString file;
    QList<int> ids;
    bool read = false;

    void load() {   // (mutex held)
        if (read || file.isEmpty()) {
            return;
        }
        read = true;
        QFile f(file);
        if (f.open(QIODevice::ReadOnly)) {
            for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).array()) {
                if (v.toInt() > 0) {
                    ids.append(v.toInt());
                }
            }
        }
    }
    QList<int> list() {
        QMutexLocker lock(&mutex);
        load();
        return ids;
    }
    void add(int id) {
        QMutexLocker lock(&mutex);
        load();
        if (id <= 0 || (!ids.isEmpty() && ids.first() == id)) {
            return;
        }
        ids.removeAll(id);
        ids.prepend(id);
        while (ids.size() > 500) {
            ids.removeLast();
        }
        QJsonArray out;
        for (int i : std::as_const(ids)) {
            out.append(i);
        }
        QSaveFile f(file);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(out).toJson(QJsonDocument::Compact));
            f.commit();
        }
    }
};
Played& played() {
    static Played p;
    return p;
}

// The phrase mixing runs on Mixxx's main thread (its Auto DJ's), with what it needs: it outlives the hub's objects
struct MixHolder {
    UserSettingsPointer config;
    MixPlanner planner;
    QPointer<AutoDJProcessor> processor;
    // player mode's notification (ZydekRunningService.player): what it was last told, and which deck it shows (the
    // one playing; in a mix, the one coming in)
    std::atomic<bool> playerOn{false};
    std::atomic<int> current{1};
    int playingBefore = 0;
    QString told;
    int recorded = -1;   // the track last put in the player history
};

// Player mode's state to the notification (Android), as it changes
void tellPlayer(MixHolder& h) {
    const auto get = [](int deck, const char* key) {
        return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), QString::fromLatin1(key)));
    };
    const bool p1 = get(1, "play") > 0, p2 = get(2, "play") > 0;
    if (p1 != p2) {
        h.current = p1 ? 1 : 2;
    } else if (p1 && p2 && h.playingBefore == 1) {
        h.current = h.current == 1 ? 2 : 1;   // (a mix began: the track coming in)
    }
    h.playingBefore = p1 + p2;
    const int deck = h.current;
    const bool on = h.playerOn;
    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(QStringLiteral("[Channel%1]").arg(deck));
    // the player history: a track as it starts playing, while Auto DJ or player mode runs (picked or cut in)
    if (pTrack && get(deck, "play") > 0 &&
            (on || ControlObject::get(ConfigKey(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled"))) > 0)) {
        const int id = pTrack->getId().toVariant().toInt();
        if (id != h.recorded) {
            h.recorded = id;
            played().add(id);
        }
    }
    const QString title = pTrack ? pTrack->getTitle() : QString(), artist = pTrack ? pTrack->getArtist() : QString();
    const bool playing = get(deck, "play") > 0;
    const QString state = QStringLiteral("%1|%2|%3|%4|%5").arg(on).arg(deck).arg(title, artist).arg(playing);
    if (state == h.told) {
        return;
    }
    h.told = state;
#ifdef Q_OS_ANDROID
    const double duration = get(deck, "duration");
    QJniObject::callStaticMethod<void>("org/mixxx/ZydekRunningService", "player", "(ZLjava/lang/String;Ljava/lang/String;ZJJ)V",
            static_cast<jboolean>(on), QJniObject::fromString(title).object<jstring>(), QJniObject::fromString(artist).object<jstring>(),
            static_cast<jboolean>(playing), static_cast<jlong>(get(deck, "playposition") * duration * 1000),
            static_cast<jlong>(duration * 1000));
#endif
}
std::shared_ptr<MixHolder>& mixHolder() {
    static std::shared_ptr<MixHolder> holder;
    return holder;
}

Beatmatch& beatmatch() {   // (one Auto DJ: on Mixxx's main thread, with the phrase mixing)
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
    QMutexLocker lock(&played().mutex);
    played().file = QDir(pConfig->getSettingsPath()).filePath(QStringLiteral("zydeck-player-history.json"));
}

QJsonObject AutoDj::autoplayJson() const {
    QJsonObject o = m_pool.toJson();
    o.insert(QStringLiteral("on"), m_autoplay);
    if (!m_autoplayError.isEmpty()) {
        o.insert(QStringLiteral("error"), m_autoplayError);
    }
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
            {"mix", MixSettings::read(m_pConfig).json()},
            {"plan", mixHolder() ? mixHolder()->planner.plan() : QJsonObject()},
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
    m_autoplayError.clear();
    m_queueMisses = 0;
    if (mixHolder()) {
        mixHolder()->playerOn = on;
    }
    if (!on) {
        return {{"ok", true}};
    }
    // autoplay picks: Mixxx's own top-up from the crates would add others
    m_pConfig->setValue(ConfigKey(kPrefs, QStringLiteral("EnableRandomQueue")), 0);
    topUp();
    if (m_pLibrary->autoDjQueueLength() == 0) {
        m_autoplay = false;
        if (mixHolder()) {
            mixHolder()->playerOn = false;
        }
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
    // the tempo they follow: the queue's last track's, else the playing deck's
    double after = m_pLibrary->autoDjQueueTailBpm();
    for (const QString group : {QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]")}) {
        if (after <= 0 && ControlObject::get(ConfigKey(group, QStringLiteral("play"))) > 0) {
            after = ControlObject::get(ConfigKey(group, QStringLiteral("file_bpm")));
        }
    }
    const QList<int> picked = m_pLibrary->pick(m_pool, kAhead - queued, after);
    if (picked.isEmpty()) {
        return;
    }
    for (int id : picked) {
        add(id, QStringLiteral("bottom"));
        qInfo() << "Zydek: player mode queued" << m_pLibrary->location(id);
    }
    // they stay in Mixxx's queue (until each is played), or something takes them out as they come (a track that
    // won't load): three times running, and player mode stops rather than pick on and on
    if (m_pLibrary->autoDjQueueLength() > queued) {
        m_queueMisses = 0;
        return;
    }
    qWarning() << "Zydek: player mode's tracks didn't stay in Mixxx's Auto DJ queue";
    if (++m_queueMisses >= 3) {
        m_autoplay = false;
        if (mixHolder()) {
            mixHolder()->playerOn = false;
        }
        m_autoplayError = QStringLiteral("Player mode stopped: Mixxx's Auto DJ didn't keep the tracks it was given (they wouldn't load?)");
        m_queueMisses = 0;
    }
}

QList<int> AutoDj::autoplayHistory() {
    return played().list();
}

void AutoDj::autoplayTick() {
    if (m_autoplay && ControlObject::get(ConfigKey(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled"))) > 0) {
        topUp();
    }
}


void AutoDj::startMixing() {
    if (mixHolder()) {
        return;
    }
    auto holder = std::make_shared<MixHolder>();
    holder->config = m_pConfig;
    mixHolder() = holder;
    const QString structures = QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("zydeck-structure/%1.json"));
    QMetaObject::invokeMethod(QCoreApplication::instance(), [holder, structures] {
        auto* timer = new QTimer(QCoreApplication::instance());
        timer->setInterval(250);
        QObject::connect(timer, &QTimer::timeout, timer, [holder, structures] {
            if (!holder->processor) {
                holder->processor = processor();
            }
            holder->planner.tick(holder->processor, MixSettings::read(holder->config), [structures](int id) {
                QFile f(structures.arg(id));
                return f.open(QIODevice::ReadOnly) ? mixSections(QJsonDocument::fromJson(f.readAll()).object()) : QList<MixSection>();
            }, QCoreApplication::instance());
            // the beatmatching, with where the mix starts: the planned exit, else where Mixxx's Auto DJ will start it
            double at = holder->planner.exitAt();
            if (AutoDJProcessor* p = holder->processor; at < 0 && p) {
                for (const QString g : {QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]")}) {
                    if (ControlObject::get(ConfigKey(g, QStringLiteral("play"))) > 0) {
                        at = transitionStart(g, p->getTransitionMode(), p->getTransitionTime());
                    }
                }
            }
            beatmatch().tick(holder->config->getValue(ConfigKey(kZyDeck, QStringLiteral("AutoDjBeatmatch")), 0) == 1, at);
            tellPlayer(*holder);
        });
        timer->start();
    });
}

QJsonObject AutoDj::setMix(const QJsonObject& change) {
    MixSettings::write(m_pConfig, change);
    return {{"ok", true}};
}

QJsonObject AutoDj::player(const QString& what) {
    const auto deckKey = [](int deck, const char* key) {
        return ConfigKey(QStringLiteral("[Channel%1]").arg(deck), QString::fromLatin1(key));
    };
    const bool playing = ControlObject::get(deckKey(1, "play")) > 0 || ControlObject::get(deckKey(2, "play")) > 0;
    if (what == QLatin1String("next")) {   // the mix to the next track, now
        return act(QStringLiteral("fade"));
    }
    if (what == QLatin1String("stop")) {   // player mode off, the music stopped
        setAutoplay(false, {});
        setOn(false);
        ControlObject::set(deckKey(1, "play"), 0);
        ControlObject::set(deckKey(2, "play"), 0);
        m_pausedDeck = 0;
        return {{"ok", true}};
    }
    if (what == QLatin1String("pause") || (what == QLatin1String("toggle") && playing)) {
        if (!playing) {
            return {{"ok", true}};
        }
        // Auto DJ off first: a deck paused under it, it takes the other one for the track that plays
        m_pausedDeck = mixHolder() ? mixHolder()->current.load() : 1;
        setOn(false);
        ControlObject::set(deckKey(1, "play"), 0);
        ControlObject::set(deckKey(2, "play"), 0);
        return {{"ok", true}};
    }
    // play: player mode from scratch, or the paused track again (the one coming in, if it was paused in a mix)
    if (!m_autoplay) {
        readPool();
        return setAutoplay(true, m_pool.toJson());
    }
    const int deck = m_pausedDeck ? m_pausedDeck : (mixHolder() ? mixHolder()->current.load() : 1);
    m_pausedDeck = 0;
    ControlObject::set(ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader")), deck == 1 ? -1.0 : 1.0);
    ControlObject::set(deckKey(deck, "play"), 1);
    return setOn(true);
}

} // namespace zydek
