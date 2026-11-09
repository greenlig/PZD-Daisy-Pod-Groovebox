// Pod Looper - a screenless 4-track looper with a small CS-80-inspired
// polysynth, for a stock Electrosmith Daisy Pod. Runs on its own tempo or
// MIDI clock (built around a Roland P-6).
//
// This file is the Pod-specific part: it reads the buttons, encoder, knobs
// and MIDI, drives the two RGB LEDs, plays the synth and feeds everything
// through looper::Engine. See README.md for the controls.

#include <cmath>

#include "daisy_pod.h"

#include "config.h"
#include "looper.h"
#include "spsc_queue.h"
#include "synth.h"

using namespace daisy;
using looper::Quantize;
using looper::StartMode;
using looper::TrackState;

namespace
{
constexpr int    kTracks    = cfg::kNumTracks;
constexpr size_t kMaxFrames = static_cast<size_t>(cfg::kMaxLoopSeconds * cfg::kSampleRate);

static_assert(kTracks <= looper::kMaxTracks, "too many tracks");
static_assert(2ull * (kTracks + 1) * kMaxFrames * sizeof(int16_t) <= 64ull * 1024 * 1024,
              "loop buffers don't fit in the Seed's 64 MB SDRAM");

constexpr uint32_t kClearHoldMs      = 1000; // hold Button 2: clear this track
constexpr uint32_t kStartFreshHoldMs = 3000; // keep holding: start fresh
constexpr uint32_t kClickMaxMs       = 600;  // encoder press shorter than this = Play/Stop
constexpr int      kBootSettleMs     = 200;  // let the knob readings settle

struct Rgb
{
    float r, g, b;
};
constexpr Rgb kOff{0.f, 0.f, 0.f};
constexpr Rgb kRed{1.f, 0.f, 0.f};
constexpr Rgb kAmber{1.f, 0.6f, 0.f};
constexpr Rgb kGreen{0.f, 1.f, 0.f};
constexpr Rgb kCyan{0.f, 1.f, 1.f};
constexpr Rgb kBlue{0.f, 0.f, 0.8f};
constexpr Rgb kWhite{1.f, 1.f, 1.f};
// LED 1 colour for tracks 1-4: cyan, magenta, yellow, white.
constexpr Rgb kTrackColour[4] = {{0.f, 1.f, 1.f},
                                 {1.f, 0.f, 1.f},
                                 {1.f, 1.f, 0.f},
                                 {1.f, 1.f, 1.f}};
// LED 2 colour shown when a synth patch is picked: Blade, Strings, Bass, Bells.
constexpr Rgb kPatchColour[synth::kPatches] = {{1.f, 0.35f, 0.f},
                                               {1.f, 0.f, 1.f},
                                               {0.f, 0.f, 1.f},
                                               {0.f, 1.f, 1.f}};
constexpr float kDim = 0.45f; // LED 1 between beats (the LED driver cubes it)

// Record modes in the order Shift + Button 2 steps through them.
constexpr looper::RecordMode kModes[] = {
    {Quantize::Bar, StartMode::Immediate},
    {Quantize::Bar, StartMode::Armed},
    {Quantize::Beat, StartMode::Immediate},
    {Quantize::Beat, StartMode::Armed},
    {Quantize::Free, StartMode::Immediate},
};
constexpr int kNumModes = sizeof(kModes) / sizeof(kModes[0]);

// MIDI clock/Start/Stop, stamped with their arrival time in the UART
// interrupt so the audio callback can place them to within a sample or two.
struct SyncMsg
{
    looper::SyncType type;
    uint32_t         us;
};

// Everything else from MIDI, sorted in the main loop and acted on in the
// audio callback.
enum class Kind : uint8_t
{
    LooperPad,   // a = pad 0-5
    LooperCc,    // a = cc, b = value
    NoteOn,      // a = note, b = velocity
    PadNoteOn,   // same, from a P-6 pad: releases itself if no note-off comes
    NoteOff,     // a = note
    SynthCc,     // a = cc, b = value
    Bend,        // a = low 7 bits, b = high 7 bits (0..16383)
    Pressure,    // b = value
    PolyPressure, // a = note, b = value
    Program,     // a = program
};
struct MidiMsg
{
    Kind    kind;
    uint8_t a;
    uint8_t b;
};

// A knob only takes over a value once it has been moved, so selecting a
// track (or a CC change, or letting go of Shift) never makes a value jump.
struct Pickup
{
    float ref  = 0.f;
    bool  live = false;

    void Hold(float knob)
    {
        ref  = knob;
        live = false;
    }
    bool Update(float knob)
    {
        if(!live && std::fabs(knob - ref) > 0.03f)
            live = true;
        return live;
    }
};
} // namespace

