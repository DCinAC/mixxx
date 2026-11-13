#include "zydeck/padfx.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QGuiApplication>
#include <QScreen>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <functional>
#include <array>
#include <cmath>
#include <vector>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "effects/backends/effectmanifest.h"
#include "effects/backends/effectmanifestparameter.h"
#include "effects/backends/effectsbackend.h"
#include "effects/backends/effectsbackendmanager.h"
#include "effects/effectchain.h"
#include "effects/effectparameterslotbase.h"
#include "effects/effectslot.h"
#include "effects/effectsmanager.h"
#include "effects/visibleeffectslist.h"

namespace zydeck {

namespace {

constexpr int kDecks = 4;
constexpr int kPads = 16;
constexpr int kFileVersion = 3;   // (1: 32 pads, a SHIFT layer on each page: the defaults again; 2: Backward Roll not yet)
constexpr int kSlots = 3;

struct Preset {
    QString effect;    // its id ("" an empty pad)
    QString backend;   // EffectsBackend::backendTypeToString's
    QString name;      // what the pad says ("" the effect's short name)
    QJsonObject params;   // parameter id: value (one left out: its default)
    double beats = 0;     // its knob in beats, if it has one (0: as the params say)
    double meta = -1;     // its meta knob (-1: the effect's default)
    double level = 0.5;   // the unit's mix while it's on
    bool release = false;   // a RELEASE FX pad: pressed, it ends the deck's other pads and plays its own instead
    bool hold = true;       // (a release pad: on while held; off: on at a press, off at the next)

