// Pod Looper - the loop engine.
//
// Plain C++ with no libDaisy dependency, so it can be unit tested on a PC
// (see test/). The Pod-specific code in main.cpp turns buttons, knobs and
// MIDI into calls on this class and shows its state on the LEDs.
//
// Model:
//  - A master grid of beats and bars (4/4 by default), driven by the
//    internal tempo or, when it's arriving, external MIDI clock.
//  - Up to kMaxTracks independent stereo tracks. Each has its own length:
//    a whole number of bars (or beats) on the grid, or free.
//  - Grid tracks keep their place on the grid: what you played on beat 3
//    plays back on beat 3, and they are nudged back into phase if an
//    external clock drifts.
//  - One level of undo/redo covering the last take, overdub or clear.
//
// Every method must be called from the audio thread (main.cpp calls them at
// the start of each audio callback, before Process()).
#pragma once

#include <cstddef>
#include <cstdint>

namespace looper
{
constexpr int kMaxTracks = 4;

enum class TrackState : uint8_t
{
    Empty,
    Armed,       // waiting for the next beat/bar (or for Play) to start recording
    Recording,   // first take: sets the track's length
    Overdubbing, // adding to the loop
    Playing,
    Muted,
};

// Where a take's start and end snap to.
enum class Quantize : uint8_t
{
    Free, // no grid: the loop is exactly as long as you played
    Beat, // whole beats
    Bar,  // whole bars
};

// Whether recording starts the moment you press, or on the next beat/bar.
enum class StartMode : uint8_t
{
    Immediate,
    Armed,
};

struct RecordMode
{
    Quantize  quantize = Quantize::Bar;
    StartMode start    = StartMode::Immediate;
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
    float      sample_rate   = 48000.f;
    size_t     max_frames    = 0; // per track, per channel
    int        beats_per_bar = 4;
    float      tempo         = 120.f;
    float      min_tempo     = 40.f;
    float      max_tempo     = 240.f;
    bool       follow_clock  = true; // use external MIDI clock when it arrives
    RecordMode record_mode;
};

class Engine
{
  public:
    // buf_l / buf_r: num_tracks buffers of cfg.max_frames samples each.
    // undo_l / undo_r: one more buffer pair, for undoing overdubs.
    // Audio is stored as 16-bit with 6 dB of headroom (full scale = 2.0).
    void Init(const EngineConfig& cfg,
              int16_t* const*     buf_l,
              int16_t* const*     buf_r,
              int16_t*            undo_l,
              int16_t*            undo_r,
              int                 num_tracks);

    // ---- actions --------------------------------------------------------
    // Rec / Play / Dub: Empty -> Recording -> Playing -> Overdubbing ->
    // Playing ... and Muted -> Playing. Starts the internal transport if
    // it's stopped.
    void RecPlayDub(int track);
    // Mute <-> play. Also cancels an armed recording.
    void StopToggle(int track);
    void Clear(int track);
    // Clear every track, stop the transport, forget undo.
    void StartFresh();
    // Undo, or redo, the last take, overdub or clear (one level). Undo
    // while recording cancels the recording. Returns false if there was
    // nothing to undo.
    bool Undo();
    // Play / stop the internal transport. Returns false (does nothing)
    // while following external clock.
    bool TransportToggle();
    // Returns false if the tempo is locked (see TempoLocked).
    bool SetTempo(float bpm);
    void SetRecordMode(RecordMode m) { mode_ = m; }

    void  SetVolume(int track, float gain);
    float Volume(int track) const;
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

    // ---- status ---------------------------------------------------------
    TrackState State(int track) const { return tracks_[track].state; }
    bool       EndPending(int track) const { return tracks_[track].end_pending; }
    size_t     Length(int track) const { return tracks_[track].len; }
    size_t     Playhead(int track) const { return tracks_[track].ph; }
    bool       OnGrid(int track) const { return tracks_[track].grid; }
    // Track length in beats (0 for free tracks).
    double     LengthBeats(int track) const;
    RecordMode Mode() const { return mode_; }
    bool       Playing() const { return playing_; }
    bool       ExternalClock() const { return ext_present_; }
    float      Tempo() const;
    // Tempo can't be changed while following external clock, or while any
    // grid track holds audio (it would no longer fit the bars).
    bool TempoLocked() const;
    bool CanUndo() const { return undo_kind_ != UndoKind::None; }
    bool UndoBusy() const { return job_active_ || job_pending_; }
    // Grid position in beats, and a counter that ticks once per beat (for
    // the beat LED). LastBeatWasBar() is true when that beat was a downbeat.
    double   PositionBeats() const { return pos_ / 24.0; }
    uint32_t BeatCount() const { return beat_count_; }
    bool     LastBeatWasBar() const { return last_beat_bar_; }
    // Sync diagnostics.
    int32_t  LastPhaseError(int track) const { return tracks_[track].last_error; }
    uint32_t PhaseCorrections() const { return phase_corrections_; }

  private:
    enum class UndoKind : uint8_t
    {
        None,
        Take,    // a first recording
        Overdub, // pre-overdub audio is saved in the undo buffer
        Clear,
    };