DaisyPod       hw;
looper::Engine engine;
synth::Synth   poly;

int16_t DSY_SDRAM_BSS buf_l[kTracks][kMaxFrames];
int16_t DSY_SDRAM_BSS buf_r[kTracks][kMaxFrames];
int16_t DSY_SDRAM_BSS undo_l[kMaxFrames];
int16_t DSY_SDRAM_BSS undo_r[kMaxFrames];

SpscQueue<SyncMsg, 128> sync_q;
SpscQueue<MidiMsg, 128> midi_q;

// UI state, touched only by the audio callback.
int      selected        = 0;
int      mode_index      = 0;
Pickup   knob1, knob2;           // track volume, live level
Pickup   shift_k1, shift_k2;     // brightness, patch (while Shift is held)
bool     b2_cleared      = false;
bool     b2_fresh        = false;
bool     b2_was_shift    = false;
uint32_t enc_down_at     = 0;
bool     shift_used      = false;
int      boot_blocks     = 0;
uint32_t beat_flash_until = 0;
bool     beat_flash_bar   = false;
uint32_t last_beat_count  = 0;
uint32_t confirm_until    = 0; // white flash on LED 2
uint32_t refuse_until     = 0; // red flash on LED 2
uint32_t show_mode_until  = 0;
uint32_t show_patch_until = 0;
float    frames_per_us    = 0.048f;

inline float KnobToGain(float k)
{
    return 1.5f * k * k; // unity at about 80 %, up to +3.5 dB
}

void SelectTrack(int t)
{
    selected = t;
    knob1.Hold(hw.knob1.Value());
}

void Confirm(uint32_t now)
{
    confirm_until = now + 200;
}

void Refuse(uint32_t now)
{
    refuse_until = now + 200;
}

void DoUndo(uint32_t now)
{
    if(engine.Undo())
        Confirm(now);
    else
        Refuse(now);
}

void PickPatch(int p, uint32_t now)
{
    if(p == poly.PatchIndex())
        return;
    poly.SetPatch(p);
    show_patch_until = now + 1500;
}

// ------------------------------------------------------------------- MIDI

// Runs inside the UART interrupt: just timestamp and queue.
void OnMidiRealtime(MidiEvent& e)
{
    SyncMsg m;
    m.us = System::GetUs();
    switch(e.srt_type)
    {
        case SystemRealTimeType::TimingClock: m.type = looper::SyncType::Clock; break;
        case SystemRealTimeType::Start: m.type = looper::SyncType::Start; break;
        case SystemRealTimeType::Continue: m.type = looper::SyncType::Continue; break;
        case SystemRealTimeType::Stop: m.type = looper::SyncType::Stop; break;
        default: return; // active sensing etc.
    }
    sync_q.Push(m);
}

inline bool OnControlChannel(int channel) // libDaisy channels are 0-15
{
    return cfg::kControlChannel == 0 || channel == cfg::kControlChannel - 1;
}

inline bool OnSynthChannel(int channel)
{
    return cfg::kSynthChannel == 0 ? !OnControlChannel(channel)
                                   : channel == cfg::kSynthChannel - 1;
}

// A P-6 pad in banks E-G -> a note of the scale, one octave per bank.
inline int PadToSynthNote(int pad_note)
{
    const int i = pad_note - cfg::kSynthPadFirstNote;
    return cfg::kSynthPadRoot + 12 * (i / 6) + cfg::kSynthPadScale[i % 6];
}

void RouteNote(int channel, uint8_t note, uint8_t velocity, bool on)
{
    if(OnControlChannel(channel))
    {
        const int pad = static_cast<int>(note) - cfg::kPadNoteBase;
        if(pad >= 0 && pad < kTracks + 2)
        {
            if(on)
                midi_q.Push({Kind::LooperPad, static_cast<uint8_t>(pad), 0});
            return;
        }
        if(cfg::kPadsPlaySynth && note >= cfg::kSynthPadFirstNote
           && note <= cfg::kSynthPadLastNote)
        {
            const uint8_t n = static_cast<uint8_t>(PadToSynthNote(note));
            midi_q.Push({on ? Kind::PadNoteOn : Kind::NoteOff, n, velocity});
        }
        return;
    }
    if(OnSynthChannel(channel))
        midi_q.Push({on ? Kind::NoteOn : Kind::NoteOff, note, velocity});
}

