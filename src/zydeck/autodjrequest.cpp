#include "zydeck/autodjrequest.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <functional>
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
#include "mixer/playerinfo.h"
#include "track/track.h"

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
    MixPlanner mix;

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
        if (what == QLatin1String("mix")) {   // the phrase mixing's choices (MixSettings)
            MixSettings::write(config, ask);
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
                {"mix", MixSettings::read(config).json()},
                {"plan", mix.plan()},
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
        // the tracks' sections, as ZyDeck Connect works them out (zydeck-structure/<id>.json here)
        const QString structures = QDir(dir).filePath(QStringLiteral("zydeck-structure/%1.json"));
        mix.tick(autoDj(), MixSettings::read(config), [structures](int id) {
            QFile f(structures.arg(id));
            return f.open(QIODevice::ReadOnly) ? mixSections(QJsonDocument::fromJson(f.readAll()).object()) : QList<MixSection>();
        }, QCoreApplication::instance());
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