    QJsonObject toJson() const {
        QJsonObject o{{"effect", effect}, {"backend", backend}, {"name", name}, {"params", params}, {"level", level}};
        if (release) {
            o.insert(QStringLiteral("release"), true);
            o.insert(QStringLiteral("hold"), hold);
        }
        if (beats > 0) {
            o.insert(QStringLiteral("beats"), beats);
        }
        if (meta >= 0) {
            o.insert(QStringLiteral("meta"), meta);
        }
        return o;
    }
    static Preset fromJson(const QJsonObject& o) {
        Preset p;
        p.effect = o.value(QStringLiteral("effect")).toString();
        p.backend = o.value(QStringLiteral("backend")).toString();
        p.name = o.value(QStringLiteral("name")).toString();
        p.params = o.value(QStringLiteral("params")).toObject();
        p.beats = o.value(QStringLiteral("beats")).toDouble(0);
        p.meta = o.value(QStringLiteral("meta")).toDouble(-1);
        p.level = std::clamp(o.value(QStringLiteral("level")).toDouble(0.5), 0.0, 1.0);
        p.release = o.value(QStringLiteral("release")).toBool();
        p.hold = o.value(QStringLiteral("hold")).toBool(true);
        return p;
    }
};

// The pads to start with: rolls that keep the track under them (level 1/2), and the usual beat FX
Preset preset(const char* id, double beats, double level, double meta = -1, const char* name = "") {
    Preset p;
    p.name = QString::fromLatin1(name);
    p.effect = QString::fromLatin1(id);
    p.backend = EffectsBackend::backendTypeToString(EffectBackendType::BuiltIn);
    p.beats = beats;
    p.level = level;
    p.meta = meta;
    return p;
}
std::array<Preset, kPads> defaults() {
    const char* roll = "org.zydeck.effects.roll";
    const char* backroll = "org.zydeck.effects.backroll";
    const char* echo = "org.mixxx.effects.echo";
    const char* reverb = "org.mixxx.effects.reverb";
    const char* flanger = "org.mixxx.effects.flanger";
    const char* phaser = "org.mixxx.effects.phaser";
    const char* filter = "org.mixxx.effects.filter";
    const char* tremolo = "org.mixxx.effects.tremolo";
    const char* pump = "org.zydeck.effects.sidechain";
    return {{
            // page 1
            preset(roll, 0.25, 0.5, 0), preset(roll, 0.5, 0.5, 0), preset(roll, 1, 0.5, 0), preset(roll, 2, 0.5, 0),
            preset(echo, 0.5, 0.5), preset(echo, 0.75, 0.5), preset(reverb, 0, 0.5), preset(flanger, 4, 0.7),
            // page 2
            preset(backroll, 0.25, 0.5, 0), preset(backroll, 0.5, 0.5, 0), preset(echo, 1, 0.5), preset(phaser, 2, 0.7),
            preset(filter, 0, 1, 0.3, "LPF"), preset(filter, 0, 1, 0.7, "HPF"), preset(tremolo, 0.25, 1), preset(pump, 1, 1),
    }};
}

// The menu PAD FX's presets are picked from (as a DJ mixer's pad FX: BEAT, COLOR, SCENE and RELEASE FX), each a
// preset made of this Mixxx's effects and ZyDeck's: so a Delay is Beat Echo without its repeats going on, HPF the
// Filter turned up. (Mixxx's own effects, as they come, are a fifth list beside them: the editors list those.)
struct Entry {
    const char* category;
    const char* name;
    Preset preset;
};
Preset with(Preset p, std::initializer_list<std::pair<const char*, double>> params, const char* name) {
    for (const auto& [k, v] : params) {
        p.params.insert(QString::fromLatin1(k), v);
    }
    p.name = QString::fromLatin1(name);
    return p;
}
Preset releasing(Preset p, bool hold = true) {
    p.release = true;
    p.hold = hold;
    return p;
}
const char* const kBrake = "org.zydeck.release.brake";       // (not effects: the deck itself, braking or spinning
const char* const kBackspin = "org.zydeck.release.backspin";  // back, as a controller's mapping would)
const std::vector<Entry>& catalog() {
    static const std::vector<Entry> entries = [] {
        const char* echo = "org.zydeck.effects.beatecho";
        const char* roll = "org.zydeck.effects.roll";
        const char* reverb = "org.mixxx.effects.reverb";
        const char* filter = "org.mixxx.effects.filter";
        return std::vector<Entry>{
                {"BEAT FX", "Delay", with(preset(echo, 0.5, 0.5), {{"feedback", 0}}, "Delay")},
                {"BEAT FX", "Echo", with(preset(echo, 0.5, 0.5), {{"feedback", 0.6}}, "Echo")},
                {"BEAT FX", "Pitch Echo", with(preset(echo, 0.5, 0.5), {{"feedback", 0.85}, {"pitch", 1}}, "Pitch Echo")},
                {"BEAT FX", "Reverb", with(preset(reverb, 0, 0.5), {}, "Reverb")},
                {"BEAT FX", "Trans", with(preset("org.mixxx.effects.tremolo", 0.25, 1), {{"depth", 1}, {"waveform", 0}}, "Trans")},
                {"BEAT FX", "Filter LFO", with(preset("org.zydeck.effects.filterlfo", 2, 1), {}, "Filter LFO")},
                {"BEAT FX", "Flanger", with(preset("org.mixxx.effects.flanger", 4, 0.7), {}, "Flanger")},
                {"BEAT FX", "Phaser", with(preset("org.mixxx.effects.phaser", 2, 0.7), {}, "Phaser")},
                {"BEAT FX", "Robot", with(preset("org.zydeck.effects.robot", 0, 1), {}, "Robot")},
                {"BEAT FX", "Slip Roll", with(preset(roll, 0.5, 1, 0), {}, "Slip Roll")},
                {"BEAT FX", "Roll", with(preset(roll, 0.5, 0.5, 0), {}, "Roll")},
                {"BEAT FX", "Backward Roll", with(preset("org.zydeck.effects.backroll", 0.5, 0.5, 0), {}, "Back Roll")},
                {"BEAT FX", "Triplet Roll", with(preset(roll, 1.0 / 3, 0.5, 0), {}, "Trip Roll")},
                {"BEAT FX", "Multi Delay", with(preset(echo, 1, 0.5), {{"feedback", 0.4}, {"taps", 3}}, "Multi Delay")},
                {"BEAT FX", "Rising Echo", with(preset(echo, 0.5, 0.5), {{"feedback", 0.7}, {"pitch", 2}}, "Up Echo")},
                {"BEAT FX", "Falling Echo", with(preset(echo, 0.5, 0.5), {{"feedback", 0.7}, {"pitch", -2}}, "Down Echo")},
                {"COLOR FX", "Crush", with(preset("org.mixxx.effects.bitcrusher", 0, 1, 0.6), {}, "Crush")},
                {"COLOR FX", "Space", with(preset(reverb, 0, 0.6, 0.7), {}, "Space")},
                {"COLOR FX", "Dub Echo", with(preset(echo, 0.75, 0.6), {{"feedback", 0.75}, {"tone", 0.35}}, "Dub Echo")},
                {"COLOR FX", "Pitch", with(preset("org.mixxx.effects.pitchshift", 0, 1, 0.7), {}, "Pitch")},
                {"COLOR FX", "HPF", with(preset(filter, 0, 1, 0.7), {}, "HPF")},
                {"COLOR FX", "LPF", with(preset(filter, 0, 1, 0.3), {}, "LPF")},
                {"COLOR FX", "Sweep", with(preset("org.zydeck.effects.sweep", 0, 1, 0.8), {}, "Sweep")},
                {"COLOR FX", "Noise", with(preset("org.mixxx.effects.whitenoise", 0, 0.5), {}, "Noise")},
                {"COLOR FX", "Gate Comp", with(preset("org.mixxx.effects.compressor", 0, 1), {}, "Gate Comp")},
                {"SCENE FX", "Band Echo", with(preset(echo, 0.5, 0.6), {{"feedback", 0.7}, {"band", 1}}, "Band Echo")},
                {"SCENE FX", "Noise Riser", with(preset("org.zydeck.effects.noiseriser", 8, 1), {}, "Noise Rise")},
                {"SCENE FX", "Pitch Rise", with(preset(echo, 0.5, 0.6), {{"feedback", 0.9}, {"glide", 12}}, "Pitch Rise")},
                {"SCENE FX", "Reverb Rise", with(preset("org.zydeck.effects.reverbrise", 8, 1), {{"pitch", 12}}, "Rev Rise")},
                {"SCENE FX", "HPF Echo", with(preset(echo, 0.5, 0.6), {{"feedback", 0.7}, {"tone", 0.5}}, "HPF Echo")},
                {"SCENE FX", "LPF Echo", with(preset(echo, 0.5, 0.6), {{"feedback", 0.7}, {"tone", -0.5}}, "LPF Echo")},
                {"SCENE FX", "Crush Echo", with(preset(echo, 0.5, 0.6), {{"feedback", 0.7}, {"crush", 0.6}}, "Crush Echo")},
                {"SCENE FX", "Pitch Fall", with(preset(echo, 0.5, 0.6), {{"feedback", 0.9}, {"glide", -12}}, "Pitch Fall")},
                {"SCENE FX", "Reverb Fall", with(preset("org.zydeck.effects.reverbrise", 8, 1), {{"pitch", -12}}, "Rev Fall")},
                {"RELEASE FX", "Brake", releasing(with(preset(kBrake, 1, 1), {}, "Brake"))},
                {"RELEASE FX", "Echo Out", releasing(with(preset(echo, 0.5, 1), {{"feedback", 0.7}, {"release", 1}}, "Echo Out"))},
                {"RELEASE FX", "Backspin", releasing(with(preset(kBackspin, 1, 1), {}, "Backspin"))},
        };
    }();
    return entries;
}

QString beatsText(double b) {   // ("1/4", "3/4", "1/3", "2")
    static const QHash<int, QString> fractions{{208, QStringLiteral("1/48")}, {625, QStringLiteral("1/16")},
            {833, QStringLiteral("1/12")}, {1250, QStringLiteral("1/8")}, {1667, QStringLiteral("1/6")},
            {2500, QStringLiteral("1/4")}, {3333, QStringLiteral("1/3")}, {5000, QStringLiteral("1/2")},
            {6667, QStringLiteral("2/3")}, {7500, QStringLiteral("3/4")}};
    return fractions.value(static_cast<int>(std::lround(b * 10000)), QString::number(b));
}

class PadFx;
PadFx* s_pPadFx = nullptr;

class PadFx : public QObject {
  public:
    PadFx(const std::shared_ptr<EffectsManager>& pEffects, UserSettingsPointer pConfig)
            : QObject(QCoreApplication::instance()),
              m_pEffects(pEffects),
              m_file(pConfig->getSettingsPath() + QStringLiteral("/zydeck-padfx.json")),
              m_edit(pConfig->getSettingsPath() + QStringLiteral("/zydeck-padfx-edit.json")),
              m_version(ConfigKey(QStringLiteral("[ZyDeck]"), QStringLiteral("padfx_version"))),
              m_shiftEdits(ConfigKey(QStringLiteral("[ZyDeck]"), QStringLiteral("padfx_shift_edits"))) {
        m_presets = defaults();
        load();
        m_clock.start();
        for (int d = 0; d < kDecks; ++d) {
            const QString group = QStringLiteral("[Channel%1]").arg(d + 1);
            for (int p = 0; p < kPads; ++p) {
                auto* pPad = new ControlPushButton(ConfigKey(group, QStringLiteral("zydeck_padfx_%1").arg(p + 1)));
                pPad->setParent(this);
                m_pads[d][p] = pPad;
                // (a controller's mapping sets it from its own thread: here it comes queued, on this one)
                connect(pPad, &ControlObject::valueChanged, this, [this, d, p](double v) { pressed(d, p, v > 0); });
            }
        }
        QFile::remove(m_edit);   // (one a previous run left: not now)
        s_pPadFx = this;
        auto* pTimer = new QTimer(this);
        pTimer->setInterval(250);
        connect(pTimer, &QTimer::timeout, this, &PadFx::takeEdit);
        pTimer->start();
    }

