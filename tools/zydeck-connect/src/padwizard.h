#pragma once
// A one-time helper (plain on purpose): what the Inpulse 300 MK2's pad-mode keys send, SHIFT or not, heard on deck A
// key by key, each followed by a pad (whose note block says which mode the key picked). Saved for Hardware
// (hardware.h: Hardware::learnedFile()), which then follows every mode key straight away. Read only on the controller.

class QWidget;

namespace zc {

void showPadWizard(QWidget* parent = nullptr);

} // namespace zc
