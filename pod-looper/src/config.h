// Pod Looper - settings you might want to change.
//
// Everything a player is likely to tweak lives here, so the rest of the code
// can stay untouched. Rebuild and reflash after changing anything.
#pragma once

#include <cstddef>

#include "looper.h"

namespace cfg
{
// ---------------------------------------------------------------- audio --

constexpr float  kSampleRate = 48000.f;
constexpr size_t kBlockSize  = 48; // 1 ms blocks: controls and LEDs run at 1 kHz

// Four stereo tracks plus one undo buffer, stored as 16-bit in the Seed's
// 64 MB SDRAM: 5 x 2 channels x 2 bytes x 48000 x 40 s = 38.4 MB, leaving
// room for samples later. (40 s is 20 bars at 120 BPM.)
constexpr int   kNumTracks      = 4;
constexpr float kMaxLoopSeconds = 40.f;

// ---------------------------------------------------------------- tempo --

constexpr float kDefaultTempo = 120.f;
constexpr float kMinTempo     = 40.f;
constexpr float kMaxTempo     = 240.f;
constexpr int   kBeatsPerBar  = 4;

// Follow MIDI clock whenever it's arriving (the P-6 sends it on MIDI OUT).
// Set to false to always use the Pod's own tempo, e.g. if your controller
// sends clock even while stopped.
constexpr bool kFollowMidiClock = true;

// --------------------------------------------------------- record mode --

// What a recording snaps to, and whether it starts on the press or on the
// next line. Shift + Button 2 cycles through these on the Pod.
constexpr looper::Quantize  kDefaultQuantize = looper::Quantize::Bar;
constexpr looper::StartMode kDefaultStart    = looper::StartMode::Immediate;

// ----------------------------------------------------------------- MIDI --

// Channel (1-16) that note and CC remote control listens on, or 0 for any
// channel. The Roland P-6 sends its sample pads on channel 11 by default.
constexpr int kControlChannel = 11;

// Pads on the P-6 send notes 48-95: bank A pad 1 = 48 ... bank H pad 6 = 95.
// Six notes starting here act as remote buttons. The default (90) is bank H,
// so leave bank H's pads empty on the P-6 and use them as looper controls:
//   +0..+3  track 1-4: select it and press Rec/Play/Dub
//   +4      mute / unmute the selected track
//   +5      undo / redo
constexpr int kPadNoteBase = 90;

// CC remote control on kControlChannel (102-119 are undefined in MIDI and
// aren't in the P-6's CC list).
constexpr int kTrackVolumeCcBase = 102; // CC 102-105 = track 1-4 volume
constexpr int kMonitorCc         = 106; // live input level

} // namespace cfg
