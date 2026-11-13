#pragma once

#include <memory>

#include "preferences/usersettings.h"

class EffectsManager;

namespace zydeck {

// ZyDeck: PAD FX, as rekordbox's: 32 pads (two pages of 8, and 8 more with SHIFT on each), each a preset of one
// effect with all its parameter values, its beat length and its LEVEL/DEPTH (the unit's wet/dry mix), on while the
// pad is held. [ChannelN],zydeck_padfx_P (P = 1-32: page 1's 1-8, with SHIFT 9-16; page 2's 17-32) is the pad,
// pressed (1) and let go (0) by a skin button, a controller's mapping or a phone or tablet through ZyDeck Connect;
// it reads 1 while its effect is on.
//
// Decks 1 and 3 use effect unit 3, decks 2 and 4 unit 4 (units 1 and 2 stay the decks' own FX). A pad's effect goes
// in the unit's slot that has that effect already (so the rolls share one, only their length changing), else in one
// that isn't on, the one unused longest; its parameters, beat length, meta knob and the unit's mix are set before it
// goes on, and only this deck goes through the unit (the other deck on it, unless it's holding a pad too).
//
// The presets: zydeck-padfx.json in the settings folder, {"pads": [32 × {"effect", "backend", "name", "params":
// {parameter id: value}, "beats", "meta", "level"}]} ("effect": "" an empty pad), written by this whenever one
// changes ([ZyDeck],padfx_version goes up then). Changes come as zydeck-padfx-edit.json (ZyDeck Connect's, for a
// phone or tablet, and Mixxx ZyDeck's own editor): {"pad": P, "preset": {...}} sets one, {"pad": P, "from": {"unit":
// U, "slot": S}} saves what that slot has as it is, {"pad": P, "reset": true} puts the default back.
void setUpPadFx(const std::shared_ptr<EffectsManager>& pEffects, UserSettingsPointer pConfig);

/// What pad P (0-31) says: its preset's name, else its effect's short name and its beat length ("ROLL", "1/4"); ""
/// for an empty pad. (Mixxx ZyDeck's pads in the skin show it; [ZyDeck],padfx_version says when it changes.)
struct PadFxLabel {
    QString name;
    QString beats;
};
PadFxLabel padFxLabel(int pad);

} // namespace zydeck