// Main loop: sort incoming MIDI for the audio side.
void PollMidi()
{
    hw.midi.Listen();
    while(hw.midi.HasEvents())
    {
        MidiEvent e = hw.midi.PopEvent();
        switch(e.type)
        {
            case NoteOn:
            {
                const NoteOnEvent n = e.AsNoteOn();
                RouteNote(n.channel, n.note, n.velocity, true);
                break;
            }
            case NoteOff: // libDaisy also delivers velocity-0 note-ons as these
            {
                const NoteOffEvent n = e.AsNoteOff();
                RouteNote(n.channel, n.note, 0, false);
                break;
            }
            case ControlChange:
            {
                const ControlChangeEvent c = e.AsControlChange();
                const int cc = c.control_number;
                if(OnControlChannel(c.channel) && cc >= cfg::kTrackVolumeCcBase
                   && cc <= cfg::kMonitorCc)
                    midi_q.Push({Kind::LooperCc, c.control_number, c.value});
                else if(OnSynthChannel(c.channel))
                    midi_q.Push({Kind::SynthCc, c.control_number, c.value});
                break;
            }
            case PitchBend:
            {
                const PitchBendEvent p = e.AsPitchBend();
                if(OnSynthChannel(p.channel))
                {
                    const int v = p.value + 8192; // 0..16383
                    midi_q.Push({Kind::Bend, static_cast<uint8_t>(v & 0x7f),
                                 static_cast<uint8_t>((v >> 7) & 0x7f)});
                }
                break;
            }
            case ChannelPressure:
            {
                const ChannelPressureEvent p = e.AsChannelPressure();
                if(OnSynthChannel(p.channel))
                    midi_q.Push({Kind::Pressure, 0, p.pressure});
                break;
            }
            case PolyphonicKeyPressure:
            {
                const PolyphonicKeyPressureEvent p = e.AsPolyphonicKeyPressure();
                if(OnSynthChannel(p.channel))
                    midi_q.Push({Kind::PolyPressure, p.note, p.pressure});
                break;
            }
            case ProgramChange:
            {
                // Only on a dedicated synth channel: the P-6 sends program
                // changes of its own when it changes patterns.
                const ProgramChangeEvent p = e.AsProgramChange();
                if(cfg::kSynthChannel != 0 && OnSynthChannel(p.channel))
                    midi_q.Push({Kind::Program, p.program, 0});
                break;
            }
            default: break;
        }
    }
}

// --------------------------------------------------------------- controls
//
// The encoder doubles as Shift: hold it down and
//   turn       = tempo -/+ 1 BPM
//   Button 1   = undo / redo
//   Button 2   = next record mode
//   Knob 1     = synth brightness
//   Knob 2     = synth patch (four zones: Blade, Strings, Bass, Bells)
// A short click on its own (no Shift action) is Play / Stop.

void HandleControls(uint32_t now)
{
    hw.ProcessAllControls();

    const float k1 = hw.knob1.Value();
    const float k2 = hw.knob2.Value();
    if(boot_blocks < kBootSettleMs)
    {
        ++boot_blocks;
        knob1.Hold(k1);  // track volumes start at unity
        knob2.ref  = k2; // the live level follows Knob 2 from the start
        knob2.live = true;
        engine.SetMonitor(KnobToGain(k2));
        return;
    }

    if(hw.encoder.RisingEdge())
    {
        enc_down_at = now;
        shift_used  = false;
        shift_k1.Hold(k1);
        shift_k2.Hold(k2);
    }
    const bool shift = hw.encoder.Pressed();

    const int inc = hw.encoder.Increment();
    if(inc != 0)
    {
        if(shift)
        {
            shift_used = true;
            if(!engine.SetTempo(std::round(engine.Tempo()) + static_cast<float>(inc)))
                Refuse(now); // locked: following MIDI clock, or grid tracks recorded
        }
        else
            SelectTrack(((selected + inc) % kTracks + kTracks) % kTracks);
    }

    if(hw.encoder.FallingEdge())
    {
        if(!shift_used && now - enc_down_at < kClickMaxMs && !engine.TransportToggle())
            Refuse(now); // the P-6 is in charge of Play/Stop
        // Hand the knobs back to volume and live level without a jump.
        knob1.Hold(k1);
        knob2.Hold(k2);
    }

    // Button 1: Rec / Play / Dub (Shift: undo / redo).
    if(hw.button1.RisingEdge())
    {
        if(shift)
        {
            shift_used = true;
            DoUndo(now);
        }
        else
            engine.RecPlayDub(selected);
    }

    // Button 2: tap = mute/unmute, hold 1 s = clear track, 3 s = start fresh
    // (Shift: next record mode).
    if(hw.button2.RisingEdge())
    {
        b2_cleared   = false;
        b2_fresh     = false;
        b2_was_shift = shift;
        if(shift)
        {
            shift_used = true;
            mode_index = (mode_index + 1) % kNumModes;
            engine.SetRecordMode(kModes[mode_index]);
            show_mode_until  = now + 1500;
            show_patch_until = 0;
        }
        else
            engine.StopToggle(selected);
    }
    if(hw.button2.Pressed() && !b2_was_shift)
    {
        const float held = hw.button2.TimeHeldMs();
        if(!b2_cleared && held >= kClearHoldMs)
        {
            engine.Clear(selected);
            b2_cleared = true;
            Confirm(now);
        }
        if(!b2_fresh && held >= kStartFreshHoldMs)
        {
            engine.StartFresh();
            poly.AllNotesOff();
            b2_fresh = true;
            Confirm(now);
        }
    }

    if(shift)
    {
        // Shift + knobs: the synth.
        if(shift_k1.Update(k1))
        {
            shift_used = true;
            poly.SetBrightness(k1);
        }
        if(shift_k2.Update(k2))
        {
            shift_used = true;
            int p = static_cast<int>(k2 * synth::kPatches);
            PickPatch(p < synth::kPatches ? p : synth::kPatches - 1, now);
        }
    }
    else
    {
        // Knob 1: selected track's volume. Knob 2: live level (input + synth).
        if(knob1.Update(k1))
            engine.SetVolume(selected, KnobToGain(k1));
        if(knob2.Update(k2))
            engine.SetMonitor(KnobToGain(k2));
    }
}