    PadFxLabel label(int pad) const {
        if (pad < 0 || pad >= kPads) {
            return {};
        }
        const Preset& p = m_presets[pad];
        const EffectManifestPointer pManifest = manifestOf(p);
        if (p.effect.isEmpty()) {
            return {};
        }
        PadFxLabel l;
        l.name = !p.name.isEmpty() ? p.name
                : pManifest        ? (pManifest->shortName().isEmpty() ? pManifest->name() : pManifest->shortName())
                                   : p.effect.section('.', -1);
        if (p.beats > 0) {
            l.beats = beatsText(p.beats);
        }
        l.release = p.release;
        return l;
    }

    // (the editor's)
    Preset preset(int pad) const {
        return pad >= 0 && pad < kPads ? m_presets[pad] : Preset();
    }
    void setPreset(int pad, const Preset& p) {
        if (pad >= 0 && pad < kPads) {
            m_presets[pad] = p;
            save();
        }
    }
    Preset defaultPreset(int pad) const {
        return pad >= 0 && pad < kPads ? defaults()[pad] : Preset();
    }
    Preset captured(int unit, int slot) const {
        return capture(unit, slot);
    }
    std::shared_ptr<EffectsManager> effects() const {
        return m_pEffects;
    }
    EffectManifestPointer manifest(const Preset& p) const {
        return manifestOf(p);
    }

  private:
    struct Release {
        int pad = -1;
        int kind = 0;   // 1 an effect (Echo Out), 2 Brake, 3 Backspin
        bool slipSet = false;
        double from = 1;
        double lengthMs = 500;
        QElapsedTimer clock;
        QTimer* pTimer = nullptr;
    };
    struct Held {
        bool on = false;
        int unit = 0;
        int slot = -1;
    };

    static int unitOf(int deck) {
        return deck % 2 ? 4 : 3;   // (deck index 0-3: decks 1, 3 unit 3; 2, 4 unit 4)
    }
    static QString unitGroup(int unit) {
        return QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit);
    }

    EffectManifestPointer manifestOf(const Preset& p) const {
        if (p.effect.isEmpty() || !m_pEffects) {
            return {};
        }
        const EffectBackendType type = p.backend.isEmpty() ? EffectBackendType::BuiltIn
                                                           : EffectsBackend::backendTypeFromString(p.backend);
        return m_pEffects->getBackendManager()->getManifest(p.effect, type);
    }
    EffectSlotPointer slotOf(int unit, int slot) const {
        const EffectChainPointer pChain = m_pEffects ? m_pEffects->getStandardEffectChain(unit - 1) : EffectChainPointer();
        if (!pChain || slot < 0 || slot >= pChain->getEffectSlots().size()) {
            return {};
        }
        return pChain->getEffectSlots().at(slot);
    }
    bool heldElsewhere(int unit, int slot, int notDeck, int notPad) const {
        for (int d = 0; d < kDecks; ++d) {
            for (int p = 0; p < kPads; ++p) {
                const Held& h = m_held[d][p];
                if (h.on && h.unit == unit && h.slot == slot && (d != notDeck || p != notPad)) {
                    return true;
                }
            }
        }
        return false;
    }
    bool deckHolds(int deck) const {
        for (const Held& h : m_held[deck]) {
            if (h.on) {
                return true;
            }
        }
        return false;
    }

    // The preset's values into the slot (its effect loaded first if it hasn't it): meta first (it moves the knobs
    // linked to it), then every knob and button as the preset says (or its default), then the beat length
    void apply(const EffectSlotPointer& pSlot, const EffectManifestPointer& pManifest, const Preset& p) {
        if (!pSlot->getManifest() || !(*pSlot->getManifest() == *pManifest)) {
            pSlot->loadEffectWithDefaults(pManifest);
        }
        const QString group = pSlot->getGroup();
        ControlObject::set(ConfigKey(group, QStringLiteral("meta")), p.meta >= 0 ? p.meta : pManifest->metaknobDefault());
        int beatsKnob = 0;
        for (const auto type : {EffectParameterType::Knob, EffectParameterType::Button}) {
            const unsigned int n = pSlot->numParameters(type);
            for (unsigned int k = 0; k < n; ++k) {
                const EffectParameterSlotBasePointer pParam = pSlot->getEffectParameterSlot(type, k);
                const EffectManifestParameterPointer pInfo = pParam ? pParam->getManifest() : EffectManifestParameterPointer();
                if (!pInfo) {
                    continue;
                }
                const QString key = type == EffectParameterType::Knob ? QStringLiteral("parameter%1").arg(k + 1)
                                                                      : QStringLiteral("button_parameter%1").arg(k + 1);
                ControlObject::set(ConfigKey(group, key), p.params.value(pInfo->id()).toDouble(pInfo->getDefault()));
                if (type == EffectParameterType::Knob && !beatsKnob &&
                        (pInfo->unitsHint() == EffectManifestParameter::UnitsHint::Beats ||
                                (k == 0 && pManifest->id() == QLatin1String("org.mixxx.effects.flanger")))) {
                    beatsKnob = static_cast<int>(k) + 1;
                }
            }
        }
        if (p.beats > 0 && beatsKnob) {
            ControlObject::set(ConfigKey(group, QStringLiteral("parameter%1").arg(beatsKnob)), p.beats);
        }
    }