    // What's needed to bring a cleared/undone track back.
    struct Meta
    {
        TrackState state     = TrackState::Empty;
        size_t     len       = 0;
        bool       grid      = false;
        double     anchor    = 0.0;
        double     len_ticks = 0.0;
        double     tpf       = 0.0; // tempo it was recorded at (ticks per frame)
    };

    struct Track
    {
        int16_t*   l     = nullptr;
        int16_t*   r     = nullptr;
        TrackState state = TrackState::Empty;
        size_t     len   = 0; // frames (kept after a clear, for undo)
        size_t     ph    = 0; // playhead

        // place on the grid
        bool   grid      = false;
        double anchor    = 0.0; // grid position (ticks) of loop frame 0
        double len_ticks = 0.0;
        double rec_tpf   = 0.0; // tempo it was recorded at

        float volume   = 1.f;
        float gain     = 0.f; // smoothed output level
        float rec_gain = 0.f; // smoothed overdub input level

        // take in progress
        Quantize arm_q           = Quantize::Bar;
        Quantize rec_q           = Quantize::Bar;
        size_t   rec_pos         = 0;
        double   unit            = 96.0; // ticks per beat or bar
        uint32_t units           = 0;    // whole units recorded
        double   next_unit       = 0.0;  // grid position of the next unit line
        size_t   rec_pos_at_unit = 0;    // rec_pos at the last unit line
        bool     end_pending     = false;
        uint32_t end_units       = 0;
        bool     finish_to_muted = false;
        bool     dub_after_job   = false;
        bool     anchor_on_next_frame = false; // immediate start: anchor = first recorded frame

        // crossfade for phase corrections
        size_t xf_from  = 0;
        size_t xf_left  = 0;
        size_t xf_total = 0;

        int32_t last_error = 0;
    };

    // grid / transport
    void   StartTransport(double pos);
    void   StopTransport();
    void   OnBeat(int64_t beat);
    double TicksPerFrame() const;
    bool   ExternalActive() const { return cfg_.follow_clock && ext_present_; }
    void   HandleSync(SyncType type);
    void   OnExternalTick();
    void   LoseExternalClock();

    // tracks
    void   StartTake(int t, double anchor, Quantize q);
    void   RequestFinish(int t);
    void   FinishTake(int t, size_t len, size_t over);
    void   CutTakeShort(int t); // transport stopped / memory full mid-take
    void   StartOverdub(int t);
    void   JumpTo(Track& tr, size_t target);
    size_t ExpectedPhase(const Track& tr) const;
    void   Rephase(Track& tr);
    Meta   SaveMeta(const Track& tr) const;
    void   RestoreMeta(Track& tr, const Meta& m);
    bool   HoldsLoop(TrackState s) const;
    bool   FitsTempo(const Meta& m) const;

    // undo
    void StopUndoCapture();
    void StartUndoJob();
    void RunUndoJob();

    EngineConfig cfg_;
    Track        tracks_[kMaxTracks];
    int          num_tracks_ = 0;
    int16_t*     undo_l_     = nullptr;
    int16_t*     undo_r_     = nullptr;
    RecordMode   mode_;

    // transport and grid (positions in MIDI clock ticks, 24 per beat)
    bool     playing_       = false;
    double   pos_           = 0.0;
    float    bpm_           = 120.f;
    double   tpf_           = 0.0; // internal ticks per frame
    int64_t  beat_index_    = -1;
    uint32_t beat_count_    = 0;
    bool     last_beat_bar_ = false;
    float    play_gain_     = 0.f;
    int      ticks_per_bar_ = 96;

    // external clock
    bool     ext_present_      = false;
    bool     ext_running_      = false;
    bool     ext_wait_first_   = false;
    int64_t  ext_next_tick_    = 0;
    double   ext_last_tick_    = 0.0;
    uint32_t ext_frames_since_ = 0;
    uint32_t ext_intervals_    = 0;
    double   ext_tpf_          = 0.0;

    // undo
    UndoKind undo_kind_    = UndoKind::None;
    int      undo_track_   = -1;
    bool     undo_done_    = false; // true = undone (next Undo redoes)
    Meta     undo_meta_;
    size_t   undo_start_   = 0; // overdub region in the track buffer
    size_t   undo_count_   = 0;
    bool     undo_capture_ = false; // overdub in progress: saving old audio
    bool     job_pending_  = false; // swap waits for the overdub to fade out
    bool     job_active_   = false;
    int      job_track_    = -1;
    size_t   job_start_    = 0;
    size_t   job_count_    = 0;
    size_t   job_len_      = 0;
    size_t   job_offset_   = 0;
    size_t   job_done_     = 0;

    float monitor_        = 1.f;
    float monitor_smooth_ = 1.f;
    uint32_t phase_corrections_ = 0;

    // derived constants
    float  smooth_coef_     = 0.f; // ~5 ms
    float  fast_coef_       = 0.f; // ~1 ms, transport start/stop
    size_t clock_timeout_   = 0;
    size_t min_take_frames_ = 0;
    size_t seam_fade_       = 0;
    size_t drift_threshold_ = 0;
    size_t xf_len_          = 0;
    size_t late_window_     = 0; // frames: a late "stop" press still ends on the line just passed
};

} // namespace looper