void HandleMidi(uint32_t now)
{
    MidiMsg m;
    while(midi_q.Pop(m))
    {
        switch(m.kind)
        {
            case Kind::LooperPad:
                if(m.a < kTracks)
                {
                    SelectTrack(m.a);
                    engine.RecPlayDub(m.a);
                }
                else if(m.a == kTracks)
                    engine.StopToggle(selected);
                else
                    DoUndo(now);
                break;

            case Kind::LooperCc:
            {
                const float v = m.b / 127.f;
                const int   t = static_cast<int>(m.a) - cfg::kTrackVolumeCcBase;
                if(t >= 0 && t < kTracks)
                {
                    engine.SetVolume(t, KnobToGain(v));
                    if(t == selected)
                        knob1.Hold(hw.knob1.Value());
                }
                else if(m.a == cfg::kMonitorCc)
                {
                    engine.SetMonitor(KnobToGain(v));
                    knob2.Hold(hw.knob2.Value());
                }
                break;
            }

            case Kind::NoteOn: poly.NoteOn(m.a, m.b); break;
            case Kind::PadNoteOn: poly.NoteOn(m.a, m.b, cfg::kPadMaxHoldSeconds); break;
            case Kind::NoteOff: poly.NoteOff(m.a); break;

            case Kind::SynthCc:
                switch(m.a)
                {
                    case 1: poly.SetModWheel(m.b / 127.f); break;
                    case 64: poly.SetSustain(m.b >= 64); break;
                    case 71: poly.SetResonanceOffset((m.b - 64) / 128.f); break;
                    case 74: poly.SetBrightness(m.b / 127.f); break;
                    case 120:
                    case 123: poly.AllNotesOff(); break;
                    default: break;
                }
                break;

            case Kind::Bend:
                poly.SetPitchBend(((m.b << 7 | m.a) - 8192) / 8192.f);
                break;
            case Kind::Pressure: poly.SetPressure(m.b / 127.f); break;
            case Kind::PolyPressure: poly.SetPolyPressure(m.a, m.b / 127.f); break;
            case Kind::Program: PickPatch(m.a % synth::kPatches, now); break;
        }
    }
}

// ------------------------------------------------------------------- LEDs
//
// LED 1: the selected track's colour, pulsing bright on every beat and white
//        on the first beat of each bar. Steady and dim = stopped.
// LED 2: what that track is doing (see README); the record mode while the
//        encoder is held; the synth patch's colour just after it changes.

Rgb Scale(Rgb c, float k)
{
    return {c.r * k, c.g * k, c.b * k};
}

