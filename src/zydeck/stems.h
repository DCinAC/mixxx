#pragma once

#include <functional>

#include "track/track_decl.h"

class QMenu;
class TrackCollectionManager;

namespace zydeck {

// ZyDeck: stems on request. The track menu's "Make stems" asks ZyDeck Connect, on this computer, to have them made
// (its PC stem server: Connect's /web/stems); a small window follows each one. The stem file lands in the ZyDeck Web
// folder with a note of its original, and WebFolder takes it in: see takeOver().

/// The track menu's "Make stems" (for the tracks it's open on).
void addStemsAction(QMenu* pMenu, std::function<TrackPointerList()> tracks);

/// A stem file for original: its beat grid, key, cues (hot cues, loops, the main cue), colour and rating copied
/// from it, and it joins the original's crates and playlists.
void takeOver(TrackCollectionManager* pManager, const TrackPointer& pStem, const TrackPointer& pOriginal);

} // namespace zydeck
