#pragma once

#include <memory>

#include "preferences/usersettings.h"

class Library;

namespace zydeck {

// ZyDeck: Mixxx's Auto DJ driven from a phone or tablet in controller mode (its library page), through ZyDeck Connect.
// Connect drops each request in the settings folder as zydeck-autodj-<n>.json, {"id", "do", ...}:
//   on | off | fade (now) | skip (the next track) | skiptrack (the playing one, gracefully: to 16 beats before its
//   transition) | random (one random track) | shuffle | clear
//   transition {seconds, mode: intro-outro | outro-start | full-track | skip-silence | skip-silence-full}
//   options {random: 0|1, min, requeue: 0|1 (Preferences › Auto DJ's), beatmatch: 0|1 (synced transitions, when the
//   tempos are near: its Beatmatch)}   source {crate, on: 0|1}
//   add {track, where: bottom | top | replace}   remove {track}
//   mix {entry, exit, bars, style}: the phrase mixing (its MixPlanner: where the next track comes in and the playing
//   one goes out, by their sections, which ZyDeck Connect works out into zydeck-structure/<id>.json here)
// This carries them out as Mixxx's own Auto DJ view would (its processor, the queue's playlist, the crates), in order,
// and keeps zydeck-autodj-state.json up to date: {on, state: off | starting | playing | fading, transition, random,
// requeue, beatmatch, mix, plan (the next transition), done: [{id, ok, error}] (the last few)}. Connect reads the queue
// and crates from the database itself.
void watchAutoDj(const std::shared_ptr<Library>& pLibrary, UserSettingsPointer pConfig);

} // namespace zydeck
