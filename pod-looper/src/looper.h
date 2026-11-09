// Pod Looper - the loop engine.
//
// Plain C++ with no libDaisy dependency, so it can be unit tested on a PC
// (see test/). The Pod-specific code in main.cpp turns buttons, knobs and
// MIDI into calls on this class and shows its state on the LEDs.
//
// Model (the same one PZD's Groovebox uses):
//  - Up to kMaxLayers stereo layers share one loop length and one playhead.
//  - The first layer you record sets that length. After that, recording on
//    any empty layer starts straight away, in time with the loop.
//  - If MIDI clock is running, the first recording starts and ends on a bar
//    line, the loop is kept in phase with the clock, and MIDI Start/Stop
//    restart/pause it.
//
// Threading: every method except NeedsZero/ZeroFrames/MarkZeroed must be
// called from the audio thread. Clearing a layer hands its buffer to another
// thread to be zeroed (NeedsZero -> memset -> MarkZeroed), so no big memset
// ever happens inside the audio callback.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace looper
{
constexpr int kMaxLayers = 4;

enum class LayerState : uint8_t
{
    Empty,       // nothing recorded
    Armed,       // waiting to record (for a bar line, or for its buffer to be zeroed)
    Recording,   // the first take, which sets the loop length
    Overdubbing, // adding to the loop (also how a new layer is recorded)
    Playing,
    Muted,
};

enum class SyncType : uint8_t
{
    Clock, // 24 per quarter note
    Start,
    Continue,
    Stop,
};

struct SyncEvent
{
    SyncType type;
    uint32_t frame; // offset into the block passed to Process()
};

struct EngineConfig
{
    float  sample_rate      = 48000.f;
    size_t max_frames       = 0; // per layer, per channel
    int    beats_per_bar    = 4;
    bool   quantize_to_bars = true;
    bool   follow_transport = true;
};

class Engine
{
  public:
    // buf_l / buf_r: num_layers buffers of cfg.max_frames floats each. They
    // must be zeroed before Init.
    void Init(const EngineConfig& cfg,
              float* const*       buf_l,
              float* const*       buf_r,
              int                 num_layers);

    // ---- actions --------------------------------------------------------
    // Rec/Play/Dub, like a looper pedal: Empty -> Recording -> Playing ->
    // Overdubbing -> Playing ... (Muted -> Playing).
    void RecPlayDub(int layer);
    // Mute <-> play. Also cancels an armed recording.
    void StopToggle(int layer);
    void Clear(int layer);
    void ClearAll();
    // Pause / resume every layer together.
    void TogglePause();

    void  SetVolume(int layer, float gain);
    float Volume(int layer) const;
    void  SetMonitor(float gain); // live input straight to the output

    // ---- audio ----------------------------------------------------------
    // Events must be in time order with frame < n.
    void Process(const float*     in_l,
                 const float*     in_r,
                 float*           out_l,
                 float*           out_r,
                 size_t           n,
                 const SyncEvent* events,
                 size_t           num_events);

    // ---- buffer zeroing (any thread) -------------------------------------
    bool   NeedsZero(int layer) const;
    size_t ZeroFrames(int layer) const;
    void   MarkZeroed(int layer);

    // ---- status ---------------------------------------------------------
    LayerState State(int layer) const { return layers_[layer].state; }
    bool       StopPending() const { return stop_pending_; }
    bool       HasLoop() const { return loop_len_ > 0; }
    bool       HasContent() const;
    size_t     LoopLength() const { return loop_len_; }
    size_t     Playhead() const { return ph_; }
    bool       Paused() const { return paused_; }
    bool       ClockPresent() const { return clock_present_; }
    bool       TransportRunning() const { return transport_running_; }
    bool       Synced() const { return synced_; }
    bool       SyncActive() const;
    // Counters that tick on every loop wrap / every bar line, for the LEDs.
    uint32_t LoopWraps() const { return loop_wraps_; }
    uint32_t BarCount() const { return bar_count_; }
    // Sync diagnostics: how far (in frames) the playhead was from the MIDI
    // clock at the last bar line, before any correction, and how many times
    // it has been pulled back into phase.
    int32_t  LastPhaseError() const { return last_phase_error_; }
    uint32_t PhaseCorrections() const { return phase_corrections_; }

  private:
    struct Layer
    {
        float*     l          = nullptr;
        float*     r          = nullptr;
        LayerState state      = LayerState::Empty;
        float      volume     = 1.f; // target level
        float      gain       = 0.f; // smoothed output level
        float      rec_gain   = 0.f; // smoothed overdub input level
        size_t     dirty      = 0;   // frames that may hold audio
        bool       want_zero  = false;
        size_t     zero_frames = 0;
        std::atomic<bool> needs_zero{false};
    };

    void   ServiceArmed();
    bool   ZeroReady(const Layer& L) const;
    void   StartFirstTake(int layer);
    void   RequestFinishFirstTake();
    void   FinishFirstTake(bool at_bar);
    void   StartOverdub(int layer);
    void   DisarmFirstTakes();
    void   HandleSync(SyncType type);
    void   OnClockTick();
    void   JumpTo(size_t target);
    // Where a synced loop's playhead belongs at the clock tick being handled.
    size_t ExpectedPhase() const;
    void   EndOfBlock();

    EngineConfig cfg_;
    Layer        layers_[kMaxLayers];
    int          num_layers_ = 0;

    // shared loop
    size_t loop_len_      = 0; // 0 = no loop yet
    size_t ph_            = 0; // playhead
    bool   paused_        = false;
    float  play_gain_     = 0.f;
    bool   reset_pending_ = false; // all layers cleared, waiting for fade-out
    uint32_t loop_wraps_  = 0;

    // first take
    int    recording_layer_   = -1;
    size_t rec_pos_           = 0;
    bool   stop_pending_      = false; // finish on the next bar line
    bool   finish_to_muted_   = false;
    bool   first_take_synced_ = false;
    int64_t anchor_tick_      = 0;

    // crossfade used when the playhead jumps to stay in sync
    size_t xf_from_  = 0;
    size_t xf_left_  = 0;
    size_t xf_total_ = 0; // length of the crossfade in progress
    size_t xf_len_   = 0; // standard crossfade length

    // MIDI clock
    bool     clock_present_     = false;
    bool     transport_running_ = false;
    bool     resume_on_tick_    = false;
    int64_t  tick_count_        = 0; // ticks since the last Start
    uint32_t frames_since_tick_ = 0;
    uint32_t bar_count_         = 0;
    bool     synced_            = false; // loop length is a whole number of bars
    int64_t  loop_ticks_        = 0;
    int32_t  last_phase_error_  = 0;
    uint32_t phase_corrections_ = 0;

    // monitor
    float monitor_        = 1.f;
    float monitor_smooth_ = 1.f;

    // derived constants
    float    smooth_coef_     = 0.f;
    int      ticks_per_bar_   = 96;
    uint32_t clock_timeout_   = 0;
    size_t   min_take_frames_ = 0;
    size_t   seam_fade_       = 0;
    size_t   drift_threshold_ = 0;
};

} // namespace looper
