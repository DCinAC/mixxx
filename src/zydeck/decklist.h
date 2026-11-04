#pragma once

#include "preferences/usersettings.h"

class TrackCollectionManager;

namespace zydeck {

// ZyDeck: what each deck and sampler holds, {"[Channel1]": {id, title, artist, seconds} or null, ...}, written to
// zydeck-decks.json in the settings folder whenever one loads or ejects a track. ZyDeck Connect reads it, so a phone
// or tablet driving this Mixxx names the track that's really there (plain Mixxx only tells it the track's length).
// A loaded track's beat grid, as it changes (the grid edited, or analysed after loading), is saved to the library
// 1.5 s after the last change (Mixxx itself saves it only as the track leaves the decks), and its entry's "grid"
// changes then: ZyDeck Connect reads the grid from the library, so the devices get the new one, and its sections.
void watchDecks(UserSettingsPointer pConfig);
// the library those grids are saved to (it's made after the decks)
void saveGridsTo(TrackCollectionManager* pTracks);

} // namespace zydeck
