// Pod Looper - a screenless 4-track looper for a stock Electrosmith Daisy
// Pod, with its own tempo or MIDI clock sync (built around a Roland P-6).
//
// This file is the Pod-specific part: it reads the buttons, encoder, knobs
// and MIDI, drives the two RGB LEDs, and feeds audio through looper::Engine.
// See README.md for the controls.

#include <cmath>

#include "daisy_pod.h"

#include "config.h"
#include "looper.h"
#include "spsc_queue.h"

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

// Notes and CCs, from the main loop to the audio callback.
struct RemoteMsg
{
    bool    is_cc;
    uint8_t num;
    uint8_t value;
};

// A knob only takes over a value once it has been moved, so selecting a
// track (or a CC change) never makes the level jump.
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

int16_t DSY_SDRAM_BSS buf_l[kTracks][kMaxFrames];
int16_t DSY_SDRAM_BSS buf_r[kTracks][kMaxFrames];
int16_t DSY_SDRAM_BSS undo_l[kMaxFrames];
int16_t DSY_SDRAM_BSS undo_r[kMaxFrames];

SpscQueue<SyncMsg, 128>  sync_q;
SpscQueue<RemoteMsg, 64> remote_q;

// UI state, touched only by the audio callback.
int      selected      = 0;
int      mode_index    = 0;
Pickup   knob1, knob2;
bool     b2_cleared    = false;
bool     b2_fresh      = false;
bool     b2_was_shift  = false;
uint32_t enc_down_at   = 0;
bool     shift_used    = false;
int      boot_blocks   = 0;
uint32_t beat_flash_until = 0;
bool     beat_flash_bar   = false;
uint32_t last_beat_count  = 0;
uint32_t confirm_until    = 0; // white flash on LED 2
uint32_t refuse_until     = 0; // red flash on LED 2
uint32_t show_mode_until  = 0;
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

// Main loop: forward notes and CCs on the control channel to the audio side.
void PollMidi()
{
    hw.midi.Listen();
    while(hw.midi.HasEvents())
    {
        MidiEvent e = hw.midi.PopEvent();
        if(e.type == NoteOn)
        {
            const NoteOnEvent n = e.AsNoteOn();
            // The P-6 sends note-ons only; velocity 0 would mean note-off.
            if(n.velocity > 0 && OnControlChannel(n.channel))
                remote_q.Push({false, n.note, n.velocity});
        }
        else if(e.type == ControlChange)
        {
            const ControlChangeEvent c = e.AsControlChange();
            if(OnControlChannel(c.channel))
                remote_q.Push({true, c.control_number, c.value});
        }
    }
}

// --------------------------------------------------------------- controls
//
// The encoder doubles as Shift: hold it down and
//   turn       = tempo -/+ 1 BPM
//   Button 1   = undo / redo
//   Button 2   = next record mode
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
        knob2.ref  = k2; // the input level follows Knob 2 from the start
        knob2.live = true;
        engine.SetMonitor(KnobToGain(k2));
        return;
    }

    if(hw.encoder.RisingEdge())
    {
        enc_down_at = now;
        shift_used  = false;
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

    if(hw.encoder.FallingEdge() && !shift_used && now - enc_down_at < kClickMaxMs)
    {
        if(!engine.TransportToggle())
            Refuse(now); // the P-6 is in charge of Play/Stop
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
            show_mode_until = now + 1500;
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
            b2_fresh = true;
            Confirm(now);
        }
    }

    // Knob 1: selected track's volume. Knob 2: live input level.
    if(knob1.Update(k1))
        engine.SetVolume(selected, KnobToGain(k1));
    if(knob2.Update(k2))
        engine.SetMonitor(KnobToGain(k2));
}

void HandleRemote(uint32_t now)
{
    RemoteMsg m;
    while(remote_q.Pop(m))
    {
        if(!m.is_cc)
        {
            const int pad = static_cast<int>(m.num) - cfg::kPadNoteBase;
            if(pad >= 0 && pad < kTracks)
            {
                SelectTrack(pad);
                engine.RecPlayDub(pad);
            }
            else if(pad == kTracks)
                engine.StopToggle(selected);
            else if(pad == kTracks + 1)
                DoUndo(now);
        }
        else
        {
            const float v = m.value / 127.f;
            const int   t = static_cast<int>(m.num) - cfg::kTrackVolumeCcBase;
            if(t >= 0 && t < kTracks)
            {
                engine.SetVolume(t, KnobToGain(v));
                if(t == selected)
                    knob1.Hold(hw.knob1.Value());
            }
            else if(m.num == cfg::kMonitorCc)
            {
                engine.SetMonitor(KnobToGain(v));
                knob2.Hold(hw.knob2.Value());
            }
        }
    }
}

// ------------------------------------------------------------------- LEDs
//
// LED 1: the selected track's colour, pulsing bright on every beat and white
//        on the first beat of each bar. Steady and dim = stopped.
// LED 2: what that track is doing (see README), or the record mode while
//        the encoder is held.

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
    if(hw.encoder.Pressed() || now < show_mode_until)
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
    HandleRemote(now_ms);

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

    engine.Process(in[0], in[1], out[0], out[1], size, events, num_events);

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

    hw.StartAdc();
    hw.midi.StartReceiveRt(OnMidiRealtime);
    hw.StartAudio(AudioCallback);

    for(;;)
        PollMidi();
}