    // A RELEASE FX pad: pressed, the deck's other pads go off and its release plays; let go (or, not held: pressed
    // again) it ends. Echo Out is an effect as the other pads' are; Brake and Backspin are the deck itself: its
    // speed taken down to nothing, or back, over the pad's beats (Mixxx's scratch speed, as a mapping's brake), slip
    // on meanwhile, so the track goes on where it would have been once it ends
    void releasePressed(int deck, int pad, bool down) {
        bool& phys = m_phys[deck][pad];
        if (phys == down) {
            return;   // (its own light being set)
        }
        phys = down;
        Release& run = m_release[deck];
        const Preset& p = m_presets[pad];
        if (!down) {
            if (run.pad == pad && p.hold) {
                endRelease(deck);
            }
            return;
        }
        if (run.pad == pad) {   // (not held: pressed again)
            endRelease(deck);
            return;
        }
        if (run.pad >= 0) {
            endRelease(deck);
        }
        for (int q = 0; q < kPads; ++q) {
            if (m_held[deck][q].on) {
                pressed(deck, q, false);
            }
        }
        run.pad = pad;
        run.kind = p.effect == QLatin1String(kBrake) ? 2 : p.effect == QLatin1String(kBackspin) ? 3 : 1;
        if (run.kind == 1) {
            m_inRelease = true;
            pressed(deck, pad, true);
            m_inRelease = false;
            return;
        }
        const QString group = QStringLiteral("[Channel%1]").arg(deck + 1);
        if (ControlObject::get(ConfigKey(group, QStringLiteral("play"))) <= 0) {
            return;   // (stopped: nothing to brake)
        }
        run.slipSet = ControlObject::get(ConfigKey(group, QStringLiteral("slip_enabled"))) <= 0;
        if (run.slipSet) {
            ControlObject::set(ConfigKey(group, QStringLiteral("slip_enabled")), 1);
        }
        const double speed = ControlObject::get(ConfigKey(group, QStringLiteral("rate_ratio")));
        run.from = run.kind == 2 ? (speed > 0 ? speed : 1) : -8;
        const double bpm = ControlObject::get(ConfigKey(group, QStringLiteral("bpm")));
        run.lengthMs = std::max(60.0, (p.beats > 0 ? p.beats : 1) * (bpm > 0 ? 60000 / bpm : 500));
        ControlObject::set(ConfigKey(group, QStringLiteral("scratch2")), run.from);
        ControlObject::set(ConfigKey(group, QStringLiteral("scratch2_enable")), 1);
        run.clock.start();
        if (!run.pTimer) {
            run.pTimer = new QTimer(this);
            run.pTimer->setInterval(5);
            run.pTimer->setTimerType(Qt::PreciseTimer);
            connect(run.pTimer, &QTimer::timeout, this, [this, deck] {
                Release& rr = m_release[deck];
                const double t = std::min(1.0, rr.clock.elapsed() / rr.lengthMs);
                const double v = rr.kind == 2 ? rr.from * (1 - t) : rr.from * (1 - t) * (1 - t);
                ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck + 1), QStringLiteral("scratch2")), v);
                if (t >= 1) {
                    rr.pTimer->stop();   // (stopped, until it's let go)
                }
            });
        }
        run.pTimer->start();
    }
    void endRelease(int deck) {
        Release& run = m_release[deck];
        const int pad = run.pad;
        if (pad < 0) {
            return;
        }
        if (run.kind == 1) {
            m_inRelease = true;
            pressed(deck, pad, false);
            m_inRelease = false;
        } else {
            const QString group = QStringLiteral("[Channel%1]").arg(deck + 1);
            if (run.pTimer) {
                run.pTimer->stop();
            }
            if (run.clock.isValid()) {
                ControlObject::set(ConfigKey(group, QStringLiteral("scratch2")), 0);
                ControlObject::set(ConfigKey(group, QStringLiteral("scratch2_enable")), 0);
                if (run.slipSet) {   // (back where the track would have been)
                    ControlObject::set(ConfigKey(group, QStringLiteral("slip_enabled")), 0);
                }
            }
            run.clock.invalidate();
        }
        run.pad = -1;
        run.kind = 0;
        run.slipSet = false;
        m_pads[deck][pad]->set(0);
    }

    void pressed(int deck, int pad, bool down) {
        if (m_presets[pad].release && !m_inRelease) {
            releasePressed(deck, pad, down);
            return;
        }
        Held& held = m_held[deck][pad];
        if (down == held.on) {
            return;
        }
        if (!down) {
            held.on = false;
            if (held.slot >= 0) {
                // another pad still on the same slot: its values back (two rolls held: the first one's length)
                int other = -1;
                for (int d = 0; d < kDecks && other < 0; ++d) {
                    for (int p = 0; p < kPads; ++p) {
                        if (m_held[d][p].on && m_held[d][p].unit == held.unit && m_held[d][p].slot == held.slot) {
                            other = p;
                            break;
                        }
                    }
                }
                const EffectSlotPointer pSlot = slotOf(held.unit, held.slot);
                if (other >= 0 && pSlot) {
                    const EffectManifestPointer pManifest = manifestOf(m_presets[other]);
                    if (pManifest) {
                        apply(pSlot, pManifest, m_presets[other]);
                        ControlObject::set(ConfigKey(unitGroup(held.unit), QStringLiteral("mix")), m_presets[other].level);
                    }
                } else if (pSlot) {
                    ControlObject::set(ConfigKey(pSlot->getGroup(), QStringLiteral("enabled")), 0);
                }
            }
            held.slot = -1;
            m_pads[deck][pad]->set(0);
            return;
        }
        const Preset& p = m_presets[pad];
        const EffectManifestPointer pManifest = manifestOf(p);
        const int unit = unitOf(deck);
        if (!pManifest) {   // an empty pad, or an effect this Mixxx hasn't: nothing on
            m_pads[deck][pad]->set(0);
            return;
        }
        // the slot that has this effect, else the one unused longest that no pad holds
        int slot = -1;
        for (int j = 0; j < kSlots; ++j) {
            const EffectSlotPointer pSlot = slotOf(unit, j);
            if (pSlot && pSlot->getManifest() && *pSlot->getManifest() == *pManifest) {
                slot = j;
                break;
            }
        }
        if (slot < 0) {
            for (int j = 0; j < kSlots; ++j) {
                if (slotOf(unit, j) && !heldElsewhere(unit, j, deck, pad) &&
                        (slot < 0 || m_used[unit - 3][j] < m_used[unit - 3][slot])) {
                    slot = j;
                }
            }
        }
        const EffectSlotPointer pSlot = slotOf(unit, slot);
        if (!pSlot) {   // (three effects held on this unit already)
            m_pads[deck][pad]->set(0);
            return;
        }
        m_used[unit - 3][slot] = m_clock.elapsed() + 1;
        held = {true, unit, slot};
        apply(pSlot, pManifest, p);
        const QString ug = unitGroup(unit);
        // only this deck through the unit: the other deck on it, unless it's holding a pad itself
        const int other = (deck + 2) % kDecks;
        if (!deckHolds(other)) {
            ControlObject::set(ConfigKey(ug, QStringLiteral("group_[Channel%1]_enable").arg(other + 1)), 0);
        }
        ControlObject::set(ConfigKey(ug, QStringLiteral("group_[Channel%1]_enable").arg(deck + 1)), 1);
        ControlObject::set(ConfigKey(ug, QStringLiteral("enabled")), 1);
        ControlObject::set(ConfigKey(ug, QStringLiteral("mix")), p.level);
        ControlObject::set(ConfigKey(pSlot->getGroup(), QStringLiteral("enabled")), 1);
        m_pads[deck][pad]->set(1);
    }

    // What a slot has, as it is: its effect, every knob and button, its meta knob, and the unit's mix
    Preset capture(int unit, int slot) const {
        Preset p;
        const EffectSlotPointer pSlot = slotOf(unit, slot);
        const EffectManifestPointer pManifest = pSlot ? pSlot->getManifest() : EffectManifestPointer();
        if (!pManifest) {
            return p;
        }
        p.effect = pManifest->id();
        p.backend = EffectsBackend::backendTypeToString(pManifest->backendType());
        const QString group = pSlot->getGroup();
        for (const auto type : {EffectParameterType::Knob, EffectParameterType::Button}) {
            const unsigned int n = pSlot->numParameters(type);
            for (unsigned int k = 0; k < n; ++k) {
                const EffectParameterSlotBasePointer pParam = pSlot->getEffectParameterSlot(type, k);
                const EffectManifestParameterPointer pInfo = pParam ? pParam->getManifest() : EffectManifestParameterPointer();
                if (!pInfo) {
                    continue;
                }
                const QString key = type == EffectParameterType::Knob ? QStringLiteral("parameter%1").arg(k + 1)
                                                                      : QStringLiteral("button_parameter%1").arg(k + 1);
                p.params.insert(pInfo->id(), ControlObject::get(ConfigKey(group, key)));
            }
        }
        p.meta = ControlObject::get(ConfigKey(group, QStringLiteral("meta")));
        p.level = ControlObject::get(ConfigKey(unitGroup(unit), QStringLiteral("mix")));
        return p;
    }

    void load() {
        QFile f(m_file);
        if (!f.open(QIODevice::ReadOnly)) {
            save();   // (the defaults: there for ZyDeck Connect to show)
            return;
        }
        const QJsonObject file = QJsonDocument::fromJson(f.readAll()).object();
        const int version = file.value(QStringLiteral("version")).toInt();
        if (version != kFileVersion && version != 2) {
            f.close();
            save();   // (an older layout: the defaults)
            return;
        }
        const QJsonArray pads = file.value(QStringLiteral("pads")).toArray();
        for (int p = 0; p < kPads && p < pads.size(); ++p) {
            if (pads[p].isObject()) {
                m_presets[p] = Preset::fromJson(pads[p].toObject());
            }
        }
        if (version == 2) {   // page 2's first two pads, as they came (rolls of 1/8 and 3/4): now Backward Roll's
            const double was[2] = {0.125, 0.75};
            for (int i = 0; i < 2; ++i) {
                const Preset& p = m_presets[8 + i];
                if (p.effect == QLatin1String("org.zydeck.effects.roll") && std::abs(p.beats - was[i]) < 1e-9 &&
                        p.name.isEmpty() && p.params.isEmpty() && std::abs(p.level - 0.5) < 1e-9) {
                    m_presets[8 + i] = defaults()[8 + i];
                }
            }
            f.close();
            save();
        }
    }
    void save() {
        QJsonArray pads;
        for (const Preset& p : m_presets) {
            pads.append(p.toJson());
        }
        QJsonArray menu;   // (the presets' menu, for a device's editor)
        for (const Entry& e : catalog()) {
            menu.append(QJsonObject{{"category", QString::fromLatin1(e.category)}, {"name", QString::fromLatin1(e.name)},
                    {"preset", e.preset.toJson()}});
        }
        QSaveFile f(m_file);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(QJsonObject{{"version", kFileVersion}, {"pads", pads}, {"menu", menu}}).toJson(QJsonDocument::Compact));
            f.commit();
        }
        m_version.set(m_version.get() + 1);
    }

    void takeEdit() {
        QFile f(m_edit);
        if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
            return;
        }
        const QJsonObject ask = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        f.remove();
        const int pad = ask.value(QStringLiteral("pad")).toInt() - 1;
        if (pad < 0 || pad >= kPads) {
            return;
        }
        if (ask.value(QStringLiteral("reset")).toBool()) {
            m_presets[pad] = defaults()[pad];
        } else if (ask.contains(QStringLiteral("from"))) {
            const QJsonObject from = ask.value(QStringLiteral("from")).toObject();
            const Preset p = capture(from.value(QStringLiteral("unit")).toInt(), from.value(QStringLiteral("slot")).toInt() - 1);
            if (p.effect.isEmpty()) {
                return;   // (nothing in that slot)
            }
            m_presets[pad] = p;
        } else if (ask.contains(QStringLiteral("preset"))) {
            m_presets[pad] = Preset::fromJson(ask.value(QStringLiteral("preset")).toObject());
        } else {
            return;
        }
        save();
    }

    std::shared_ptr<EffectsManager> m_pEffects;
    QString m_file;
    QString m_edit;
    ControlObject m_version;
    ControlObject m_shiftEdits;
    std::array<Release, kDecks> m_release;
    std::array<std::array<bool, kPads>, kDecks> m_phys{};   // (release pads: pressed or not, by whoever)
    bool m_inRelease = false;
    std::array<Preset, kPads> m_presets;
    std::array<std::array<ControlPushButton*, kPads>, kDecks> m_pads{};
    std::array<std::array<Held, kPads>, kDecks> m_held{};
    std::array<std::array<qint64, kSlots>, 2> m_used{};   // units 3 and 4: when each slot was last taken
    QElapsedTimer m_clock;
};

} // namespace

