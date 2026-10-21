# ZyDeck Connect

The computer side of ZyDeck's controller mode: a phone or tablet running ZyDeck becomes a MIDI controller for
Mixxx on this computer (or any DJ app, with MIDI Learn: see [MIDI.md](MIDI.md)), over the USB cable or Wi-Fi.
It replaces ZyDeck Link (`tools/zydeck-link`, Python): the same protocol, and it takes over Link's key, so a
device that linked before still links.

- a MIDI port, **ZyDeck Connect**, for Mixxx (Preferences › Controllers › ZyDeck Connect › load ZyDeck)
- the cable: it watches every MIDI input for ZyDeck's hello and answers with this computer's addresses and key;
  the device then offers to go wireless
- the pairing code: ZyDeck › Settings › Connections › Pair by QR links over Wi-Fi, no cable at all
- track names from Mixxx's library, and its analysed waveforms and beat grids for the device's decks
- sets Mixxx up by itself, once per computer: it asks first, then keeps the ZyDeck mapping in Mixxx's
  controllers folder and turns it on for the ZyDeck Connect port in Mixxx's settings (`mixxx.cfg`, only while
  Mixxx is closed; backed up as `mixxx.cfg.before-zydeck`). It checks all the time that Mixxx answers through
  the port, and says when you need to quit or restart Mixxx. MAP does it all again.
- opens at login if you like (so it's always there before Mixxx); lives in the menu bar / tray

Builds come from the **ZyDeck Connect** workflow (Actions tab): macOS (universal), Windows (needs the free
[loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html) driver for its MIDI port), Linux.
By hand, with Qt 6.4 or newer:

    cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/<platform> && cmake --build build

`zydeck-connect --no-window` runs it without the window; `--install-mapping` installs the mapping and quits.
Third-party code: RtMidi (third_party/rtmidi, MIT-style licence) and Project Nayuki's QR Code generator
(third_party/qrcodegen, MIT).
