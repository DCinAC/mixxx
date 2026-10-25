#pragma once

#include <memory>

#include "preferences/usersettings.h"

class EffectsManager;

namespace zydeck {

// ZyDeck: the effects a unit's slots can load, in Mixxx's order (Preferences › Effects: the visible ones; a slot's
// loaded_effect is a place in that list), written to zydeck-effects.json in the settings folder at start and whenever
// the list changes. ZyDeck Connect reads it, so a phone or tablet driving this Mixxx shows the effects by name
// (without it, Connect reads effects.xml, which Mixxx only writes when it quits).
void watchEffectList(const std::shared_ptr<EffectsManager>& pEffects, UserSettingsPointer pConfig);

} // namespace zydeck
