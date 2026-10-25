#pragma once

#include "preferences/usersettings.h"

namespace zydeck {

// ZyDeck: what each deck and sampler holds, {"[Channel1]": {id, title, artist, seconds} or null, ...}, written to
// zydeck-decks.json in the settings folder whenever one loads or ejects a track. ZyDeck Connect reads it, so a phone
// or tablet driving this Mixxx names the track that's really there (plain Mixxx only tells it the track's length).
void watchDecks(UserSettingsPointer pConfig);

} // namespace zydeck