PadFxLabel padFxLabel(int pad) {
    return s_pPadFx ? s_pPadFx->label(pad) : PadFxLabel();
}

namespace {
// The pad editor: a window per pad (a second press brings it forward)
QHash<int, QPointer<QWidget>> s_editors;
UserSettingsPointer s_config;

// A knob's place (0-1000) and back, as Mixxx's knobs go: logarithmic ones by the ratio
double knobValue(const EffectManifestParameterPointer& pInfo, int x) {
    const double lo = pInfo->getMinimum(), hi = pInfo->getMaximum(), t = x / 1000.0;
    if (pInfo->valueScaler() == EffectManifestParameter::ValueScaler::Logarithmic && lo > 0 && hi > lo) {
        return lo * std::pow(hi / lo, t);
    }
    return lo + (hi - lo) * t;
}
int knobPlace(const EffectManifestParameterPointer& pInfo, double v) {
    const double lo = pInfo->getMinimum(), hi = pInfo->getMaximum();
    double t = 0;
    if (pInfo->valueScaler() == EffectManifestParameter::ValueScaler::Logarithmic && lo > 0 && hi > lo && v > 0) {
        t = std::log(v / lo) / std::log(hi / lo);
    } else if (hi > lo) {
        t = (v - lo) / (hi - lo);
    }
    return static_cast<int>(std::lround(std::clamp(t, 0.0, 1.0) * 1000));
}
bool inBeats(const EffectManifestPointer& pManifest, const EffectManifestParameterPointer& pInfo, int knob) {
    return pInfo->unitsHint() == EffectManifestParameter::UnitsHint::Beats ||
            (knob == 0 && pManifest->id() == QLatin1String("org.mixxx.effects.flanger"));
}

// Its look, as LateNight's cue menu (WCueMenuPopup, which the skin styles by its class name, so this says it again):
// the scheme's text colour, a black label field, the library's button frames; FX 3/4's teal for what's on
QString popupStyle() {
    const QString scheme = s_config && s_config->getValueString(ConfigKey(QStringLiteral("[Config]"), QStringLiteral("Scheme")))
                    .compare(QLatin1String("Classic"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("classic")
            : QStringLiteral("palemoon");
    return QStringLiteral(R"(
#PadFxPopup, #PadFxPopup QLabel, #PadFxPopup QCheckBox { color: #c2b3a5; font-family: "Open Sans"; font-size: 12px; }
#PadFxPopup QLabel#PadFxTitle { font-weight: 600; }
#PadFxPopup QLineEdit { color: #c2b3a5; background-color: #000; border: none; border-radius: 0px; padding: 2px;
  selection-color: #000; selection-background-color: #ccc; }
#PadFxPopup QPushButton, #PadFxPopup QComboBox { color: #c2b3a5; outline: none; border-width: 2px; border-radius: 0px;
  border-image: url(skins:LateNight/%1/buttons/btn_embedded_library.svg) 2 2 2 2; min-height: 20px; padding: 0 6px; }
#PadFxPopup QPushButton:pressed, #PadFxPopup QPushButton:checked {
  border-image: url(skins:LateNight/%1/buttons/btn_embedded_library_active.svg) 2 2 2 2; background-color: #146674; }
#PadFxPopup QPushButton#PadFxEmpty:hover { background-color: #6c2e2e; }
#PadFxPopup QComboBox QAbstractItemView { color: #c2b3a5; background-color: #0f0f0f; selection-background-color: #146674; border: 1px solid #000; }
#PadFxPopup QSlider::groove:horizontal { height: 4px; background: #000; }
#PadFxPopup QSlider::sub-page:horizontal { background: #146674; }
#PadFxPopup QSlider::handle:horizontal { width: 6px; margin: -5px 0; background: #c2b3a5; }
#PadFxPopup QPushButton#PadFxPick { text-align: left; }
#PadFxPopup QSpinBox { color: #c2b3a5; background-color: #000; border: none; border-radius: 0px; padding: 1px 3px;
  selection-color: #000; selection-background-color: #ccc; }
#PadFxPopup QCheckBox::indicator { width: 14px; height: 14px; background-color: #000; border: 1px solid #2a2a2a; border-radius: 0px; }
#PadFxPopup QCheckBox::indicator:checked { background-color: #146674; image: none; border-color: #257b82; }
)").arg(scheme);
}
} // namespace

namespace {
// A slider with its number beside it, 0-100 of its travel (whatever the slider's own range), typed in too
QSpinBox* numberOf(QSlider* pSlider) {
    return pSlider->property("zydeckNumber").value<QSpinBox*>();
}
void showNumber(QSlider* pSlider) {   // (after its value is set with its signals blocked)
    if (QSpinBox* pNumber = numberOf(pSlider)) {
        const QSignalBlocker block(pNumber);
        const int span = pSlider->maximum() - pSlider->minimum();
        pNumber->setValue(span > 0 ? static_cast<int>(std::lround((pSlider->value() - pSlider->minimum()) * 100.0 / span)) : 0);
    }
}
// a row's name, as wide as every other row's: the sliders under one another
QLabel* rowName(const QString& text, QWidget* pParent) {
    auto* pLabel = new QLabel(text, pParent);
    pLabel->setFixedWidth(96);
    pLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return pLabel;
}
QWidget* withNumber(QSlider* pSlider, QWidget* pParent) {
    auto* pRow = new QWidget(pParent);
    auto* pLayout = new QHBoxLayout(pRow);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(6);
    pSlider->setParent(pRow);
    pSlider->setMinimumWidth(170);
    auto* pNumber = new QSpinBox(pRow);
    pNumber->setRange(0, 100);
    pNumber->setButtonSymbols(QAbstractSpinBox::NoButtons);
    pNumber->setAlignment(Qt::AlignRight);
    pNumber->setFixedWidth(40);
    pNumber->setKeyboardTracking(false);   // (a typed number goes in as it's finished: Enter, or leaving the field)
    pNumber->setToolTip(pSlider->toolTip());
    pSlider->setProperty("zydeckNumber", QVariant::fromValue(pNumber));
    pLayout->addWidget(pSlider, 1);
    pLayout->addWidget(pNumber);
    QObject::connect(pSlider, &QSlider::valueChanged, pNumber, [pSlider] { showNumber(pSlider); });
    QObject::connect(pNumber, &QSpinBox::valueChanged, pSlider, [pSlider](int n) {
        const int span = pSlider->maximum() - pSlider->minimum();
        pSlider->setValue(pSlider->minimum() + static_cast<int>(std::lround(n * span / 100.0)));
    });
    showNumber(pSlider);
    return pRow;
}
} // namespace

// Pad P's editor, under the pad, as Mixxx's cue menu is under a hot cue: what it is (PAD FX page · pad, the effect and
// its beats), its name, its effect, each parameter, LEVEL/DEPTH and INTENSITY; on the right: empty it, take what the
// deck's FX unit has, its default. Changes are kept as they're made (as the cue menu's); a click outside closes it.
void editPadFx(int pad, const QString& group, QWidget* pParent, const QPoint& at) {
    if (!s_pPadFx || pad < 0 || pad >= kPads) {
        return;
    }
    if (QWidget* pOpen = s_editors.value(pad)) {
        pOpen->close();
    }
    PadFx* const pFx = s_pPadFx;
    auto* pPopup = new QWidget(pParent, Qt::Popup);
    s_editors.insert(pad, pPopup);
    pPopup->setAttribute(Qt::WA_DeleteOnClose);
    pPopup->setAttribute(Qt::WA_StyledBackground);
    pPopup->setObjectName(QStringLiteral("PadFxPopup"));
    pPopup->setStyleSheet(popupStyle());
    const int deck = group.mid(8, 1).toInt();
    const int unit = deck % 2 ? 1 : 2;   // (the deck's own FX unit: its Beat FX)

    auto* pTitle = new QLabel(QObject::tr("PAD FX %1 · %2").arg(pad / 8 + 1).arg(pad % 8 + 1), pPopup);
    pTitle->setObjectName(QStringLiteral("PadFxTitle"));
    auto* pWhat = new QLabel(pPopup);
    auto* pName = new QLineEdit(pPopup);
    pName->setToolTip(QObject::tr("The pad's name (empty: its effect's)"));
    // its effect: a menu of two levels, as a DJ mixer's (BEAT, COLOR, SCENE, RELEASE FX; and Mixxx's own effects)
    auto* pEffect = new QPushButton(pPopup);
    pEffect->setObjectName(QStringLiteral("PadFxPick"));
    pEffect->setToolTip(QObject::tr("Its effect"));
    auto* pHold = new QCheckBox(QObject::tr("Hold (off: a press on, the next off)"), pPopup);
    pHold->setToolTip(QObject::tr("A RELEASE FX pad: on while it's held, or on and off by presses"));
    auto* pParamsBox = new QWidget(pPopup);
    auto* pParams = new QFormLayout(pParamsBox);
    pParams->setContentsMargins(0, 2, 0, 2);
    pParams->setVerticalSpacing(3);
    const auto slider = [pPopup](const QString& tip) {
        auto* pS = new QSlider(Qt::Horizontal, pPopup);
        pS->setRange(0, 100);
        pS->setToolTip(tip);
        return pS;
    };
    auto* pLevel = slider(QObject::tr("LEVEL/DEPTH: how much of the effect against the track while the pad's held (the unit's mix)"));
    auto* pMeta = slider(QObject::tr("INTENSITY: the effect's meta knob"));
    auto* pMore = new QFormLayout();
    pMore->setVerticalSpacing(3);
    pMore->addRow(rowName(QObject::tr("LEVEL/DEPTH"), pPopup), withNumber(pLevel, pPopup));
    pMore->addRow(rowName(QObject::tr("INTENSITY"), pPopup), withNumber(pMeta, pPopup));

    auto* pEmpty = new QPushButton(QObject::tr("Empty"), pPopup);
    pEmpty->setObjectName(QStringLiteral("PadFxEmpty"));
    pEmpty->setToolTip(QObject::tr("No effect on this pad"));
    auto* pTake = new QPushButton(QObject::tr("Take FX %1").arg(unit), pPopup);
    pTake->setToolTip(QObject::tr("FX unit %1's effect (the first one on, else its first), with all its knobs and the unit's mix, as they are").arg(unit));
    auto* pDefault = new QPushButton(QObject::tr("Default"), pPopup);
    pDefault->setToolTip(QObject::tr("This pad as it came"));

    auto* pHead = new QHBoxLayout();
    pHead->addWidget(pTitle);
    pHead->addStretch(1);
    pHead->addWidget(pWhat);
    auto* pLeft = new QVBoxLayout();
    pLeft->addLayout(pHead);
    pLeft->addWidget(pName);
    pLeft->addWidget(pEffect);
    pLeft->addWidget(pHold);
    pLeft->addWidget(pParamsBox);
    pLeft->addLayout(pMore);
    auto* pRight = new QVBoxLayout();
    pRight->addWidget(pEmpty);
    pRight->addWidget(pTake);
    pRight->addStretch(1);
    pRight->addWidget(pDefault);
    auto* pMain = new QHBoxLayout(pPopup);
    pMain->addLayout(pLeft);
    pMain->addSpacing(5);
    pMain->addLayout(pRight);

    // the preset as edited, kept a moment after each change
    auto pEdit = std::make_shared<Preset>(pFx->preset(pad));
    auto* pKeep = new QTimer(pPopup);
    pKeep->setSingleShot(true);
    pKeep->setInterval(300);
    QObject::connect(pKeep, &QTimer::timeout, pPopup, [pFx, pad, pEdit] { pFx->setPreset(pad, *pEdit); });
    QObject::connect(pPopup, &QObject::destroyed, pFx, [pFx, pad, pEdit, pKeep] {
        Q_UNUSED(pKeep);
        pFx->setPreset(pad, *pEdit);   // (closed meanwhile: kept)
    });
    const auto changed = [pKeep] { pKeep->start(); };

    const VisibleEffectsListPointer pList = pFx->effects()->getVisibleEffectsList();
    const QList<EffectManifestPointer> manifests = pList ? pList->getList() : QList<EffectManifestPointer>();
    auto fill = std::make_shared<std::function<void()>>();
    auto* pMenu = new QMenu(pPopup);
    const auto pick = [=](const Preset& p) {
        const QString name = pEdit->name;
        *pEdit = p;
        if (pEdit->name.isEmpty()) {
            pEdit->name = name;
        }
        (*fill)();
        changed();
    };
    for (const char* category : {"BEAT FX", "COLOR FX", "SCENE FX", "RELEASE FX"}) {
        QMenu* pSub = pMenu->addMenu(QString::fromLatin1(category));
        for (const Entry& e : catalog()) {
            if (QLatin1String(e.category) == QLatin1String(category)) {
                const bool there = e.preset.effect.startsWith(QLatin1String("org.zydeck.release.")) || pFx->manifest(e.preset);
                QAction* pAct = pSub->addAction(QString::fromLatin1(e.name), pPopup, [=] { pick(e.preset); });
                pAct->setEnabled(there);   // (an effect this Mixxx hasn't)
            }
        }
    }
    QMenu* pMixxx = pMenu->addMenu(QObject::tr("MIXXX FX"));   // (Mixxx's own, as they come)
    for (const EffectManifestPointer& pManifest : manifests) {
        pMixxx->addAction(pManifest->displayName().isEmpty() ? pManifest->name() : pManifest->displayName(), pPopup, [=] {
            Preset p;
            p.level = pEdit->level;
            p.effect = pManifest->id();
            p.backend = EffectsBackend::backendTypeToString(pManifest->backendType());
            const QString name = pEdit->name;
            *pEdit = p;
            pEdit->name = QString();
            Q_UNUSED(name);
            (*fill)();
            changed();
        });
    }
    pMenu->addSeparator();
    pMenu->addAction(QObject::tr("Empty"), pPopup, [=] { pick(Preset()); });
    QObject::connect(pEffect, &QPushButton::clicked, pPopup, [pEffect, pMenu] {
        pMenu->popup(pEffect->mapToGlobal(QPoint(0, pEffect->height())));
    });
    *fill = [=]() {
        const EffectManifestPointer pManifest = pFx->manifest(*pEdit);
        const bool deckRelease = pEdit->effect.startsWith(QLatin1String("org.zydeck.release."));
        const QString kind = deckRelease ? pEdit->name : pManifest ? pManifest->name() : QObject::tr("empty");
        pWhat->setText(pEdit->beats > 0 && (pManifest || deckRelease) ? QStringLiteral("%1 · %2").arg(kind, beatsText(pEdit->beats)) : kind);
        pName->setText(pEdit->name);
        pName->setPlaceholderText(pManifest ? pManifest->name() : QObject::tr("Name"));
        pEffect->setText(QStringLiteral("%1%2  \u25be").arg(pEdit->release ? QStringLiteral("RELEASE · ") : QString(),
                pManifest ? pManifest->name() : deckRelease ? pEdit->name : QObject::tr("(empty)")));
        {
            const QSignalBlocker block(pHold);
            pHold->setChecked(pEdit->hold);
        }
        pHold->setVisible(pEdit->release);
        {
            const QSignalBlocker b1(pLevel), b2(pMeta);
            pLevel->setValue(static_cast<int>(std::lround(pEdit->level * 100)));
            pMeta->setValue(static_cast<int>(std::lround((pEdit->meta >= 0 ? pEdit->meta : pManifest ? pManifest->metaknobDefault() : 0.5) * 100)));
        }
        showNumber(pLevel);
        showNumber(pMeta);
        pMeta->setEnabled(pManifest != nullptr);
        while (pParams->rowCount()) {
            pParams->removeRow(0);
        }
        if (deckRelease) {   // Brake, Backspin: how many beats it takes
            auto* pBeats = new QComboBox(pParamsBox);
            for (const double b : {1.0 / 4, 0.5, 1.0, 2.0, 4.0, 8.0}) {
                pBeats->addItem(beatsText(b), b);
                if (std::abs(b - pEdit->beats) < 1e-6) {
                    pBeats->setCurrentIndex(pBeats->count() - 1);
                }
            }
            QObject::connect(pBeats, &QComboBox::currentIndexChanged, pBeats, [=](int) {
                pEdit->beats = pBeats->currentData().toDouble();
                changed();
            });
            pParams->addRow(rowName(QObject::tr("Beats"), pParamsBox), pBeats);
        }
        if (!pManifest) {
            pPopup->adjustSize();
            return;
        }
        int knob = 0;
        for (const EffectManifestParameterPointer& pInfo : pManifest->parameters()) {
            const QString id = pInfo->id();
            const double now = pEdit->params.value(id).toDouble(pInfo->getDefault());
            if (pInfo->parameterType() == EffectManifestParameter::ParameterType::Button) {
                auto* pBox = new QCheckBox(pParamsBox);
                pBox->setChecked(now > 0.5);
                QObject::connect(pBox, &QCheckBox::toggled, pBox, [pEdit, id, changed](bool on) {
                    pEdit->params.insert(id, on ? 1 : 0);
                    changed();
                });
                pParams->addRow(rowName(pInfo->name(), pParamsBox), pBox);
                continue;
            }
            if (inBeats(pManifest, pInfo, knob++)) {   // its length in beats, as the pads say it
                auto* pBeats = new QComboBox(pParamsBox);
                const double shown = pEdit->beats > 0 ? pEdit->beats : now;
                for (const double b : {1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 6, 0.25, 1.0 / 3, 0.5, 2.0 / 3, 0.75, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0}) {
                    if (b >= pInfo->getMinimum() - 1e-9 && b <= pInfo->getMaximum() + 1e-9) {
                        pBeats->addItem(beatsText(b), b);
                        if (std::abs(b - shown) < 1e-6) {
                            pBeats->setCurrentIndex(pBeats->count() - 1);
                        }
                    }
                }
                QObject::connect(pBeats, &QComboBox::currentIndexChanged, pBeats, [=](int) {
                    pEdit->beats = pBeats->currentData().toDouble();
                    pEdit->params.insert(id, pEdit->beats);
                    pWhat->setText(QStringLiteral("%1 · %2").arg(pManifest->name(), beatsText(pEdit->beats)));
                    changed();
                });
                pParams->addRow(rowName(pInfo->name(), pParamsBox), pBeats);
                continue;
            }
            auto* pS = new QSlider(Qt::Horizontal, pParamsBox);
            pS->setRange(0, 1000);
            pS->setValue(knobPlace(pInfo, now));
            pS->setToolTip(QStringLiteral("%1: %2").arg(pInfo->name(), QString::number(now, 'g', 3)));
            QObject::connect(pS, &QSlider::valueChanged, pS, [pEdit, pInfo, pS, id, changed](int x) {
                const double v = knobValue(pInfo, x);
                pEdit->params.insert(id, v);
                pS->setToolTip(QStringLiteral("%1: %2").arg(pInfo->name(), QString::number(v, 'g', 3)));
                changed();
            });
            pParams->addRow(rowName(pInfo->name(), pParamsBox), withNumber(pS, pParamsBox));
        }
        pPopup->adjustSize();
    };
    (*fill)();

    QObject::connect(pHold, &QCheckBox::toggled, pPopup, [pEdit, changed](bool on) {
        pEdit->hold = on;
        changed();
    });
    QObject::connect(pName, &QLineEdit::textEdited, pPopup, [pEdit, changed](const QString& t) {
        pEdit->name = t.trimmed();
        changed();
    });
    QObject::connect(pName, &QLineEdit::returnPressed, pPopup, &QWidget::close);
    QObject::connect(pLevel, &QSlider::valueChanged, pPopup, [pEdit, changed](int v) {
        pEdit->level = v / 100.0;
        changed();
    });
    QObject::connect(pMeta, &QSlider::valueChanged, pPopup, [pEdit, changed](int v) {
        pEdit->meta = v / 100.0;
        changed();
    });
    QObject::connect(pEmpty, &QPushButton::clicked, pPopup, [=] {
        *pEdit = Preset();
        (*fill)();
        changed();
    });
    QObject::connect(pTake, &QPushButton::clicked, pPopup, [=] {
        int slot = 0;   // (the first effect on, else the first)
        for (int j = 2; j >= 0; --j) {
            if (ControlObject::get(ConfigKey(QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]").arg(unit).arg(j + 1), QStringLiteral("enabled"))) > 0) {
                slot = j;
            }
        }
        const Preset p = pFx->captured(unit, slot);
        if (p.effect.isEmpty()) {
            return;
        }
        const QString name = pEdit->name;
        *pEdit = p;
        pEdit->name = name;
        (*fill)();
        changed();
    });
    QObject::connect(pDefault, &QPushButton::clicked, pPopup, [=] {
        *pEdit = pFx->defaultPreset(pad);
        (*fill)();
        changed();
    });
    pPopup->adjustSize();
    QPoint topLeft = at;
    if (const QScreen* pScreen = QGuiApplication::screenAt(at)) {   // (on the screen, as Mixxx's popups)
        const QRect area = pScreen->availableGeometry();
        topLeft.setX(std::clamp(topLeft.x(), area.left(), area.right() - pPopup->width()));
        topLeft.setY(topLeft.y() + pPopup->height() > area.bottom() ? at.y() - pPopup->height() - 60 : topLeft.y());
    }
    pPopup->move(topLeft);
    pPopup->show();
    pName->setFocus();
}

void setUpPadFx(const std::shared_ptr<EffectsManager>& pEffects, UserSettingsPointer pConfig) {
    s_config = pConfig;
    if (pEffects) {
        new PadFx(pEffects, pConfig);
    }
}

} // namespace zydeck
