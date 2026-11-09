#include "looper.h"

#include <cmath>

namespace looper
{
namespace
{
constexpr float kSilent = 1e-5f;

// Linear up to 0.8, then a smooth knee that never exceeds 1.0, so a loud
// pile-up of layers bends instead of clipping hard.
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

inline bool Audible(LayerState s)
{
    return s == LayerState::Playing || s == LayerState::Overdubbing;
}

inline bool HoldsAudio(LayerState s)
{
    return s == LayerState::Recording || s == LayerState::Overdubbing
           || s == LayerState::Playing || s == LayerState::Muted;
}
} // namespace

void Engine::Init(const EngineConfig& cfg,
                  float* const*       buf_l,
                  float* const*       buf_r,
                  int                 num_layers)
{
    cfg_        = cfg;
    num_layers_ = num_layers < kMaxLayers ? num_layers : kMaxLayers;
    for(int k = 0; k < num_layers_; ++k)
    {
        Layer& L      = layers_[k];
        L.l           = buf_l[k];
        L.r           = buf_r[k];
        L.state       = LayerState::Empty;
        L.volume      = 1.f;
        L.gain        = 0.f;
        L.rec_gain    = 0.f;
        L.dirty       = 0;
        L.want_zero   = false;
        L.zero_frames = 0;
        L.needs_zero.store(false);
    }

    const float sr   = cfg_.sample_rate;
    smooth_coef_     = 1.f - std::exp(-1.f / (0.005f * sr)); // ~5 ms
    ticks_per_bar_   = 24 * (cfg_.beats_per_bar > 0 ? cfg_.beats_per_bar : 4);
    clock_timeout_   = static_cast<uint32_t>(0.5f * sr);
    min_take_frames_ = static_cast<size_t>(0.1f * sr);
    seam_fade_       = static_cast<size_t>(0.002f * sr);
    drift_threshold_ = static_cast<size_t>(0.0015f * sr);
    xf_len_          = static_cast<size_t>(0.005f * sr);

    loop_len_          = 0;
    ph_                = 0;
    paused_            = false;
    play_gain_         = 0.f;
    reset_pending_     = false;
    loop_wraps_        = 0;
    recording_layer_   = -1;
    rec_pos_           = 0;
    stop_pending_      = false;
    finish_to_muted_   = false;
    first_take_synced_ = false;
    anchor_tick_       = 0;
    xf_from_           = 0;
    xf_left_           = 0;
    xf_total_          = 0;
    clock_present_     = false;
    transport_running_ = false;
    resume_on_tick_    = false;
    tick_count_        = 0;
    frames_since_tick_ = 0;
    bar_count_         = 0;
    synced_            = false;
    loop_ticks_        = 0;
    last_phase_error_  = 0;
    phase_corrections_ = 0;
    monitor_           = 1.f;
    monitor_smooth_    = 1.f;
}

// ------------------------------------------------------------------ actions

void Engine::RecPlayDub(int layer)
{
    if(layer < 0 || layer >= num_layers_)
        return;
    Layer& L = layers_[layer];
    switch(L.state)
    {
        case LayerState::Empty:
            if(recording_layer_ >= 0)
            {
                // A first take is running on another layer: close it, and
                // this layer starts overdubbing as soon as the loop exists.
                finish_to_muted_ = false;
                RequestFinishFirstTake();
            }
            else if(loop_len_ == 0)
            {
                DisarmFirstTakes(); // only one first take can be waiting
            }
            L.state = LayerState::Armed;
            ServiceArmed();
            break;
        case LayerState::Armed: L.state = LayerState::Empty; break;
        case LayerState::Recording:
            finish_to_muted_ = false;
            RequestFinishFirstTake();
            break;
        case LayerState::Playing: StartOverdub(layer); break;
        case LayerState::Overdubbing: L.state = LayerState::Playing; break;
        case LayerState::Muted:
            L.state = LayerState::Playing;
            paused_ = false;
            break;
    }
}

void Engine::StopToggle(int layer)
{
    if(layer < 0 || layer >= num_layers_)
        return;
    Layer& L = layers_[layer];
    switch(L.state)
    {
        case LayerState::Empty: break;
        case LayerState::Armed: L.state = LayerState::Empty; break;
        case LayerState::Recording:
            finish_to_muted_ = true;
            RequestFinishFirstTake();
            break;
        case LayerState::Overdubbing:
        case LayerState::Playing: L.state = LayerState::Muted; break;
        case LayerState::Muted:
            L.state = LayerState::Playing;
            paused_ = false;
            break;
    }
}

void Engine::Clear(int layer)
{
    if(layer < 0 || layer >= num_layers_)
        return;
    Layer& L = layers_[layer];
    if(layer == recording_layer_)
    {
        if(rec_pos_ > L.dirty)
            L.dirty = rec_pos_;
        recording_layer_ = -1;
        stop_pending_    = false;
    }
    L.state = LayerState::Empty;
    if(L.dirty > 0)
        L.want_zero = true; // zeroed once it has faded out (EndOfBlock)
    if(loop_len_ > 0 && !HasContent())
        reset_pending_ = true; // last layer gone: forget the loop length
}

void Engine::ClearAll()
{
    for(int k = 0; k < num_layers_; ++k)
        Clear(k);
}

void Engine::TogglePause()
{
    if(loop_len_ == 0 || reset_pending_)
        return;
    paused_ = !paused_;
    if(paused_)
        for(int k = 0; k < num_layers_; ++k)
            if(layers_[k].state == LayerState::Overdubbing)
                layers_[k].state = LayerState::Playing;
}

void Engine::SetVolume(int layer, float gain)
{
    if(layer >= 0 && layer < num_layers_)
        layers_[layer].volume = gain < 0.f ? 0.f : gain;
}

float Engine::Volume(int layer) const
{
    return (layer >= 0 && layer < num_layers_) ? layers_[layer].volume : 0.f;
}

void Engine::SetMonitor(float gain)
{
    monitor_ = gain < 0.f ? 0.f : gain;
}

// ----------------------------------------------------------- state changes

bool Engine::HasContent() const
{
    for(int k = 0; k < num_layers_; ++k)
        if(HoldsAudio(layers_[k].state))
            return true;
    return false;
}

bool Engine::SyncActive() const
{
    return cfg_.quantize_to_bars && clock_present_ && transport_running_;
}

bool Engine::ZeroReady(const Layer& L) const
{
    return !L.want_zero && !L.needs_zero.load(std::memory_order_acquire);
}

void Engine::ServiceArmed()
{
    if(reset_pending_)
        return;
    for(int k = 0; k < num_layers_; ++k)
    {
        Layer& L = layers_[k];
        if(L.state != LayerState::Armed || !ZeroReady(L))
            continue;
        if(loop_len_ > 0)
            StartOverdub(k);
        else if(recording_layer_ < 0 && !SyncActive())
            StartFirstTake(k); // with clock running, OnClockTick starts it on the bar
    }
}

void Engine::DisarmFirstTakes()
{
    for(int k = 0; k < num_layers_; ++k)
        if(layers_[k].state == LayerState::Armed)
            layers_[k].state = LayerState::Empty;
}

void Engine::StartFirstTake(int layer)
{
    recording_layer_   = layer;
    rec_pos_           = 0;
    stop_pending_      = false;
    finish_to_muted_   = false;
    first_take_synced_ = SyncActive();
    anchor_tick_       = tick_count_;
    ph_                = 0;
    paused_            = false;
    layers_[layer].state = LayerState::Recording;
}

void Engine::RequestFinishFirstTake()
{
    if(recording_layer_ < 0)
        return;
    if(first_take_synced_ && SyncActive())
        stop_pending_ = true; // OnClockTick finishes it on the next bar line
    else
        FinishFirstTake(false);
}

void Engine::FinishFirstTake(bool at_bar)
{
    Layer&       L   = layers_[recording_layer_];
    const size_t len = rec_pos_;
    if(len > L.dirty)
        L.dirty = len;
    recording_layer_ = -1;
    stop_pending_    = false;

    if(len < min_take_frames_)
    {
        // A double-tap, not a loop.
        L.state     = LayerState::Empty;
        L.want_zero = true;
        return;
    }

    // Short fades at both ends so the loop point doesn't click.
    size_t fade = seam_fade_ < len / 4 ? seam_fade_ : len / 4;
    for(size_t i = 0; i < fade; ++i)
    {
        const float g = static_cast<float>(i) / static_cast<float>(fade);
        L.l[i] *= g;
        L.r[i] *= g;
        L.l[len - 1 - i] *= g;
        L.r[len - 1 - i] *= g;
    }

    loop_len_   = len;
    ph_         = 0;
    xf_left_    = 0;
    paused_     = false;
    synced_     = at_bar && first_take_synced_;
    loop_ticks_ = synced_ ? tick_count_ - anchor_tick_ : 0;
    if(loop_ticks_ <= 0)
        synced_ = false;
    L.state          = finish_to_muted_ ? LayerState::Muted : LayerState::Playing;
    finish_to_muted_ = false;
    // Start at full level: the seam fade already prevents a click, and a
    // fade-in here would soften the first downbeat.
    play_gain_ = 1.f;
    if(L.state == LayerState::Playing)
        L.gain = L.volume;
    ServiceArmed(); // layers waiting for the loop start overdubbing now
}

void Engine::StartOverdub(int layer)
{
    Layer& L = layers_[layer];
    L.state  = LayerState::Overdubbing;
    if(L.dirty < loop_len_)
        L.dirty = loop_len_;
    paused_ = false;
}

void Engine::JumpTo(size_t target)
{
    if(loop_len_ == 0)
        return;
    target %= loop_len_;
    size_t len = xf_len_ < loop_len_ / 2 ? xf_len_ : loop_len_ / 2;
    if(len == 0)
    {
        ph_ = target;
        return;
    }
    xf_from_  = ph_;
    ph_       = target;
    xf_left_  = len;
    xf_total_ = len;
}

size_t Engine::ExpectedPhase() const
{
    if(!synced_ || loop_ticks_ <= 0 || loop_len_ == 0)
        return 0;
    int64_t t = (tick_count_ - anchor_tick_) % loop_ticks_;
    if(t < 0)
        t += loop_ticks_;
    return static_cast<size_t>(static_cast<uint64_t>(t) * loop_len_
                               / static_cast<uint64_t>(loop_ticks_));
}

// --------------------------------------------------------------- MIDI sync

void Engine::HandleSync(SyncType type)
{
    switch(type)
    {
        case SyncType::Clock: OnClockTick(); break;

        case SyncType::Start:
            tick_count_        = 0; // the next clock is beat 1 of bar 1
            transport_running_ = true;
            resume_on_tick_    = true;
            if(recording_layer_ >= 0)
                first_take_synced_ = false; // its bar count just restarted
            break;

        case SyncType::Continue:
            transport_running_ = true;
            resume_on_tick_    = true;
            break;

        case SyncType::Stop:
            transport_running_ = false;
            resume_on_tick_    = false;
            if(recording_layer_ >= 0)
            {
                first_take_synced_ = false;
                if(stop_pending_)
                    FinishFirstTake(false);
            }
            else if(loop_len_ == 0)
            {
                DisarmFirstTakes(); // they were waiting for a bar line
            }
            if(cfg_.follow_transport && loop_len_ > 0 && !reset_pending_)
            {
                paused_ = true;
                for(int k = 0; k < num_layers_; ++k)
                    if(layers_[k].state == LayerState::Overdubbing)
                        layers_[k].state = LayerState::Playing;
            }
            break;
    }
}

void Engine::OnClockTick()
{
    frames_since_tick_ = 0;
    clock_present_     = true;
    const bool bar     = (tick_count_ % ticks_per_bar_) == 0;
    if(bar && transport_running_)
        ++bar_count_;

    // First clock after Start/Continue: line the loop up with the clock.
    if(resume_on_tick_)
    {
        resume_on_tick_ = false;
        if(loop_len_ > 0 && !reset_pending_)
        {
            const size_t target = ExpectedPhase();
            if(!paused_)
                JumpTo(target);
            else if(cfg_.follow_transport)
            {
                ph_        = target;
                xf_left_   = 0;
                paused_    = false;
                play_gain_ = 1.f; // land on the downbeat at full level
            }
        }
    }

    // Bar-quantised start and end of the first take.
    if(bar && transport_running_ && cfg_.quantize_to_bars)
    {
        if(recording_layer_ >= 0)
        {
            if(stop_pending_ && first_take_synced_)
                FinishFirstTake(true);
        }
        else if(loop_len_ == 0 && !reset_pending_)
        {
            for(int k = 0; k < num_layers_; ++k)
            {
                if(layers_[k].state == LayerState::Armed
                   && ZeroReady(layers_[k]))
                {
                    StartFirstTake(k);
                    break;
                }
            }
        }
    }

    // Once a bar, pull a synced loop back into phase if the two clocks have
    // drifted apart (a short crossfade hides the jump).
    if(bar && synced_ && loop_len_ > 0 && transport_running_ && !paused_
       && !reset_pending_)
    {
        const size_t  target = ExpectedPhase();
        int64_t       d      = static_cast<int64_t>(ph_) - static_cast<int64_t>(target);
        const int64_t len    = static_cast<int64_t>(loop_len_);
        if(d > len / 2)
            d -= len;
        else if(d < -len / 2)
            d += len;
        last_phase_error_ = static_cast<int32_t>(d);
        if(d < 0)
            d = -d;
        if(d > static_cast<int64_t>(drift_threshold_))
        {
            JumpTo(target);
            ++phase_corrections_;
        }
    }

    ++tick_count_;
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
    ServiceArmed();
    const float a  = smooth_coef_;
    size_t      ev = 0;

    for(size_t i = 0; i < n; ++i)
    {
        while(ev < num_events && events[ev].frame <= i)
            HandleSync(events[ev++].type);

        if(clock_present_ && ++frames_since_tick_ > clock_timeout_)
        {
            // Clock went away (cable pulled, P-6 switched off).
            clock_present_     = false;
            transport_running_ = false;
            resume_on_tick_    = false;
            if(recording_layer_ >= 0)
            {
                first_take_synced_ = false;
                if(stop_pending_)
                    FinishFirstTake(false);
            }
            else if(loop_len_ == 0)
            {
                DisarmFirstTakes();
            }
        }

        const float il = in_l[i];
        const float ir = in_r[i];
        monitor_smooth_ += a * (monitor_ - monitor_smooth_);
        float ol = il * monitor_smooth_;
        float orr = ir * monitor_smooth_;

        if(recording_layer_ >= 0)
        {
            Layer& R      = layers_[recording_layer_];
            R.l[rec_pos_] = il;
            R.r[rec_pos_] = ir;
            if(++rec_pos_ >= cfg_.max_frames)
                FinishFirstTake(false); // out of memory: keep what we have
        }

        if(loop_len_ > 0)
        {
            play_gain_ += a * ((paused_ ? 0.f : 1.f) - play_gain_);
            if(paused_ && play_gain_ < kSilent)
                play_gain_ = 0.f;
            const bool  moving = !paused_ || play_gain_ > 0.f;
            const float xg     = xf_left_ ? static_cast<float>(xf_left_)
                                            / static_cast<float>(xf_total_)
                                          : 0.f;

            for(int k = 0; k < num_layers_; ++k)
            {
                Layer&     L       = layers_[k];
                const bool audible = Audible(L.state);
                L.gain += a * ((audible ? L.volume : 0.f) - L.gain);
                if(!audible && L.gain < kSilent)
                    L.gain = 0.f;
                const bool dub = L.state == LayerState::Overdubbing && moving;
                L.rec_gain += a * ((dub ? 1.f : 0.f) - L.rec_gain);
                if(!dub && L.rec_gain < kSilent)
                    L.rec_gain = 0.f;

                if(L.gain > 0.f)
                {
                    float sl = L.l[ph_];
                    float sr = L.r[ph_];
                    if(xf_left_)
                    {
                        sl += (L.l[xf_from_] - sl) * xg;
                        sr += (L.r[xf_from_] - sr) * xg;
                    }
                    const float g = L.gain * play_gain_;
                    ol += sl * g;
                    orr += sr * g;
                }
                // Read before write: the overdub is heard next time round,
                // not doubled with the live monitor now.
                if(L.rec_gain > 0.f && moving)
                {
                    L.l[ph_] += il * L.rec_gain;
                    L.r[ph_] += ir * L.rec_gain;
                }
            }

            if(moving)
            {
                if(++ph_ >= loop_len_)
                {
                    ph_ = 0;
                    ++loop_wraps_;
                }
                if(xf_left_)
                {
                    if(++xf_from_ >= loop_len_)
                        xf_from_ = 0;
                    --xf_left_;
                }
            }
        }

        out_l[i] = SoftClip(ol);
        out_r[i] = SoftClip(orr);
    }

    while(ev < num_events)
        HandleSync(events[ev++].type);
    EndOfBlock();
}

void Engine::EndOfBlock()
{
    bool all_silent = true;
    for(int k = 0; k < num_layers_; ++k)
    {
        Layer& L = layers_[k];
        if(L.gain > 0.f || L.rec_gain > 0.f)
            all_silent = false;
        // A cleared layer is handed over for zeroing once nothing reads or
        // writes it any more.
        const bool idle = L.state == LayerState::Empty || L.state == LayerState::Armed;
        if(L.want_zero && idle && L.gain == 0.f && L.rec_gain == 0.f)
        {
            L.want_zero   = false;
            L.zero_frames = L.dirty;
            L.dirty       = 0;
            if(L.zero_frames > 0)
                L.needs_zero.store(true, std::memory_order_release);
        }
    }
    if(reset_pending_ && all_silent)
    {
        reset_pending_ = false;
        loop_len_      = 0;
        ph_            = 0;
        xf_left_       = 0;
        synced_        = false;
        loop_ticks_    = 0;
        paused_        = false;
        play_gain_     = 0.f;
    }
}

// --------------------------------------------------------- buffer zeroing

bool Engine::NeedsZero(int layer) const
{
    return layers_[layer].needs_zero.load(std::memory_order_acquire);
}

size_t Engine::ZeroFrames(int layer) const
{
    return layers_[layer].zero_frames;
}

void Engine::MarkZeroed(int layer)
{
    layers_[layer].needs_zero.store(false, std::memory_order_release);
}

} // namespace looper