void UpdateLeds(uint32_t now)
{
    const uint32_t beats = engine.BeatCount();
    if(beats != last_beat_count)
    {
        last_beat_count  = beats;
        beat_flash_until = now + 90;
        beat_flash_bar   = engine.LastBeatWasBar();
    }
    const Rgb track = kTrackColour[selected % 4];
    Rgb       c1    = Scale(track, kDim);
    if(engine.Playing() && now < beat_flash_until)
        c1 = beat_flash_bar ? kWhite : track;

    const bool fast = (now / 125) % 2;
    Rgb        c2   = kOff;
    if(now < show_patch_until)
        c2 = kPatchColour[poly.PatchIndex()];
    else if(hw.encoder.Pressed() || now < show_mode_until)
    {
        // Record mode: green = bar, cyan = beat, white = free; blinking =
        // armed start (waits for the line), steady = starts on the press.
        const looper::RecordMode m = engine.Mode();
        c2 = m.quantize == Quantize::Bar ? kGreen : m.quantize == Quantize::Beat ? kCyan : kWhite;
        if(m.quantize != Quantize::Free && m.start == StartMode::Armed && fast)
            c2 = kOff;
    }
    else
    {
        switch(engine.State(selected))
        {
            case TrackState::Empty: break;
            case TrackState::Armed: c2 = fast ? kRed : kOff; break;
            case TrackState::Recording:
                c2 = (engine.EndPending(selected) && fast) ? kOff : kRed;
                break;
            case TrackState::Overdubbing: c2 = kAmber; break;
            case TrackState::Playing: c2 = kGreen; break;
            case TrackState::Muted: c2 = kBlue; break;
        }
    }
    if(now < confirm_until)
        c2 = kWhite;
    if(now < refuse_until)
        c2 = fast ? kRed : kOff;

    hw.led1.Set(c1.r, c1.g, c1.b);
    hw.led2.Set(c2.r, c2.g, c2.b);
    hw.UpdateLeds();
}

// ------------------------------------------------------------------ audio

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    const uint32_t now_us = System::GetUs();
    const uint32_t now_ms = System::GetNow();

    HandleControls(now_ms);
    HandleMidi(now_ms);

    // Place each clock message at the input sample that arrived when it did.
    // This block's input was captured over the last `size` samples, so a
    // message that is `age` old belongs `age` samples before the block end.
    looper::SyncEvent events[32];
    size_t            num_events = 0;
    SyncMsg           m;
    while(num_events < 32 && sync_q.Pop(m))
    {
        const int32_t age_us = static_cast<int32_t>(now_us - m.us);
        int frame = static_cast<int>(size) - static_cast<int>(std::lround(age_us * frames_per_us));
        if(frame < 0)
            frame = 0;
        if(frame >= static_cast<int>(size))
            frame = static_cast<int>(size) - 1;
        events[num_events++] = {m.type, static_cast<uint32_t>(frame)};
    }

    // The synth joins the live input, so it's heard (Knob 2) and can be
    // recorded into a track like anything else you play.
    float live_l[cfg::kBlockSize], live_r[cfg::kBlockSize];
    if(size > cfg::kBlockSize)
        size = cfg::kBlockSize;
    poly.Process(live_l, live_r, size);
    for(size_t i = 0; i < size; ++i)
    {
        live_l[i] = in[0][i] + live_l[i] * cfg::kSynthLevel;
        live_r[i] = in[1][i] + live_r[i] * cfg::kSynthLevel;
    }

    engine.Process(live_l, live_r, out[0], out[1], size, events, num_events);

    UpdateLeds(now_ms);
}

// -------------------------------------------------------------- main loop

int main(void)
{
    hw.Init(true); // 480 MHz
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.SetAudioBlockSize(cfg::kBlockSize);
    frames_per_us = hw.AudioSampleRate() * 1e-6f;

    int16_t* lp[kTracks];
    int16_t* rp[kTracks];
    for(int t = 0; t < kTracks; ++t)
    {
        lp[t] = buf_l[t];
        rp[t] = buf_r[t];
    }
    looper::EngineConfig ec;
    ec.sample_rate          = hw.AudioSampleRate();
    ec.max_frames           = kMaxFrames;
    ec.beats_per_bar        = cfg::kBeatsPerBar;
    ec.tempo                = cfg::kDefaultTempo;
    ec.min_tempo            = cfg::kMinTempo;
    ec.max_tempo            = cfg::kMaxTempo;
    ec.follow_clock         = cfg::kFollowMidiClock;
    ec.record_mode.quantize = cfg::kDefaultQuantize;
    ec.record_mode.start    = cfg::kDefaultStart;
    engine.Init(ec, lp, rp, undo_l, undo_r, kTracks);
    for(int i = 0; i < kNumModes; ++i)
        if(kModes[i].quantize == ec.record_mode.quantize
           && kModes[i].start == ec.record_mode.start)
            mode_index = i;

    poly.Init(hw.AudioSampleRate());
    poly.SetPatch(cfg::kDefaultSynthPatch);

    hw.StartAdc();
    hw.midi.StartReceiveRt(OnMidiRealtime);
    hw.StartAudio(AudioCallback);

    for(;;)
        PollMidi();
}
