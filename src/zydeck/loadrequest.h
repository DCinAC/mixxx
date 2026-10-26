#pragma once

#include <memory>

#include "preferences/usersettings.h"

class PlayerManager;

namespace zydeck {

// ZyDeck: a track loaded into a deck when ZyDeck Connect asks (a phone or tablet's "download and load to deck N"):
// Connect writes zydeck-load.json in the settings folder, {"id", "group": "[Channel1]", "path"}; this loads that file
// with Mixxx's own loader, unless the deck is playing and Mixxx isn't set to allow that (Preferences › Decks), and
// answers in zydeck-load-done.json, {"id", "ok", "error"}. Plain Mixxx never answers: Connect says so.
void watchLoadRequests(const std::shared_ptr<PlayerManager>& pPlayerManager, UserSettingsPointer pConfig);

} // namespace zydeck
