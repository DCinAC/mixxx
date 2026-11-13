#pragma once

#include <memory>

#include "preferences/usersettings.h"

class EffectsManager;
class QWidget;
class QPoint;

namespace zydeck {

// ZyDeck: PAD FX, as rekordbox's: 16 pads (two pages of 8; SHIFT + a pad edits it, on a screen), each a preset of
// one effect with all its parameter values, its beat length and its LEVEL/DEPTH (the unit's wet/dry mix), on while
// the pad is held. [ChannelN],zydeck_padfx_P (P = 1-16: page 1's 1-8, page 2's 9-16) is the pad,
// pressed (1) and let go (0) by a skin button, a controller's mapping or a phone or tablet through ZyDeck Connect;
// it reads 1 while its effect is on.
//
// Decks 1 and 3 use effect unit 3, decks 2 and 4 unit 4 (units 1 and 2 stay the decks' own FX). A pad's effect goes
// in the unit's slot that has that effect already (so the rolls share one, only their length changing), else in one
// that isn't on, the one unused longest; its parameters, beat length, meta knob and the unit's mix are set before it
// goes on, and only this deck goes through the unit (the other deck on it, unless it's holding a pad too).
//
// The presets: zydeck-padfx.json in the settings folder, {"version": 2, "pads": [16 × {"effect", "backend", "name", "params":
// {parameter id: value}, "beats", "meta", "level"}]} ("effect": "" an empty pad), written by this whenever one
// changes ([ZyDeck],padfx_version goes up then). Changes come as zydeck-padfx-edit.json (ZyDeck Connect's, for a
// phone or tablet, and Mixxx ZyDeck's own editor): {"pad": P, "preset": {...}} sets one, {"pad": P, "from": {"unit":
// U, "slot": S}} saves what that slot has as it is, {"pad": P, "reset": true} puts the default back.
void setUpPadFx(const std::shared_ptr<EffectsManager>& pEffects, UserSettingsPointer pConfig);

/// What pad P (0-15) says: its preset's name, else its effect's short name and its beat length ("ROLL", "1/4"); ""
/// for an empty pad. (Mixxx ZyDeck's pads in the skin show it; [ZyDeck],padfx_version says when it changes.)
struct PadFxLabel {
    QString name;
    QString beats;
};
PadFxLabel padFxLabel(int pad);

/// Pad P's (0-15) editor for the deck in group, as Mixxx's cue menu (a popup at the global point at): its name, its
/// effect and each of its parameters, its beat length, LEVEL/DEPTH and meta knob; or empty, or what the deck's FX unit
/// has now, or the default back. Kept as it's changed.
void editPadFx(int pad, const QString& group, QWidget* pParent, const QPoint& at);

// [ZyDeck],padfx_shift_edits: 1 while a phone or tablet mirrors the DJ controller (its "Mirror the physical
// controller"): the controller's SHIFT + a PAD FX pad then opens that pad's editor there (ZyDeck Connect tells it)
// and its mapping leaves the effect off; 0: SHIFT + a pad is the pad.

} // namespace zydeck
