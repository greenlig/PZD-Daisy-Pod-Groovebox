// Pod Looper - a screenless 4-layer stereo looper for a stock Electrosmith
// Daisy Pod, with MIDI clock sync (built around a Roland P-6).
//
// This file is the Pod-specific part: it reads the buttons, encoder, knobs
// and MIDI, drives the two RGB LEDs, and feeds audio through looper::Engine.
// See README.md for the controls.

#include <cmath>
#include <cstring>

#include "daisy_pod.h"

#include "config.h"
#include "looper.h"
#include "spsc_queue.h"

using namespace daisy;

namespace
{
constexpr int    kLayers    = cfg::kNumLayers;
constexpr size_t kMaxFrames = static_cast<size_t>(cfg::kMaxLoopSeconds * cfg::kSampleRate);

static_assert(kLayers <= looper::kMaxLayers, "too many layers");
static_assert(2ull * kLayers * kMaxFrames * sizeof(float) <= 64ull * 1024 * 1024,
              "loop buffers don't fit in the Seed's 64 MB SDRAM");

constexpr uint32_t kClearHoldMs    = 1000; // hold Button 2: clear this layer
constexpr uint32_t kClearAllHoldMs = 3000; // keep holding: clear every layer
constexpr int      kBootSettleMs   = 200;  // let the knob readings settle

struct Rgb
{
    float r, g, b;
};
constexpr Rgb kOff{0.f, 0.f, 0.f};
constexpr Rgb kRed{1.f, 0.f, 0.f};
constexpr Rgb kAmber{1.f, 0.6f, 0.f};
constexpr Rgb kGreen{0.f, 1.f, 0.f};
constexpr Rgb kBlue{0.f, 0.f, 0.8f};
constexpr Rgb kWhite{1.f, 1.f, 1.f};
// LED 1 colour for layers 1-4: cyan, magenta, yellow, white.
constexpr Rgb kLayerColour[4] = {{0.f, 1.f, 1.f},
                                 {1.f, 0.f, 1.f},
                                 {1.f, 1.f, 0.f},
                                 {1.f, 1.f, 1.f}};

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
// layer (or a CC change) never makes the level jump.
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

float DSY_SDRAM_BSS buf_l[kLayers][kMaxFrames];
float DSY_SDRAM_BSS buf_r[kLayers][kMaxFrames];

SpscQueue<SyncMsg, 128>  sync_q;
SpscQueue<RemoteMsg, 64> remote_q;

// UI state, touched only by the audio callback.
int      selected      = 0;
Pickup   knob1, knob2;
bool     b2_cleared     = false;
bool     b2_cleared_all = false;
int      boot_blocks    = 0;
uint32_t blink_until    = 0;
uint32_t flash_until    = 0;
uint32_t last_wraps     = 0;
uint32_t last_bars      = 0;
float    frames_per_us  = 0.048f;

inline float KnobToGain(float k)
{
    return 1.5f * k * k; // unity at about 80 %, up to +3.5 dB
}

void SelectLayer(int layer)
{
    selected = layer;
    knob1.Hold(hw.knob1.Value());
}

void Flash(uint32_t now)
{
    flash_until = now + 200;
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

void HandleControls(uint32_t now)
{
    hw.ProcessAllControls();

    const float k1 = hw.knob1.Value();
    const float k2 = hw.knob2.Value();
    if(boot_blocks < kBootSettleMs)
    {
        ++boot_blocks;
        knob1.Hold(k1);  // layer volumes start at unity
        knob2.ref  = k2; // the input level follows Knob 2 from the start
        knob2.live = true;
        engine.SetMonitor(KnobToGain(k2));
        return;
    }

    // Encoder: turn to pick a layer, click to pause/resume everything.
    const int inc = hw.encoder.Increment();
    if(inc != 0)
        SelectLayer(((selected + inc) % kLayers + kLayers) % kLayers);
    if(hw.encoder.RisingEdge())
        engine.TogglePause();

    // Button 1: Rec / Play / Dub.
    if(hw.button1.RisingEdge())
        engine.RecPlayDub(selected);

    // Button 2: tap = mute/unmute, hold 1 s = clear layer, 3 s = clear all.
    if(hw.button2.RisingEdge())
    {
        engine.StopToggle(selected);
        b2_cleared     = false;
        b2_cleared_all = false;
    }
    if(hw.button2.Pressed())
    {
        const float held = hw.button2.TimeHeldMs();
        if(!b2_cleared && held >= kClearHoldMs)
        {
            engine.Clear(selected);
            b2_cleared = true;
            Flash(now);
        }
        if(!b2_cleared_all && held >= kClearAllHoldMs)
        {
            engine.ClearAll();
            b2_cleared_all = true;
            Flash(now);
        }
    }

    // Knob 1: selected layer's volume. Knob 2: live input level.
    if(knob1.Update(k1))
        engine.SetVolume(selected, KnobToGain(k1));
    if(knob2.Update(k2))
        engine.SetMonitor(KnobToGain(k2));
}

void HandleRemote()
{
    RemoteMsg m;
    while(remote_q.Pop(m))
    {
        if(!m.is_cc)
        {
            const int pad = static_cast<int>(m.num) - cfg::kPadNoteBase;
            if(pad >= 0 && pad < kLayers)
            {
                SelectLayer(pad);
                engine.RecPlayDub(pad);
            }
            else if(pad == kLayers)
                engine.StopToggle(selected);
            else if(pad == kLayers + 1)
                engine.TogglePause();
        }
        else
        {
            const float v     = m.value / 127.f;
            const int   layer = static_cast<int>(m.num) - cfg::kLayerVolumeCcBase;
            if(layer >= 0 && layer < kLayers)
            {
                engine.SetVolume(layer, KnobToGain(v));
                if(layer == selected)
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
// LED 1: which layer is selected (colour), blinking off briefly at the top
//        of the loop - or on every bar while the P-6 clock is running and
//        there's no loop yet.
// LED 2: what that layer is doing - off empty, flashing red armed, red
//        recording (flickering while it waits for the bar to stop), amber
//        overdubbing, green playing, blue muted. Slow blink = paused.
//        A white flash confirms a clear.
void UpdateLeds(uint32_t now)
{
    const uint32_t wraps = engine.LoopWraps();
    const uint32_t bars  = engine.BarCount();
    if(wraps != last_wraps)
    {
        last_wraps = wraps;
        blink_until = now + 60;
    }
    if(bars != last_bars)
    {
        last_bars = bars;
        if(!engine.HasLoop())
            blink_until = now + 60;
    }
    Rgb c1 = now < blink_until ? kOff : kLayerColour[selected % 4];

    const bool fast = (now / 125) % 2;
    const bool slow = (now / 500) % 2;
    Rgb        c2   = kOff;
    const looper::LayerState s = engine.State(selected);
    switch(s)
    {
        case looper::LayerState::Empty: break;
        case looper::LayerState::Armed: c2 = fast ? kRed : kOff; break;
        case looper::LayerState::Recording:
            c2 = (engine.StopPending() && fast) ? kOff : kRed;
            break;
        case looper::LayerState::Overdubbing: c2 = kAmber; break;
        case looper::LayerState::Playing: c2 = kGreen; break;
        case looper::LayerState::Muted: c2 = kBlue; break;
    }
    if(engine.Paused() && engine.HasLoop() && slow
       && s != looper::LayerState::Recording)
        c2 = kOff;
    if(now < flash_until)
        c2 = kWhite;

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
    HandleRemote();

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

// Clearing a layer hands its buffer here to be zeroed, a slice at a time so
// MIDI keeps flowing.
void ServiceZeroing()
{
    static int    layer = -1;
    static size_t pos = 0, total = 0;
    if(layer < 0)
    {
        for(int k = 0; k < kLayers; ++k)
        {
            if(engine.NeedsZero(k))
            {
                layer = k;
                pos   = 0;
                total = engine.ZeroFrames(k);
                if(total > kMaxFrames)
                    total = kMaxFrames;
                break;
            }
        }
        if(layer < 0)
            return;
    }
    const size_t n = (total - pos) < 16384 ? (total - pos) : 16384;
    std::memset(&buf_l[layer][pos], 0, n * sizeof(float));
    std::memset(&buf_r[layer][pos], 0, n * sizeof(float));
    pos += n;
    if(pos >= total)
    {
        engine.MarkZeroed(layer);
        layer = -1;
    }
}

int main(void)
{
    hw.Init(true); // 480 MHz
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.SetAudioBlockSize(cfg::kBlockSize);
    frames_per_us = hw.AudioSampleRate() * 1e-6f;

    // SDRAM isn't cleared at power-on, and empty layers must be silent.
    std::memset(buf_l, 0, sizeof(buf_l));
    std::memset(buf_r, 0, sizeof(buf_r));

    float* lp[kLayers];
    float* rp[kLayers];
    for(int k = 0; k < kLayers; ++k)
    {
        lp[k] = buf_l[k];
        rp[k] = buf_r[k];
    }
    looper::EngineConfig ec;
    ec.sample_rate      = hw.AudioSampleRate();
    ec.max_frames       = kMaxFrames;
    ec.beats_per_bar    = cfg::kBeatsPerBar;
    ec.quantize_to_bars = cfg::kQuantizeToBars;
    ec.follow_transport = cfg::kFollowMidiTransport;
    engine.Init(ec, lp, rp, kLayers);

    hw.StartAdc();
    hw.midi.StartReceiveRt(OnMidiRealtime);
    hw.StartAudio(AudioCallback);

    for(;;)
    {
        PollMidi();
        ServiceZeroing();
    }
}
