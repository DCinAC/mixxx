#include "zydeck/padfx.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
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
constexpr int kFileVersion = 2;   // (1: 32 pads, a SHIFT layer on each page: started again from the defaults)
constexpr int kSlots = 3;

struct Preset {
    QString effect;    // its id ("" an empty pad)
    QString backend;   // EffectsBackend::backendTypeToString's
    QString name;      // what the pad says ("" the effect's short name)
    QJsonObject params;   // parameter id: value (one left out: its default)
    double beats = 0;     // its knob in beats, if it has one (0: as the params say)
    double meta = -1;     // its meta knob (-1: the effect's default)
    double level = 0.5;   // the unit's mix while it's on

    QJsonObject toJson() const {
        QJsonObject o{{"effect", effect}, {"backend", backend}, {"name", name}, {"params", params}, {"level", level}};
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
            preset(roll, 0.125, 0.5, 0), preset(roll, 0.75, 0.5, 0), preset(echo, 1, 0.5), preset(phaser, 2, 0.7),
            preset(filter, 0, 1, 0.3, "LPF"), preset(filter, 0, 1, 0.7, "HPF"), preset(tremolo, 0.25, 1), preset(pump, 1, 1),
    }};
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
            static const QHash<int, QString> fractions{{625, QStringLiteral("1/16")}, {1250, QStringLiteral("1/8")},
                    {2500, QStringLiteral("1/4")}, {5000, QStringLiteral("1/2")}, {7500, QStringLiteral("3/4")}};
            const int k = static_cast<int>(std::lround(p.beats * 10000));
            l.beats = fractions.value(k, QString::number(p.beats));
        }
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

    void pressed(int deck, int pad, bool down) {
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
        if (file.value(QStringLiteral("version")).toInt() != kFileVersion) {
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
    }
    void save() {
        QJsonArray pads;
        for (const Preset& p : m_presets) {
            pads.append(p.toJson());
        }
        QSaveFile f(m_file);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(QJsonObject{{"version", kFileVersion}, {"pads", pads}}).toJson(QJsonDocument::Compact));
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
QHash<int, QPointer<QDialog>> s_editors;

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
QString beatsText(double b) {
    static const QHash<int, QString> fractions{{625, QStringLiteral("1/16")}, {1250, QStringLiteral("1/8")},
            {2500, QStringLiteral("1/4")}, {5000, QStringLiteral("1/2")}, {7500, QStringLiteral("3/4")}};
    return fractions.value(static_cast<int>(std::lround(b * 10000)), QString::number(b));
}
} // namespace

