#pragma once
// Jog wheel setup: measures a DJ controller's jog wheel - its steps in one full turn, how it counts them, how many it
// packs into one message when turned fast - so that one turn of the hand is one turn of the record in Mixxx, and
// writes a copy of the controller's Mixxx mapping that says so (Mixxx's own mapping files are left as they are).
//
// For platters without a light or a screen on them: the hand follows a mark that's already there (a logo, a sticker,
// the grip), from 12 o'clock round to 12 o'clock. Motorized platters: later.
//
// Read only on the controller: it's listened to on its own MIDI input (macOS lets Mixxx read it at the same time),
// never written to. The look: a setup wizard of the early 2000s (Qt's Modern wizard style: a white band across the
// top, a picture down the side of the first and last pages).

#include <QString>

class QWidget;

namespace zc {

/// Opens the wizard (one at a time). demo: fake jog messages, and shotDir: each page drawn into it as a PNG
/// (for checking its look without a controller)
void showJogWizard(QWidget* parent = nullptr, bool demo = false, const QString& shotDir = {});

} // namespace zc
