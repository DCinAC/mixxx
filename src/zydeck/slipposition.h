#pragma once

namespace zydeck {

// ZyDeck: [ChannelN],zydeck_slip_seconds: where the deck's slip position is (s), 0 while it isn't slipping. Mixxx keeps
// that position only for its waveforms; as a control, the mapping can send it to a phone or tablet (Zydek-Tablet's
// value subscriptions), which draws the same slip ghost. Read only: nothing here touches the engine.
void publishSlipPositions();

} // namespace zydeck