void editPadFx(int pad, QWidget* pParent) {
    if (!s_pPadFx || pad < 0 || pad >= kPads) {
        return;
    }
    if (QDialog* pOpen = s_editors.value(pad)) {
        pOpen->raise();
        pOpen->activateWindow();
        return;
    }
    PadFx* const pFx = s_pPadFx;
    auto* pDialog = new QDialog(pParent);
    s_editors.insert(pad, pDialog);
    pDialog->setAttribute(Qt::WA_DeleteOnClose);
    pDialog->setWindowTitle(QStringLiteral("PAD FX %1 · pad %2").arg(pad / 8 + 1).arg(pad % 8 + 1));
    auto* pOuter = new QVBoxLayout(pDialog);
    auto* pTop = new QFormLayout();
    auto* pName = new QLineEdit(pDialog);
    auto* pEffect = new QComboBox(pDialog);
    pTop->addRow(QObject::tr("Name"), pName);
    pTop->addRow(QObject::tr("Effect"), pEffect);
    auto* pLevel = new QSlider(Qt::Horizontal, pDialog);
    pLevel->setRange(0, 100);
    auto* pLevelText = new QLabel(pDialog);
    auto* pLevelRow = new QHBoxLayout();
    pLevelRow->addWidget(pLevel);
    pLevelRow->addWidget(pLevelText);
    pTop->addRow(QObject::tr("LEVEL/DEPTH"), pLevelRow);
    auto* pMeta = new QSlider(Qt::Horizontal, pDialog);
    pMeta->setRange(0, 100);
    pTop->addRow(QObject::tr("Intensity (meta)"), pMeta);
    pOuter->addLayout(pTop);
    auto* pParamsBox = new QWidget(pDialog);
    auto* pParams = new QFormLayout(pParamsBox);
    pParams->setContentsMargins(0, 6, 0, 6);
    pOuter->addWidget(pParamsBox);
    // what an FX unit's slot has now, as it is
    auto* pTakeRow = new QHBoxLayout();
    auto* pFrom = new QComboBox(pDialog);
    for (int u = 1; u <= 4; ++u) {
        for (int j = 1; j <= 3; ++j) {
            pFrom->addItem(QObject::tr("FX unit %1, effect %2").arg(u).arg(j), QPoint(u, j));
        }
    }
    auto* pTake = new QPushButton(QObject::tr("Take as it is"), pDialog);
    pTakeRow->addWidget(pFrom, 1);
    pTakeRow->addWidget(pTake);
    pOuter->addLayout(pTakeRow);
    auto* pButtons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, pDialog);
    pOuter->addWidget(pButtons);

    // the preset as edited
    auto pEdit = std::make_shared<Preset>(pFx->preset(pad));
    const VisibleEffectsListPointer pList = pFx->effects()->getVisibleEffectsList();
    const QList<EffectManifestPointer> manifests = pList ? pList->getList() : QList<EffectManifestPointer>();
    pEffect->addItem(QObject::tr("(empty pad)"));
    for (const EffectManifestPointer& pManifest : manifests) {
        pEffect->addItem(pManifest->displayName().isEmpty() ? pManifest->name() : pManifest->displayName());
    }
    // (filled from pEdit: its parameters' rows made again for its effect)
    auto fill = std::make_shared<std::function<void()>>();
    *fill = [=]() {
        const EffectManifestPointer pManifest = pFx->manifest(*pEdit);
        pName->setText(pEdit->name);
        pName->setPlaceholderText(pManifest ? pManifest->name() : QString());
        const QSignalBlocker block(pEffect);
        int at = 0;
        for (int i = 0; i < manifests.size(); ++i) {
            if (pManifest && *manifests[i] == *pManifest) {
                at = i + 1;
            }
        }
        pEffect->setCurrentIndex(at);
        pLevel->setValue(static_cast<int>(std::lround(pEdit->level * 100)));
        pLevelText->setText(QStringLiteral("%1 %").arg(pLevel->value()));
        pMeta->setValue(static_cast<int>(std::lround((pEdit->meta >= 0 ? pEdit->meta : pManifest ? pManifest->metaknobDefault() : 0.5) * 100)));
        pMeta->setEnabled(pManifest != nullptr);
        while (pParams->rowCount()) {
            pParams->removeRow(0);
        }
        if (!pManifest) {
            return;
        }
        int knob = 0;
        for (const EffectManifestParameterPointer& pInfo : pManifest->parameters()) {
            const QString id = pInfo->id();
            const double now = pEdit->params.value(id).toDouble(pInfo->getDefault());
            if (pInfo->parameterType() == EffectManifestParameter::ParameterType::Button) {
                auto* pBox = new QCheckBox(pParamsBox);
                pBox->setChecked(now > 0.5);
                QObject::connect(pBox, &QCheckBox::toggled, pBox, [pEdit, id](bool on) { pEdit->params.insert(id, on ? 1 : 0); });
                pParams->addRow(pInfo->name(), pBox);
                continue;
            }
            if (inBeats(pManifest, pInfo, knob++)) {   // its length in beats: as the pads say it
                auto* pBeats = new QComboBox(pParamsBox);
                const double shown = pEdit->beats > 0 ? pEdit->beats : now;
                for (const double b : {1.0 / 16, 1.0 / 8, 0.25, 0.5, 0.75, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0}) {
                    if (b >= pInfo->getMinimum() - 1e-9 && b <= pInfo->getMaximum() + 1e-9) {
                        pBeats->addItem(beatsText(b) + (b > 1 ? QObject::tr(" beats") : QObject::tr(" beat")), b);
                        if (std::abs(b - shown) < 1e-6) {
                            pBeats->setCurrentIndex(pBeats->count() - 1);
                        }
                    }
                }
                QObject::connect(pBeats, &QComboBox::currentIndexChanged, pBeats, [pEdit, pBeats, id](int) {
                    pEdit->beats = pBeats->currentData().toDouble();
                    pEdit->params.insert(id, pEdit->beats);
                });
                pParams->addRow(pInfo->name(), pBeats);
                continue;
            }
            auto* pRow = new QWidget(pParamsBox);
            auto* pRowLayout = new QHBoxLayout(pRow);
            pRowLayout->setContentsMargins(0, 0, 0, 0);
            auto* pSlider = new QSlider(Qt::Horizontal, pRow);
            pSlider->setRange(0, 1000);
            pSlider->setValue(knobPlace(pInfo, now));
            auto* pValue = new QLabel(QString::number(now, 'g', 3), pRow);
            pValue->setMinimumWidth(48);
            pRowLayout->addWidget(pSlider, 1);
            pRowLayout->addWidget(pValue);
            QObject::connect(pSlider, &QSlider::valueChanged, pSlider, [pEdit, pInfo, pValue, id](int x) {
                const double v = knobValue(pInfo, x);
                pEdit->params.insert(id, v);
                pValue->setText(QString::number(v, 'g', 3));
            });
            pParams->addRow(pInfo->name(), pRow);
        }
    };
    (*fill)();

    QObject::connect(pEffect, &QComboBox::currentIndexChanged, pDialog, [=](int i) {
        Preset p;   // (another effect: its defaults)
        p.level = pEdit->level;
        p.name = pEdit->name;
        if (i > 0 && i <= manifests.size()) {
            p.effect = manifests[i - 1]->id();
            p.backend = EffectsBackend::backendTypeToString(manifests[i - 1]->backendType());
        }
        *pEdit = p;
        (*fill)();
    });
    QObject::connect(pName, &QLineEdit::textEdited, pDialog, [pEdit](const QString& t) { pEdit->name = t.trimmed(); });
    QObject::connect(pLevel, &QSlider::valueChanged, pDialog, [pEdit, pLevelText](int v) {
        pEdit->level = v / 100.0;
        pLevelText->setText(QStringLiteral("%1 %").arg(v));
    });
    QObject::connect(pMeta, &QSlider::sliderMoved, pDialog, [pEdit](int v) { pEdit->meta = v / 100.0; });
    QObject::connect(pTake, &QPushButton::clicked, pDialog, [=] {
        const QPoint us = pFrom->currentData().toPoint();
        const Preset p = pFx->captured(us.x(), us.y() - 1);
        if (p.effect.isEmpty()) {
            return;   // (nothing in that slot)
        }
        const QString name = pEdit->name;
        *pEdit = p;
        pEdit->name = name;
        (*fill)();
    });
    QObject::connect(pButtons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, pDialog, [=] {
        *pEdit = pFx->defaultPreset(pad);
        (*fill)();
    });
    QObject::connect(pButtons, &QDialogButtonBox::accepted, pDialog, [=] {
        pFx->setPreset(pad, *pEdit);
        pDialog->close();
    });
    QObject::connect(pButtons, &QDialogButtonBox::rejected, pDialog, &QDialog::close);
    pDialog->resize(420, pDialog->sizeHint().height());
    pDialog->show();
}

void setUpPadFx(const std::shared_ptr<EffectsManager>& pEffects, UserSettingsPointer pConfig) {
    if (pEffects) {
        new PadFx(pEffects, pConfig);
    }
}

} // namespace zydeck
