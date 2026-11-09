#include "looper.h"

#include <cmath>

namespace looper
{
namespace
{
constexpr float  kSilent  = 1e-5f;
constexpr float  kScale   = 16384.f; // int16 full scale = 2.0 (6 dB headroom for overdubs)
constexpr float  kInv     = 1.f / kScale;
constexpr double kEps     = 1e-6;
constexpr size_t kSwapPerBlock = 2048; // undo swaps this many frames per Process() call

inline int16_t ToS16(float x)
{
    float v = x * kScale;
    if(v > 32767.f)
        v = 32767.f;
    else if(v < -32768.f)
        v = -32768.f;
    return static_cast<int16_t>(v >= 0.f ? v + 0.5f : v - 0.5f);
}

inline float FromS16(int16_t s)
{
    return static_cast<float>(s) * kInv;
}

// Linear up to 0.8, then a smooth knee that never exceeds 1.0, so a loud
// pile-up of tracks bends instead of clipping hard.
inline float SoftClip(float x)
{
    constexpr float t = 0.8f;
    constexpr float k = 1.f - t;
    if(x > t)
        return t + k * std::tanh((x - t) / k);
    if(x < -t)
        return -t + k * std::tanh((x + t) / k);
    return x;
}
} // namespace

void Engine::Init(const EngineConfig& cfg,
                  int16_t* const*     buf_l,
                  int16_t* const*     buf_r,
                  int16_t*            undo_l,
                  int16_t*            undo_r,
                  int                 num_tracks)
{
    cfg_        = cfg;
    num_tracks_ = num_tracks < kMaxTracks ? num_tracks : kMaxTracks;
    for(int t = 0; t < num_tracks_; ++t)
    {
        tracks_[t]   = Track();
        tracks_[t].l = buf_l[t];
        tracks_[t].r = buf_r[t];
    }
    undo_l_ = undo_l;
    undo_r_ = undo_r;
    mode_   = cfg.record_mode;

    const float sr   = cfg_.sample_rate;
    ticks_per_bar_   = 24 * (cfg_.beats_per_bar > 0 ? cfg_.beats_per_bar : 4);
    smooth_coef_     = 1.f - std::exp(-1.f / (0.005f * sr));
    fast_coef_       = 1.f - std::exp(-1.f / (0.001f * sr));
    clock_timeout_   = static_cast<size_t>(0.5f * sr);
    min_take_frames_ = static_cast<size_t>(0.05f * sr);
    seam_fade_       = static_cast<size_t>(0.002f * sr);
    drift_threshold_ = static_cast<size_t>(0.0015f * sr);
    xf_len_          = static_cast<size_t>(0.005f * sr);
    late_window_     = static_cast<size_t>(0.15f * sr);

    playing_       = false;
    pos_           = 0.0;
    beat_index_    = -1;
    beat_count_    = 0;
    last_beat_bar_ = false;
    play_gain_     = 0.f;
    bpm_           = cfg_.tempo;
    if(bpm_ < cfg_.min_tempo)
        bpm_ = cfg_.min_tempo;
    if(bpm_ > cfg_.max_tempo)
        bpm_ = cfg_.max_tempo;
    tpf_ = bpm_ * 24.0 / (60.0 * sr);

    ext_present_      = false;
    ext_running_      = false;
    ext_wait_first_   = false;
    ext_next_tick_    = 0;
    ext_last_tick_    = 0.0;
    ext_frames_since_ = 0;
    ext_intervals_    = 0;
    ext_tpf_          = 0.0;

    undo_kind_    = UndoKind::None;
    undo_track_   = -1;
    undo_done_    = false;
    undo_capture_ = false;
    job_pending_  = false;
    job_active_   = false;
    job_track_    = -1;

    monitor_           = 1.f;
    monitor_smooth_    = 1.f;
    phase_corrections_ = 0;
}

// ------------------------------------------------------------------ status

bool Engine::HoldsLoop(TrackState s) const
{
    return s == TrackState::Playing || s == TrackState::Muted
           || s == TrackState::Overdubbing;
}

double Engine::TicksPerFrame() const
{
    return (ExternalActive() && ext_tpf_ > 0.0) ? ext_tpf_ : tpf_;
}

float Engine::Tempo() const
{
    return static_cast<float>(TicksPerFrame() * 60.0 * cfg_.sample_rate / 24.0);
}

bool Engine::TempoLocked() const
{
    if(ExternalActive())
        return true;
    for(int t = 0; t < num_tracks_; ++t)
    {
        const Track& tr = tracks_[t];
        if(tr.grid && HoldsLoop(tr.state))
            return true;
        if(tr.state == TrackState::Recording && tr.rec_q != Quantize::Free)
            return true;
    }
    return false;
}

double Engine::LengthBeats(int track) const
{
    const Track& tr = tracks_[track];
    return tr.grid ? tr.len_ticks / 24.0 : 0.0;
}

// ----------------------------------------------------------------- actions

void Engine::RecPlayDub(int t)
{
    if(t < 0 || t >= num_tracks_)
        return;
    Track& tr = tracks_[t];
    switch(tr.state)
    {
        case TrackState::Empty:
            if(!playing_)
            {
                // Arm, and start the internal transport: recording begins on
                // its first beat. With external clock, wait for its Start.
                tr.state = TrackState::Armed;
                tr.arm_q = mode_.quantize;
                if(!ExternalActive())
                    StartTransport(0.0);
                return;
            }
            if(mode_.quantize == Quantize::Free || mode_.start == StartMode::Immediate)
            {
                StartTake(t, pos_, mode_.quantize);
                tr.anchor_on_next_frame = true; // exact grid position known once that frame runs
            }
            else
            {
                tr.state = TrackState::Armed;
                tr.arm_q = mode_.quantize;
            }
            break;

        case TrackState::Armed: tr.state = TrackState::Empty; break;

        case TrackState::Recording:
            tr.finish_to_muted = false;
            RequestFinish(t);
            break;

        case TrackState::Playing:
            if(!playing_ && !ExternalActive())
                StartTransport(0.0);
            if(!playing_)
                break; // waiting for the external clock's Start
            if(UndoBusy())
            {
                tr.dub_after_job = true; // an undo is still being applied
                break;
            }
            StartOverdub(t);
            break;

        case TrackState::Overdubbing: tr.state = TrackState::Playing; break;

        case TrackState::Muted: tr.state = TrackState::Playing; break;
    }
}

void Engine::StopToggle(int t)
{
    if(t < 0 || t >= num_tracks_)
        return;
    Track& tr = tracks_[t];
    switch(tr.state)
    {
        case TrackState::Empty: break;
        case TrackState::Armed: tr.state = TrackState::Empty; break;
        case TrackState::Recording:
            tr.finish_to_muted = true;
            RequestFinish(t);
            break;
        case TrackState::Overdubbing:
        case TrackState::Playing: tr.state = TrackState::Muted; break;
        case TrackState::Muted: tr.state = TrackState::Playing; break;
    }
}

void Engine::Clear(int t)
{
    if(t < 0 || t >= num_tracks_)
        return;
    if(UndoBusy() && job_track_ == t)
        return; // an undo is still being applied to this track
    Track& tr = tracks_[t];
    switch(tr.state)
    {
        case TrackState::Empty: return;
        case TrackState::Armed:
        case TrackState::Recording:
            // An unfinished take just goes away.
            tr.state       = TrackState::Empty;
            tr.len         = 0;
            tr.end_pending = false;
            return;
        default: break;
    }
    if(undo_capture_ && undo_track_ == t)
        undo_capture_ = false;
    undo_kind_       = UndoKind::Clear;
    undo_track_      = t;
    undo_done_       = false;
    undo_meta_       = SaveMeta(tr);
    tr.state         = TrackState::Empty; // len is kept: it fades out, and undo can bring it back
    tr.dub_after_job = false;
}

void Engine::StartFresh()
{
    for(int t = 0; t < num_tracks_; ++t)
    {
        Track& tr        = tracks_[t];
        if(tr.state == TrackState::Recording)
            tr.len = 0;
        tr.state         = TrackState::Empty;
        tr.end_pending   = false;
        tr.dub_after_job = false;
    }
    undo_kind_    = UndoKind::None;
    undo_capture_ = false;
    job_pending_  = false;
    job_active_   = false;
    if(!ExternalActive())
    {
        playing_    = false;
        pos_        = 0.0;
        beat_index_ = -1;
    }
}

bool Engine::Undo()
{
    // Undo while recording cancels the take(s) in progress.
    bool cancelled = false;
    for(int t = 0; t < num_tracks_; ++t)
    {
        Track& tr = tracks_[t];
        if(tr.state == TrackState::Recording || tr.state == TrackState::Armed)
        {
            tr.state       = TrackState::Empty;
            tr.len         = 0;
            tr.end_pending = false;
            cancelled      = true;
        }
    }
    if(cancelled)
        return true;
    if(UndoBusy())
        return false;

    switch(undo_kind_)
    {
        case UndoKind::None: return false;

        case UndoKind::Take:
        {
            Track& tr = tracks_[undo_track_];
            if(!undo_done_)
            {
                undo_meta_ = SaveMeta(tr);
                tr.state   = TrackState::Empty;
            }
            else
            {
                if(!FitsTempo(undo_meta_))
                    return false; // tempo changed since
                RestoreMeta(tr, undo_meta_);
            }
            undo_done_ = !undo_done_;
            return true;
        }

        case UndoKind::Clear:
        {
            Track& tr = tracks_[undo_track_];
            if(!undo_done_)
            {
                if(!FitsTempo(undo_meta_))
                    return false; // tempo changed since
                RestoreMeta(tr, undo_meta_);
            }
            else
                tr.state = TrackState::Empty;
            undo_done_ = !undo_done_;
            return true;
        }

        case UndoKind::Overdub:
        {
            Track& tr = tracks_[undo_track_];
            if(tr.state == TrackState::Overdubbing)
                tr.state = TrackState::Playing;
            // The swap starts once the overdub has faded out (RunUndoJob).
            job_pending_ = true;
            job_track_   = undo_track_;
            return true;
        }
    }
    return false;
}

bool Engine::TransportToggle()
{
    if(ExternalActive())
        return false;
    if(playing_)
        StopTransport();
    else
        StartTransport(0.0);
    return true;
}

bool Engine::SetTempo(float bpm)
{
    if(TempoLocked())
        return false;
    if(bpm < cfg_.min_tempo)
        bpm = cfg_.min_tempo;
    if(bpm > cfg_.max_tempo)
        bpm = cfg_.max_tempo;
    bpm_ = bpm;
    tpf_ = bpm_ * 24.0 / (60.0 * cfg_.sample_rate);
    return true;
}

void Engine::SetVolume(int t, float gain)
{
    if(t >= 0 && t < num_tracks_)
        tracks_[t].volume = gain < 0.f ? 0.f : gain;
}

float Engine::Volume(int t) const
{
    return (t >= 0 && t < num_tracks_) ? tracks_[t].volume : 0.f;
}

void Engine::SetMonitor(float gain)
{
    monitor_ = gain < 0.f ? 0.f : gain;
}

// ------------------------------------------------------- transport / grid

void Engine::StartTransport(double pos)
{
    playing_ = true;
    pos_     = pos;
    // So that a start exactly on a beat counts that beat.
    beat_index_ = static_cast<int64_t>(std::ceil(pos / 24.0 - kEps)) - 1;
    undo_capture_ = false; // re-phasing below would break an overdub's undo record
    for(int t = 0; t < num_tracks_; ++t)
    {
        Track& tr = tracks_[t];
        if(tr.len > 0 && HoldsLoop(tr.state))
            Rephase(tr);
    }
}

void Engine::StopTransport()
{
    playing_ = false;
    for(int t = 0; t < num_tracks_; ++t)
    {
        Track& tr = tracks_[t];
        if(tr.state == TrackState::Overdubbing)
            tr.state = TrackState::Playing;
        else if(tr.state == TrackState::Recording)
            CutTakeShort(t);
    }
}

void Engine::OnBeat(int64_t beat)
{
    const int bpb  = ticks_per_bar_ / 24;
    ++beat_count_;
    last_beat_bar_ = (beat % bpb) == 0;

    // Armed takes start on their next beat or bar line.
    for(int t = 0; t < num_tracks_; ++t)
    {
        Track& tr = tracks_[t];
        if(tr.state == TrackState::Armed
           && (tr.arm_q != Quantize::Bar || last_beat_bar_))
            StartTake(t, static_cast<double>(beat) * 24.0, tr.arm_q);
    }

    // Keep grid tracks in phase (matters with external clock; with the
    // internal clock they only drift by rounding, well under a frame per loop).
    for(int t = 0; t < num_tracks_; ++t)
    {
        Track& tr = tracks_[t];
        if(!tr.grid || tr.len == 0 || tr.xf_left != 0 || tr.rec_gain != 0.f
           || !(tr.state == TrackState::Playing || tr.state == TrackState::Muted))
            continue;
        const size_t  target = ExpectedPhase(tr);
        const int64_t len    = static_cast<int64_t>(tr.len);
        int64_t       d      = static_cast<int64_t>(tr.ph) - static_cast<int64_t>(target);
        if(d > len / 2)
            d -= len;
        else if(d < -len / 2)
            d += len;
        tr.last_error = static_cast<int32_t>(d);
        if((d < 0 ? -d : d) > static_cast<int64_t>(drift_threshold_))
        {
            JumpTo(tr, target);
            ++phase_corrections_;
        }
    }
}

size_t Engine::ExpectedPhase(const Track& tr) const
{
    if(!tr.grid || tr.len_ticks <= 0.0 || tr.len == 0)
        return 0; // free tracks restart from the top
    double x = (pos_ - tr.anchor) / tr.len_ticks;
    x -= std::floor(x);
    size_t p = static_cast<size_t>(x * static_cast<double>(tr.len) + 0.5);
    return p < tr.len ? p : p - tr.len;
}

void Engine::Rephase(Track& tr)
{
    tr.ph      = ExpectedPhase(tr);
    tr.xf_left = 0;
}

void Engine::JumpTo(Track& tr, size_t target)
{
    if(tr.len == 0)
        return;
    target %= tr.len;
    const size_t len = xf_len_ < tr.len / 2 ? xf_len_ : tr.len / 2;
    if(len == 0)
    {
        tr.ph = target;
        return;
    }
    tr.xf_from  = tr.ph;
    tr.ph       = target;
    tr.xf_left  = len;
    tr.xf_total = len;
}

// ------------------------------------------------------------ external clock

void Engine::HandleSync(SyncType type)
{
    if(!cfg_.follow_clock)
        return;
    switch(type)
    {
        case SyncType::Clock: OnExternalTick(); break;
        case SyncType::Start:
            ext_running_    = true;
            ext_wait_first_ = true; // the next clock is beat 1 of bar 1
            ext_next_tick_  = 0;
            break;
        case SyncType::Continue:
            ext_running_    = true;
            ext_wait_first_ = true;
            break;
        case SyncType::Stop:
            ext_running_    = false;
            ext_wait_first_ = false;
            if(playing_ && ext_present_)
                StopTransport();
            break;
    }
}

void Engine::OnExternalTick()
{
    const bool was_present = ext_present_;
    if(was_present && ext_frames_since_ > 0)
    {
        // Tempo: average the first few intervals, then follow slowly.
        const double inst = 1.0 / static_cast<double>(ext_frames_since_);
        ++ext_intervals_;
        const double k = ext_intervals_ < 8 ? 1.0 / ext_intervals_ : 0.05;
        ext_tpf_ += k * (inst - ext_tpf_);
    }
    if(!was_present)
    {
        // External clock takes over from the internal one.
        ext_intervals_ = 0;
        ext_tpf_       = tpf_;
        if(playing_)
            StopTransport();
    }
    ext_present_      = true;
    ext_frames_since_ = 0;

    if(ext_running_)
    {
        ext_last_tick_ = static_cast<double>(ext_next_tick_++);
        if(ext_wait_first_)
        {
            ext_wait_first_ = false;
            StartTransport(ext_last_tick_);
        }
        else if(playing_)
            pos_ = ext_last_tick_; // snap to the clock
    }
}

void Engine::LoseExternalClock()
{
    ext_present_    = false;
    ext_running_    = false;
    ext_wait_first_ = false;
    // Carry the tempo over, so tracks recorded to the clock still fit.
    if(ext_tpf_ > 0.0)
    {
        tpf_ = ext_tpf_;
        bpm_ = static_cast<float>(tpf_ * 60.0 * cfg_.sample_rate / 24.0);
    }
    if(playing_)
        StopTransport();
}

// ------------------------------------------------------------------ takes

void Engine::StartTake(int t, double anchor, Quantize q)
{
    // The take overwrites this track's buffer, so an undo record that
    // depends on it is no longer valid.
    if(undo_track_ == t && undo_kind_ != UndoKind::None)
    {
        undo_kind_    = UndoKind::None;
        undo_capture_ = false;
    }
    Track& tr          = tracks_[t];
    tr.state           = TrackState::Recording;
    tr.rec_q           = q;
    tr.unit            = q == Quantize::Bar ? static_cast<double>(ticks_per_bar_) : 24.0;
    tr.units           = 0;
    tr.anchor          = anchor;
    tr.next_unit       = anchor + tr.unit;
    tr.rec_pos         = 0;
    tr.rec_pos_at_unit = 0;
    tr.end_pending     = false;
    tr.finish_to_muted = false;
    tr.dub_after_job   = false;
    tr.len             = 0;
    tr.ph              = 0;
    tr.grid            = false;
    tr.gain            = 0.f;
    tr.rec_gain        = 0.f;
    tr.xf_left         = 0;
    tr.anchor_on_next_frame = false;
}

void Engine::RequestFinish(int t)
{
    Track& tr = tracks_[t];
    if(tr.rec_q == Quantize::Free)
    {
        FinishTake(t, tr.rec_pos, 0);
        return;
    }
    // Pressed just after a line: end on that line and keep playing from it.
    // Otherwise keep recording to the next one.
    const size_t since       = tr.rec_pos - tr.rec_pos_at_unit;
    const size_t unit_frames = static_cast<size_t>(tr.unit / TicksPerFrame());
    const size_t window = late_window_ < unit_frames / 4 ? late_window_ : unit_frames / 4;
    if(tr.units >= 1 && since <= window)
        FinishTake(t, tr.rec_pos_at_unit, since);
    else
    {
        tr.end_pending = true;
        tr.end_units   = tr.units + 1;
    }
}

void Engine::CutTakeShort(int t)
{
    Track& tr = tracks_[t];
    if(tr.rec_q == Quantize::Free)
        FinishTake(t, tr.rec_pos, 0);
    else if(tr.units >= 1)
        FinishTake(t, tr.rec_pos_at_unit, tr.rec_pos - tr.rec_pos_at_unit);
    else
    {
        tr.state       = TrackState::Empty; // not even one beat/bar: drop it
        tr.len         = 0;
        tr.end_pending = false;
    }
}

void Engine::FinishTake(int t, size_t len, size_t over)
{
    Track& tr      = tracks_[t];
    tr.end_pending = false;
    if(len < min_take_frames_)
    {
        tr.state = TrackState::Empty; // a double-tap, not a loop
        tr.len   = 0;
        return;
    }

    // Short fades at both ends so the loop point doesn't click.
    const size_t fade = seam_fade_ < len / 4 ? seam_fade_ : len / 4;
    for(size_t i = 0; i < fade; ++i)
    {
        const float g      = static_cast<float>(i) / static_cast<float>(fade);
        tr.l[i]            = static_cast<int16_t>(tr.l[i] * g);
        tr.r[i]            = static_cast<int16_t>(tr.r[i] * g);
        tr.l[len - 1 - i]  = static_cast<int16_t>(tr.l[len - 1 - i] * g);
        tr.r[len - 1 - i]  = static_cast<int16_t>(tr.r[len - 1 - i] * g);
    }

    tr.len     = len;
    tr.ph      = over % len;
    tr.xf_left = 0;
    tr.rec_tpf = TicksPerFrame();
    if(tr.rec_q == Quantize::Free)
    {
        tr.grid      = false;
        tr.len_ticks = 0.0;
    }
    else
    {
        tr.grid      = true;
        tr.len_ticks = static_cast<double>(tr.units) * tr.unit;
    }
    tr.state = tr.finish_to_muted ? TrackState::Muted : TrackState::Playing;
    tr.finish_to_muted = false;
    // Start at full level: the seam fade already prevents a click, and a
    // fade-in would soften the first downbeat.
    tr.gain = tr.state == TrackState::Playing ? tr.volume : 0.f;

    undo_kind_    = UndoKind::Take;
    undo_track_   = t;
    undo_done_    = false;
    undo_capture_ = false;
}

void Engine::StartOverdub(int t)
{
    Track& tr = tracks_[t];
    tr.state  = TrackState::Overdubbing;
    // One level of undo: save the audio this overdub replaces, as it goes.
    undo_kind_    = UndoKind::Overdub;
    undo_track_   = t;
    undo_done_    = false;
    undo_start_   = tr.ph;
    undo_count_   = 0;
    undo_capture_ = true;
}

Engine::Meta Engine::SaveMeta(const Track& tr) const
{
    Meta m;
    m.state     = tr.state == TrackState::Overdubbing ? TrackState::Playing : tr.state;
    m.len       = tr.len;
    m.grid      = tr.grid;
    m.anchor    = tr.anchor;
    m.len_ticks = tr.len_ticks;
    m.tpf       = tr.rec_tpf;
    return m;
}

bool Engine::FitsTempo(const Meta& m) const
{
    // A grid track only fits the bars at the tempo it was recorded at.
    return !m.grid || m.tpf <= 0.0 || std::fabs(m.tpf / TicksPerFrame() - 1.0) < 0.005;
}

void Engine::RestoreMeta(Track& tr, const Meta& m)
{
    tr.state       = m.state;
    tr.len         = m.len;
    tr.grid        = m.grid;
    tr.anchor      = m.anchor;
    tr.len_ticks   = m.len_ticks;
    tr.rec_tpf     = m.tpf;
    tr.end_pending = false;
    tr.gain        = 0.f;
    Rephase(tr);
}

// -------------------------------------------------------------------- undo

void Engine::StartUndoJob()
{
    job_pending_    = false;
    const Track& tr = tracks_[job_track_];
    job_start_      = undo_start_;
    job_len_        = tr.len;
    job_count_      = undo_count_ < tr.len ? undo_count_ : tr.len;
    job_done_       = 0;
    undo_done_      = !undo_done_;
    if(job_count_ == 0 || job_len_ == 0)
        return;
    // Swap the part the playhead is about to reach first; the swap runs
    // far faster than playback, so nothing stale is ever heard.
    const size_t off = (tr.ph + job_len_ - job_start_ % job_len_) % job_len_;
    job_offset_      = off < job_count_ ? off : 0;
    job_active_      = true;
}

void Engine::RunUndoJob()
{
    if(job_pending_)
    {
        const Track& tr = tracks_[job_track_];
        if(tr.rec_gain == 0.f && tr.state != TrackState::Overdubbing)
        {
            undo_capture_ = false;
            StartUndoJob();
        }
    }
    if(job_active_)
    {
        Track& tr = tracks_[job_track_];
        size_t n  = job_count_ - job_done_;
        if(n > kSwapPerBlock)
            n = kSwapPerBlock;
        for(size_t k = 0; k < n; ++k)
        {
            const size_t p   = (job_offset_ + job_done_) % job_count_;
            const size_t idx = (job_start_ + p) % job_len_;
            const int16_t sl = tr.l[idx];
            const int16_t sr = tr.r[idx];
            tr.l[idx]        = undo_l_[idx];
            tr.r[idx]        = undo_r_[idx];
            undo_l_[idx]     = sl;
            undo_r_[idx]     = sr;
            ++job_done_;
        }
        if(job_done_ >= job_count_)
            job_active_ = false;
    }
    if(!UndoBusy())
    {
        // Overdubs requested while the undo was running start now.
        for(int t = 0; t < num_tracks_; ++t)
        {
            Track& tr = tracks_[t];
            if(tr.dub_after_job)
            {
                tr.dub_after_job = false;
                if(tr.state == TrackState::Playing && playing_)
                    StartOverdub(t);
            }
        }
    }
}

// ------------------------------------------------------------------- audio

void Engine::Process(const float*     in_l,
                     const float*     in_r,
                     float*           out_l,
                     float*           out_r,
                     size_t           n,
                     const SyncEvent* events,
                     size_t           num_events)
{
    const float a  = smooth_coef_;
    const float af = fast_coef_;
    size_t      ev = 0;

    for(size_t i = 0; i < n; ++i)
    {
        while(ev < num_events && events[ev].frame <= i)
            HandleSync(events[ev++].type);

        if(ext_present_ && ext_frames_since_ > clock_timeout_)
            LoseExternalClock(); // cable pulled, P-6 switched off...

        // pos_ is this frame's place on the grid (set at the end of the last
        // frame, or snapped by a clock that just arrived).
        if(playing_)
        {
            const int64_t beat = static_cast<int64_t>(std::floor(pos_ / 24.0 + kEps));
            if(beat > beat_index_)
            {
                beat_index_ = beat;
                OnBeat(beat);
            }
            // Takes in progress: count beat/bar lines, end a pending take.
            for(int t = 0; t < num_tracks_; ++t)
            {
                Track& tr = tracks_[t];
                if(tr.state != TrackState::Recording)
                    continue;
                if(tr.anchor_on_next_frame)
                {
                    tr.anchor_on_next_frame = false;
                    tr.anchor               = pos_;
                    tr.next_unit            = pos_ + tr.unit;
                }
                if(tr.rec_q == Quantize::Free || pos_ + kEps < tr.next_unit)
                    continue;
                ++tr.units;
                tr.rec_pos_at_unit = tr.rec_pos;
                tr.next_unit = tr.anchor + static_cast<double>(tr.units + 1) * tr.unit;
                if(tr.end_pending && tr.units >= tr.end_units)
                    FinishTake(t, tr.rec_pos, 0);
            }
        }

        const float il = in_l[i];
        const float ir = in_r[i];
        monitor_smooth_ += a * (monitor_ - monitor_smooth_);
        float ol  = il * monitor_smooth_;
        float orr = ir * monitor_smooth_;

        play_gain_ += af * ((playing_ ? 1.f : 0.f) - play_gain_);
        if(!playing_ && play_gain_ < kSilent)
            play_gain_ = 0.f;
        const bool moving = playing_ || play_gain_ > 0.f;

        for(int t = 0; t < num_tracks_; ++t)
        {
            Track& tr = tracks_[t];
            if(tr.state == TrackState::Recording)
            {
                tr.l[tr.rec_pos] = ToS16(il);
                tr.r[tr.rec_pos] = ToS16(ir);
                if(++tr.rec_pos >= cfg_.max_frames)
                    CutTakeShort(t); // memory full
                continue;
            }
            if(tr.len == 0)
                continue;

            const bool audible = tr.state == TrackState::Playing
                                 || tr.state == TrackState::Overdubbing;
            tr.gain += a * ((audible ? tr.volume : 0.f) - tr.gain);
            if(!audible && tr.gain < kSilent)
                tr.gain = 0.f;
            const bool dub = tr.state == TrackState::Overdubbing && moving;
            tr.rec_gain += a * ((dub ? 1.f : 0.f) - tr.rec_gain);
            if(!dub && tr.rec_gain < kSilent)
                tr.rec_gain = 0.f;

            if(tr.gain > 0.f)
            {
                float sl = FromS16(tr.l[tr.ph]);
                float sr = FromS16(tr.r[tr.ph]);
                if(tr.xf_left)
                {
                    const float xg = static_cast<float>(tr.xf_left)
                                     / static_cast<float>(tr.xf_total);
                    sl += (FromS16(tr.l[tr.xf_from]) - sl) * xg;
                    sr += (FromS16(tr.r[tr.xf_from]) - sr) * xg;
                }
                const float g = tr.gain * play_gain_;
                ol += sl * g;
                orr += sr * g;
            }
            // Read before write: the overdub is heard next time round, not
            // doubled with the live monitor now.
            if(tr.rec_gain > 0.f && moving)
            {
                if(undo_capture_ && undo_track_ == t && undo_count_ < tr.len)
                {
                    undo_l_[tr.ph] = tr.l[tr.ph];
                    undo_r_[tr.ph] = tr.r[tr.ph];
                    ++undo_count_;
                }
                tr.l[tr.ph] = ToS16(FromS16(tr.l[tr.ph]) + il * tr.rec_gain);
                tr.r[tr.ph] = ToS16(FromS16(tr.r[tr.ph]) + ir * tr.rec_gain);
            }
            if(undo_capture_ && undo_track_ == t && tr.rec_gain == 0.f
               && tr.state != TrackState::Overdubbing)
                undo_capture_ = false; // overdub over and faded out

            if(moving && (HoldsLoop(tr.state) || tr.gain > 0.f))
            {
                if(++tr.ph >= tr.len)
                    tr.ph = 0;
                if(tr.xf_left)
                {
                    if(++tr.xf_from >= tr.len)
                        tr.xf_from = 0;
                    --tr.xf_left;
                }
            }
        }

        out_l[i] = SoftClip(ol);
        out_r[i] = SoftClip(orr);

        // Advance to the next frame's grid position.
        if(ext_frames_since_ < 0x7fffffffu)
            ++ext_frames_since_;
        if(playing_)
        {
            if(ExternalActive())
            {
                double frac = static_cast<double>(ext_frames_since_) * ext_tpf_;
                if(frac > 0.999)
                    frac = 0.999; // never run past a clock that hasn't arrived
                pos_ = ext_last_tick_ + frac;
            }
            else
                pos_ += tpf_;
        }
    }

    while(ev < num_events)
        HandleSync(events[ev++].type);
    RunUndoJob();
}

} // namespace looper
