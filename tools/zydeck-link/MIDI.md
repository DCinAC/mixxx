# ZyDeck controller mode: the Standard MIDI chart

For Serato DJ Pro, rekordbox, Traktor and other DJ apps (MIDI Learn). Choose **Standard MIDI** under
Settings › Controller mode. Over the USB cable the phone or tablet is a MIDI device named after it; over
Wi-Fi (ZyDeck Link) it's the **ZyDeck Link** port.

Buttons are notes: velocity 127 when pressed, 0 when let go. Play, sync, keylock, quantize, headphone cue,
mute and eject are toggles in the DJ app: ZyDeck sends one press and release each time.

## Decks: MIDI channels 1–4

| Note | Control | | Note | Control |
|---|---|---|---|---|
| 0x00 (0) | Play / pause | | 0x10–0x18 | Beat loop ⅛, ¼, ½, 1, 2, 4, 8, 16, 32 |
| 0x01 (1) | Cue (held) | | 0x19 | Reloop |
| 0x02 (2) | Sync | | 0x1A / 0x1B | Loop half / double |
| 0x03 (3) | Key lock | | 0x1C / 0x1D | Loop in / out |
| 0x04 (4) | Quantize | | 0x1E / 0x1F | Move the loop back / forward |
| 0x05 (5) | Headphone cue | | 0x78 / 0x79 | Key −1 / +1 |
| 0x08 (8) | Jog wheel touch (scratch) | | 0x7A / 0x7B | Key reset / key sync |
| 0x09 (9) | Sync as the master | | | |
| 0x0A (10) | Mute | | | |
| 0x0B (11) | Eject | | | |

Pads: note 0x40 + mode × 8 + pad (pads 1–8 = 0–7):

| Notes | Pad mode |
|---|---|
| 0x40–0x47 | Hot cues 1–8 |
| 0x48–0x4F | Hot cues 1–8 with SHIFT (clear) |
| 0x50–0x57 | Loop roll ⅛ … 16 |
| 0x58–0x5B | Stems: vocals, other, bass, drums on/off |
| 0x60–0x67 | Beat jump −1, +1, −2, +2, −4, +4, −8, +8 |
| 0x68–0x6F | FX pads; 0x70–0x77 FX pads with SHIFT |
| 0x78–0x7A | Key pads (also above) |

| CC | Control |
|---|---|
| 0x00 | Volume fader (0–127) |
| 0x01 | Gain (64 = centre) |
| 0x02 / 0x03 / 0x04 | EQ high / mid / low (64 = centre) |
| 0x05 | Filter (64 = centre) |
| 0x06 | Jog wheel, scratching: relative, 64 = still, 65+ forward, 63− back |
| 0x07 | Nudge: relative bend, 64 = none |
| Pitch bend | Tempo: centre = 0 %, the ends = ±8 % |

## Mixer and samplers: MIDI channel 16

| Message | Control |
|---|---|
| CC 0x00 | Crossfader (0 = left, 127 = right) |
| CC 0x01 | Main level |
| CC 0x02 | Headphone level |
| CC 0x03 | Headphone mix (cue ↔ main) |
| Notes 0x00–0x3F | Sampler pads 1–64 |
| Notes 0x40–0x7F | Stop sampler 1–64 |

## Feedback (LEDs)

If the DJ app sends LED notes back on the same numbers, ZyDeck lights up with them: play, sync, key lock,
quantize and headphone cue (notes 0–5), hot cue pads (0x40–0x47) and sampler pads (channel 16). These apps
don't send play positions, BPMs or waveforms over MIDI, so the decks can't show those.
