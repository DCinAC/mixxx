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
- **PAD FX**: rekordbox's Pad FX: 16 pads (two pages of 8), each a preset of one effect with every parameter, its beat
  length, LEVEL/DEPTH (wet/dry) and meta knob, on while held, on effect units 3 (decks 1, 3) and 4 (decks 2, 4).
  `[ChannelN],zydeck_padfx_P` is the pad (the skin's pads, a controller's mapping, a phone or tablet through Connect);
  presets in `zydeck-padfx.json`, edited in Mixxx (SHIFT + a pad, or right-click it: a popup like the cue menu) or from a device
  (`zydeck-padfx-edit.json`). `src/zydeck/padfx.*`.
- **The deck's pads**: in LateNight, in place of the hot cues and twice as tall (the deck gear menu's "ZyDeck Pads"):
  8 pads for the deck's pad mode (one button goes through HOT CUE, ROLL, STEMS, SAMPLER, BEAT JUMP, PAD FX 1/2; a hot
  cue's right button opens Mixxx's own cue menu), as rekordbox and Serato show them, following
  the DJ controller's mode through Connect (`[ChannelN],zydeck_pad_mode`, `zydeck_pad_shift`). A ZyDeck widget put in
  a skin group named `ZyDeckPads_[ChannelN]` as it's shown, so no skin parser hook. `src/zydeck/padgrid.*`.
- **Intro and outro from the phrases**: a track whose first section is an intro, or last an outro, gets those cues
  from them (each only where the track has none yet).
- **Roll**: a beat roll as an effect (ZyDeck's PAD FX): it records the next 1/16 to 4 beats as it's switched on and
  repeats them, the track still playing underneath as the unit's MIX says (Mixxx's own roll takes the deck over);
  and Backward Roll, its repeats played backwards. `src/zydeck/rolleffect.*`.
- **PAD FX's effects**: Beat Echo (repeats that can rise or fall in pitch, glide while held, tap three times, be
  filtered or crushed, or ring out alone), Filter LFO, Sweep (a beat gate one way, a band sweep the other), Robot,
  Reverb Rise (and fall) and Noise Riser. `src/zydeck/zydeckfx.*`. PAD FX's presets are picked from a menu of two
  levels, as a DJ mixer's: BEAT, COLOR, SCENE and RELEASE FX (presets of these and Mixxx's effects), and Mixxx's own
  effects; RELEASE FX pads end the deck's other pads and play Echo Out, or Brake or Backspin (the deck's own scratch
  speed, slip on meanwhile).
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
- **The slip position, for ZyDeck**: `[ChannelN],zydeck_slip_seconds`, so a phone or tablet draws the slip ghost as
  Mixxx does (Mixxx keeps that position only for its waveforms). Read only. `src/zydeck/slipposition.*`.
- **Phrases**: the sections ZyDeck Connect works out (as rekordbox's phrase analysis), drawn as ZyDeck's pages draw
  them: a strip in each section's colour along the bottom of the overview and the scrolling waveform, and a tag where
  one starts ("B Chorus"; the overview: its letter). Only on the GL waveforms (`allshader`). `src/zydeck/phrases.*`.
- **Bars and time left**: on the scrolling waveform as on ZyDeck's pages: just left of the play cursor the bars left in
  the track (-8.3 bars: bars.beats), at the lower left the section the deck is in and the bars left in it (with
  phrases), at the upper right the time left in seven segments. Only on the GL waveforms; off with "Bars & Time Left"
  in LateNight's skin menu (`[Skin],zydeck_show_waveform_counters`). `src/zydeck/phrases.*`.
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
| `src/effects/backends/builtin/builtinbackend.cpp` | registers ZyDeck's effects (`src/zydeck/sidechaineffect.*`, `rolleffect.*`, `zydeckfx.*`, the same as ZyDeck on Android) |
| `src/engine/readaheadmanager.cpp`, `src/engine/cachingreader/cachingreader.cpp` | read further ahead, with a bigger cache (network music folders) |
| `src/main.cpp` | macOS: raises the open-file limit from 256 (Mixxx's ~100 threads hold ~200 of them as Qt wake-up pipes; analysing a new stem file ran out, and Qt aborted) |
| `src/widget/woverview.cpp`, `src/waveform/widgets/allshader/waveformwidget.cpp` | add the phrases to the overview and the scrolling waveform, and the bars and time left (`src/zydeck/phrases.*`) |
| `res/skins/LateNight/skin.xml`, `skin_settings.xml` | the Bars & Time Left switch, and ZyDeck Pads' (`[Skin],zydeck_pads`) |
| `res/skins/LateNight/decks/row_5_transportLoopJump.xml`, `helpers/skin_settings_full_deck.xml` | the deck's pads in place of the hot cues, and their switch in the deck gear menu |
| `res/skins/LateNight/decks/deck_mini.xml`, `style_palemoon.qss` | the key above the big library: PaleMoon drew it blank (no box rule for WKey to draw in), and room for all of it |
| `src/coreservices.cpp` | writes the effects list and the decks' tracks for ZyDeck Connect once effects are set up (`src/zydeck/effectlist.*`, `decklist.*`), sets up PAD FX and the decks' pads (`padfx.*`, `padgrid.*`), loads a track into a deck, or previews it, when Connect asks (a device's library page and "download and load to deck": `loadrequest.*`), runs Mixxx's Auto DJ as a device asks through Connect (`autodjrequest.*`), and sets up the Live Broadcasting connection to the ZyDeck Discord bot that Connect hands over (`discordbroadcast.*`) |

Based on upstream `main` at 64c1cbec91 (2026-11-06, 2.7.0-alpha).
