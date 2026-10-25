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
        effects.append(QJsonObject{{"id", pManifest->id()},
                {"name", pManifest->displayName().isEmpty() ? pManifest->name() : pManifest->displayName()}});
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
