#pragma once

namespace zydeck {

// ZyDeck: a track that loads from a network drive (a NAS) is read once, whole, in the background, so macOS (or
// Windows) keeps it in memory and the deck's reads during the set don't wait on the network. Mixxx's own reading is
// unchanged; besides it, the decks read ~6 s ahead (readaheadmanager.cpp). Local files are left alone.
void prefetchNetworkTracks();

} // namespace zydeck
