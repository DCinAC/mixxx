# ZyDeck's desktop Mixxx fork

Mixxx (upstream `main`, mixxxdj/mixxx) with what ZyDeck needs on a PC or Mac, kept so that upstream can be merged in
at any time. The Android app (branch `stability`) is separate and stays as it is.

## What it adds

- **ZyDeck connectivity**: the same hub as the Android app (`src/zydeck/`), so a tablet or phone talks to Mixxx
  directly: the exact track, its waveform at once, and positions timed by Mixxx's audio.
- **Stems on request**: a track's right-click menu (and ZyDeck's button) makes its stems with HT-Demucs, Open-Unmix or
  the PC stem server; the stems file joins the library with the original's cues, beat grid, BPM, key and colour, in the
  same crates and playlists. The separation itself runs in ZyDeck Connect, outside Mixxx.
- **An add-on slot**: the hub's local API, through which ZyDeck Connect's downloader (YouTube, Bilibili) adds what it
  fetches to the library. The downloader is not part of this repository.

## How it stays mergeable

- Our code lives in our own files (`src/zydeck/`, `res/zydeck/`, `cmake/zydeck.cmake`), built only with
  `-DZYDECK=ON`; upstream's files get small hooks, each marked `// ZyDeck`.
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

Based on upstream `main` at 2086fe0 (2026-10-22, 2.7.0-alpha).
