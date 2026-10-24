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
- **ZyDeck connectivity**: the same hub as the Android app (`src/zydeck/`), so a tablet or phone talks to Mixxx
  directly: the exact track, its waveform at once, and positions timed by Mixxx's audio.
- **Stems on request**: a track's right-click menu (and ZyDeck's button) makes its stems with HT-Demucs, Open-Unmix or
  the PC stem server; the stems file joins the library with the original's cues, beat grid, BPM, key and colour, in the
  same crates and playlists. The separation itself runs in ZyDeck Connect, outside Mixxx.
- **An add-on slot**: the hub's local API, through which ZyDeck Connect's downloader (YouTube, Bilibili) adds what it
  fetches to the library. The downloader is not part of this repository.

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

Based on upstream `main` at 2086fe0 (2026-10-22, 2.7.0-alpha).
