#include "zydeck/effectlist.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "effects/backends/effectmanifest.h"
#include "effects/effectsmanager.h"
#include "effects/visibleeffectslist.h"

namespace zydeck {

namespace {
void write(const VisibleEffectsList& list, const QString& path) {
    QJsonArray effects;
    for (const EffectManifestPointer& pManifest : list.getList()) {
        QJsonObject e{{"id", pManifest->id()},
                {"name", pManifest->displayName().isEmpty() ? pManifest->name() : pManifest->displayName()}};
        // its knob in beats, if it has one (a slot's parameterN, N counting knobs only): ZyDeck's BEAT keys set it
        int knob = 0;
        for (const EffectManifestParameterPointer& pParam : pManifest->parameters()) {
            if (pParam->parameterType() != EffectManifestParameter::ParameterType::Knob) {
                continue;
            }
            ++knob;
            // (Flanger's speed is in beats too, 1/4 to 32 a sweep with a grid, though Mixxx doesn't say so)
            if (pParam->unitsHint() == EffectManifestParameter::UnitsHint::Beats ||
                    (knob == 1 && pManifest->id() == QLatin1String("org.mixxx.effects.flanger"))) {
                e.insert(QStringLiteral("beats"), QJsonObject{{"knob", knob}, {"min", pParam->getMinimum()}, {"max", pParam->getMaximum()}});
                break;
            }
        }
        effects.append(e);
    }
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{"effects", effects}}).toJson(QJsonDocument::Compact));
        f.commit();
    }
}
} // namespace

void watchEffectList(const std::shared_ptr<EffectsManager>& pEffects, UserSettingsPointer pConfig) {
    const VisibleEffectsListPointer pList = pEffects ? pEffects->getVisibleEffectsList() : VisibleEffectsListPointer();
    if (!pList) {
        return;
    }
    const QString path = pConfig->getSettingsPath() + QStringLiteral("/zydeck-effects.json");
    VisibleEffectsList* const pRaw = pList.data();
    write(*pRaw, path);
    QObject::connect(pRaw, &VisibleEffectsList::visibleEffectsListChanged, pRaw, [pRaw, path] { write(*pRaw, path); });
}

} // namespace zydeck
