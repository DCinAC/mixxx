#pragma once

#include "preferences/usersettings.h"

namespace zydeck {

// ZyDeck: the deck's performance pads in the skin, as rekordbox and Serato show them: 8 pads (2 rows of 4) saying
// what each does in the deck's pad mode, lit as they play, and pressed with the mouse as on the controller. Above
// them the mode button, going through the tablet's six: HOT CUE, ROLL, STEMS, SAMPLER, BEAT JUMP, PAD FX (its pages 1
// and 2); the right button goes back. A hot cue pad's right button: Mixxx's own cue menu (with Shift: deletes it).
//
// In PAD FX, SHIFT + a pad (or the right button) opens its editor (zydeck/padfx.h editPadFx).
// [ChannelN],zydeck_pad_mode is the mode (0 hot cues, 1 PAD FX 1, 2 PAD FX 2, 3 sampler, 4 stems, 5 roll, 6 beat jump), set by the row
// above the pads or by ZyDeck Connect as the DJ controller's mode changes (the Inpulse's LOOP key: PAD FX 1, SHIFT +
// LOOP: PAD FX 2); [ChannelN],zydeck_pad_shift shows the pads' SHIFT layer (Connect, as the controller's SHIFT is
// held; the keyboard's Shift does too, with the mouse).
//
// Where: a skin's WidgetGroup named ZyDeckPads_[ChannelN] (LateNight's deck row 5, in place of the hot cues, with
// [Skin],zydeck_pads): the pads go into it as it's first shown. No skin parser hook; any skin can have them.
void watchPadGrids(UserSettingsPointer pConfig);

} // namespace zydeck
