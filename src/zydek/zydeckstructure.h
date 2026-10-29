#pragma once
// ZyDeck: a track's structure, as rekordbox's phrase analysis shows it: sections on the bar grid (intro, verse, build,
// chorus, break, bridge, outro), each with a letter that repeats where the music does (A B B C ... : the chorus comes
// back as the same letter). Plain C++ (no Qt, no Mixxx), shared by the hub and ZyDeck Connect, tested by itself.
//
// How: per beat, the sound's spectrum in three ways (4 energy bands, 24 timbre bands, a 12-note chroma), averaged per
// bar; bars compared with each other (a self-similarity matrix); where the music changes (novelty along its diagonal)
// sections start, on lengths of 4, 8, 16 or 32 bars by preference; sections that sound alike bar for bar share a
// letter; the loudest letter that comes back is the chorus, a stretch without its low end is a break, and so on.
// Mixxx doesn't know which beat starts a bar (its grid's first beat is taken as one), so each of the four is tried
// and the one where the sections change most cleanly wins.

#include <atomic>
#include <string>
#include <vector>

namespace zydeck::structure {

struct Section {
    int startBeat = 0;   // index into the beats given (its first beat), and one past its last
    int endBeat = 0;
    char letter = 'A';
    std::string label;   // Intro | Verse | Build | Chorus | Break | Bridge | Outro
    double energy = 0;   // its loudness, 0 (the track's quietest section) .. 1 (its loudest)
};

struct Result {
    bool ok = false;
    std::string error;
    int bar = 0;             // beat i starts a bar when (i + bar) % 4 == 0 (as the hub's beats)
    double confidence = 0;   // how clearly the sections change where they start, 0..1 (below ~0.3: rough)
    std::vector<Section> sections;
};

/// mono: the track, one channel, at rate (about 22 kHz is plenty); beats: its beat grid in seconds (Mixxx's);
/// gridBar: the grid's own bar start, as Result::bar. cancel: stops it (Result::error "Cancelled").
Result analyse(const std::vector<float>& mono,
        double rate,
        const std::vector<double>& beats,
        int gridBar,
        const std::atomic<bool>* cancel = nullptr);

} // namespace zydeck::structure
