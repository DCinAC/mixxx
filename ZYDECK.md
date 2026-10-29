# ZyDeck's desktop Mixxx fork

Mixxx (upstream `main`, mixxxdj/mixxx) with what ZyDeck needs on a PC or Mac, kept so that upstream can be merged in
at any time. The Android app (branch `stability`) is separate and stays as it is.

## What it adds

- **Fits deck**: ZyDeck's "what fits next" in the library's sidebar: under "Fits deck", each deck that has a track; a
  click lists the tracks that mix with it now (within 6 % of its tempo, or half or double it, in a key next to its
  key as it sounds now; the same key first, then the nearest tempo). `src/zydeck/fitsfeature.*`, `fitstablemodel.*`.
- **ZyDeck Web**: the downloads folder (ZyDeck Connect's YouTube and Bilibili downloads; `[ZyDeck] WebFolder`, by
  default "ZyDeck Web" in the first music folder) is watched: a finished file that lands there joins the library at
  once, in the crate "ZyDeck Web", and is analysed. No rescan, and no connection to Connect needed (the sandbox allows
  no listening socket). `src/zydeck/webfolder.*`.
- **Stems on request**: the track menu's "Make Stems (ZyDeck)" asks ZyDeck Connect (on this computer, which answers
  it without a key) to have them made on the PC's stem server; a small window follows each. The stem file lands in
  a "stem" folder in the original's own folder when that's on a network drive (the NAS), else in ZyDeck Web; Connect's
  note in ZyDeck Web names both. It takes over the original's beat grid, key, cues, colour, rating, crates and
  playlists, and the original's comment starts with "stem available". `src/zydeck/stems.*`, `src/zydeck/webfolder.*`.
- **Sidechain**: the beat pump effect from ZyDeck on Android (the sound drops on each beat, or every 1/2 or 1/4 beat,
  and swells back, locked to the beat grid), so both have the same effects.
- **The effects list for ZyDeck**: `zydeck-effects.json` in the settings folder, Mixxx's visible effects in order,
  kept up to date; ZyDeck Connect hands it to a phone or tablet so its FX menu shows this Mixxx's effects by name.
- **Network music folders without dropouts**: each deck reads about 6 s ahead of the play head instead of ~0.4 s
  (and keeps 16 MB of it instead of 5 MB), so a NAS that stalls for a moment doesn't cut the sound. The audio
  buffer, and with it the latency, stays as set. And a track that loads from a network drive is read whole once, in
  the background, so the computer keeps it in memory and the deck's reads during the set don't wait on the network.
- **What's on the decks, for ZyDeck**: `zydeck-decks.json` beside it, each deck's and sampler's track (id, title,
  artist, length, hot cue names) as it loads and as its hot cues change, so ZyDeck Connect names the track a phone or
  tablet sees, and its hot cues, by what's really there.
- **ZyDeck connectivity** (to come): the same hub as the Android app, so a tablet or phone talks to Mixxx directly:
  the exact track, its waveform at once, and positions timed by Mixxx's audio. Downloading and separating stay in
  ZyDeck Connect, not in this repository.
- Still to come: HT-Demucs on this computer (ONNX, upstream's epic #15495) beside the PC stem server.

## How it stays mergeable

- Our code lives in our own files (`src/zydeck/`, `res/zydeck/`, `cmake/zydeck.cmake`), built with the `ZYDECK` CMake
  option (on in this fork; `-DZYDECK=OFF` builds plain Mixxx); upstream's files get small hooks, each marked
  `// ZyDeck` or `# ZyDeck`, inside `#ifdef __ZYDECK__`.
- Where upstream has a plan, we follow it: stems separation as in epic #15495 (HTDemucs ONNX: `separation_trigger`,
  `separation_percentage`, `stem_count`), stems kept as NI STEM files (`.stem.mp4`) that Mixxx already plays.
- `zydeck-desktop.yml` builds macOS arm64 and Windows x64 on every push. It is upstream's `build.yml` with its matrix
  cut to those two and its publishing removed; when upstream changes `build.yml`, make it again the same way.
  `develop.yml` skips this branch (one line).
- Upstream is merged about once a month (`git merge upstream/main`); conflicts can only come from the files below.

## Upstream files we change

| File | Why |
| --- | --- |
| `.github/workflows/develop.yml` | skip this branch (`zydeck-desktop.yml` builds it) |
| `CMakeLists.txt` | the `ZYDECK` option, including `cmake/zydeck.cmake` (before Ebur128) |
| `src/library/library.cpp` | adds the Fits deck feature after Auto DJ and starts ZyDeck Web (and their includes) |
| `src/widget/wtrackmenu.cpp` | "Make Stems (ZyDeck)" after the Analyze menu (and its include) |
| `src/effects/backends/builtin/builtinbackend.cpp` | registers the Sidechain effect (`src/zydeck/sidechaineffect.*`, the same as ZyDeck on Android) |
| `src/engine/readaheadmanager.cpp`, `src/engine/cachingreader/cachingreader.cpp` | read further ahead, with a bigger cache (network music folders) |
| `src/coreservices.cpp` | writes the effects list and the decks' tracks for ZyDeck Connect once effects are set up (`src/zydeck/effectlist.*`, `decklist.*`), loads a track into a deck, or previews it, when Connect asks (a device's library page and "download and load to deck": `loadrequest.*`), runs Mixxx's Auto DJ as a device asks through Connect (`autodjrequest.*`), and sets up the Live Broadcasting connection to the ZyDeck Discord bot that Connect hands over (`discordbroadcast.*`) |

Based on upstream `main` at 2086fe0 (2026-10-22, 2.7.0-alpha).
