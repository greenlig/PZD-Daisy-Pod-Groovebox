// Pod Looper - settings you might want to change.
//
// Everything a player is likely to tweak lives here, so the rest of the code
// can stay untouched. Rebuild and reflash after changing anything.
#pragma once

#include <cstddef>

namespace cfg
{
// ---------------------------------------------------------------- audio --

constexpr float  kSampleRate = 48000.f;
constexpr size_t kBlockSize  = 48; // 1 ms blocks: controls and LEDs run at 1 kHz

// Four stereo layers stored as float in the Seed's 64 MB SDRAM.
// 4 layers x 2 channels x 4 bytes x 48000 x 40 s = 61.4 MB, which is about
// as much as fits. Fewer layers would allow longer loops.
constexpr int   kNumLayers      = 4;
constexpr float kMaxLoopSeconds = 40.f;

// ----------------------------------------------------------------- MIDI --

// Channel (1-16) that note and CC remote control listens on, or 0 for any
// channel. The Roland P-6 sends its sample pads on channel 11 by default.
constexpr int kControlChannel = 11;

// Pads on the P-6 send notes 48-95: bank A pad 1 = 48 ... bank H pad 6 = 95.
// Six notes starting here act as remote buttons. The default (90) is bank H,
// so leave bank H's pads empty on the P-6 and use them as looper controls:
//   +0..+3  layer 1-4: select it and press Rec/Play/Dub
//   +4      mute / unmute the selected layer
//   +5      pause / play every layer
constexpr int kPadNoteBase = 90;

// CC remote control on kControlChannel (102-119 are undefined in MIDI and
// aren't in the P-6's CC list).
constexpr int kLayerVolumeCcBase = 102; // CC 102-105 = layer 1-4 volume
constexpr int kMonitorCc         = 106; // live input level

// MIDI clock sync (the P-6 sends clock, Start and Stop on MIDI OUT).
constexpr int  kBeatsPerBar        = 4;
constexpr bool kQuantizeToBars     = true; // first loop starts/ends on a bar line
constexpr bool kFollowMidiTransport = true; // P-6 Stop pauses, Start restarts in time

} // namespace cfg
