#include "synth.h"

#include <cmath>

namespace synth
{
namespace
{
constexpr float kTwoPi        = 6.28318530718f;
constexpr int   kControlEvery = 16;    // envelopes, pitch and filter update rate
constexpr float kVoiceGain    = 0.22f; // so a few notes sit comfortably below clipping

// clang-format off
const Patch kPatchTable[kPatches] = {
    // Blade: the slow-swelling CS-80 brass.
    {"Blade",
     1.0f, 0.7f, 0.5f, 0.15f, 9.f, 0.02f,
     120.f, 450.f, 0.20f, 3.0f, 0.5f, 1.5f,
     0.0f, 1.0f, 0.18f, 1.4f, 0.55f, 1.2f,
     0.06f, 1.0f, 0.85f, 1.4f, 0.5f,
     5.2f, 14.f, 0.5f, 0.55f, 0.8f},
    // Strings: slow, wide, chorused pad.
    {"Strings",
     1.0f, 0.6f, 0.5f, 0.25f, 14.f, 0.0f,
     250.f, 1800.f, 0.10f, 0.8f, 0.3f, 0.6f,
     0.3f, 1.0f, 0.8f, 2.0f, 0.7f, 2.0f,
     0.7f, 1.5f, 0.95f, 2.2f, 0.3f,
     4.6f, 6.f, 0.8f, 0.9f, 0.7f},
    // Bass: round and punchy.
    {"Bass",
     1.0f, 0.9f, 0.35f, 0.0f, 4.f, 0.0f,
     30.f, 180.f, 0.35f, 3.5f, 0.6f, 1.2f,
     0.0f, 1.0f, 0.003f, 0.35f, 0.15f, 0.15f,
     0.003f, 0.6f, 0.75f, 0.18f, 0.7f,
     5.0f, 0.f, 0.f, 0.15f, 0.9f},
    // Bells: glassy, resonant, decaying.
    {"Bells",
     0.4f, 1.0f, 0.12f, 0.05f, 6.f, 0.03f,
     300.f, 900.f, 0.65f, 3.0f, 0.8f, 1.0f,
     0.0f, 1.0f, 0.002f, 0.9f, 0.1f, 0.8f,
     0.002f, 1.8f, 0.0f, 1.2f, 0.6f,
     5.5f, 4.f, 0.2f, 0.6f, 0.8f},
};
// clang-format on

inline float PolyBlep(float t, float dt)
{
    if(t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.f;
    }
    if(t > 1.f - dt)
    {
        t = (t - 1.f) / dt;
        return t * t + t + t + 1.f;
    }
    return 0.f;
}

// Gentle saturation in front of the filter (a rational tanh).
inline float SoftSat(float x)
{
    if(x > 3.f)
        return 1.f;
    if(x < -3.f)
        return -1.f;
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

inline float Clamp(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

// One-pole coefficient that gets ~98 % of the way in `seconds`, stepped at `rate`.
inline float Coef(float seconds, float rate, float time_constants = 4.f)
{
    if(seconds < 1e-4f)
        seconds = 1e-4f;
    return 1.f - std::exp(-time_constants / (seconds * rate));
}
} // namespace

const Patch& GetPatch(int index)
{
    if(index < 0)
        index = 0;
    if(index >= kPatches)
        index = kPatches - 1;
    return kPatchTable[index];
}

void Synth::Init(float sample_rate)
{
    sr_ = sample_rate;
    for(Voice& v : voices_)
        v = Voice();
    age_counter_   = 0;
    brightness_    = 0.5f;
    mod_wheel_     = 0.f;
    bend_          = 0.f;
    pressure_      = 0.f;
    res_offset_    = 0.f;
    sustain_       = false;
    lfo_phase_     = 0.f;
    pwm_phase_     = 0.f;
    noise_         = 22222u;
    control_count_ = 0;
    for(float& s : chorus_buf_)
        s = 0.f;
    chorus_w_      = 0;
    ch_phase_slow_ = 0.f;
    ch_phase_fast_ = 0.f;
    SetPatch(0);
}

void Synth::SetPatch(int index)
{
    patch_index_ = index < 0 ? 0 : (index >= kPatches ? kPatches - 1 : index);
    p_           = &kPatchTable[patch_index_];
    const float cr = sr_ / kControlEvery;
    // The attack aims past 1.0 and stops there, like an analogue envelope:
    // 1.79 time constants to reach 1.0 when aiming at 1.2.
    amp_att_c_ = Coef(p_->attack, sr_, 1.79f);
    amp_dec_c_ = Coef(p_->decay, sr_);
    amp_rel_c_ = Coef(p_->release, sr_);
    f_att_c_   = Coef(p_->f_attack, cr, 3.f);
    f_dec_c_   = Coef(p_->f_decay, cr);
    f_rel_c_   = Coef(p_->f_release, cr);
    hp_a_      = 1.f / (1.f + kTwoPi * p_->hpf / sr_);
    detune_    = std::exp2(p_->detune * 0.5f / 1200.f);
}

void Synth::SetBrightness(float b)
{
    brightness_ = Clamp(b, 0.f, 1.f);
}
void Synth::SetModWheel(float v)
{
    mod_wheel_ = Clamp(v, 0.f, 1.f);
}
void Synth::SetPitchBend(float v)
{
    bend_ = Clamp(v, -1.f, 1.f);
}
void Synth::SetPressure(float v)
{
    pressure_ = Clamp(v, 0.f, 1.f);
}
void Synth::SetResonanceOffset(float v)
{
    res_offset_ = Clamp(v, -0.5f, 0.5f);
}

void Synth::SetPolyPressure(uint8_t note, float v)
{
    for(Voice& vo : voices_)
        if(vo.active && vo.note == note)
            vo.pressure = Clamp(v, 0.f, 1.f);
}

void Synth::SetSustain(bool down)
{
    sustain_ = down;
    if(!down)
        for(Voice& v : voices_)
            if(v.active && v.sustained)
            {
                v.sustained = false;
                Release(v);
            }
}

int Synth::ActiveVoices() const
{
    int n = 0;
    for(const Voice& v : voices_)
        n += v.active ? 1 : 0;
    return n;
}

void Synth::NoteOn(uint8_t note, uint8_t velocity, float max_hold_s)
{
    if(velocity == 0)
    {
        NoteOff(note);
        return;
    }
    // Same note again, else a free voice, else the oldest released one,
    // else the oldest of all.
    Voice* v = nullptr;
    for(Voice& c : voices_)
        if(c.active && c.note == note)
            v = &c;
    if(!v)
        for(Voice& c : voices_)
            if(!c.active)
            {
                v = &c;
                break;
            }
    if(!v)
    {
        for(Voice& c : voices_)
            if(c.amp_stage == 2 && (!v || c.age < v->age))
                v = &c;
        if(!v)
            for(Voice& c : voices_)
                if(!v || c.age < v->age)
                    v = &c;
    }

    const bool fresh = !v->active;
    v->active    = true;
    v->key_down  = true;
    v->sustained = false;
    v->note      = note;
    v->vel       = velocity / 127.f;
    v->pressure  = 0.f;
    v->age       = ++age_counter_;
    v->hold_left = max_hold_s > 0.f ? static_cast<uint32_t>(max_hold_s * sr_) : 0;
    v->amp_stage = 0; // the attack carries on from the current level: no click on a steal
    v->f_stage   = 0;
    v->fenv      = p_->f_initial;
    v->since_on  = 0.f;
    if(fresh)
    {
        // Free-running analogue oscillators: start anywhere.
        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;
        v->ph1  = (noise_ & 0xffff) / 65536.f;
        v->ph2  = (noise_ >> 16) / 65536.f;
        v->amp  = 0.f;
        v->ic1  = v->ic2 = 0.f;
        v->hp_x = v->hp_y = 0.f;
    }
    UpdateControl(*v, 0.f);
}

void Synth::NoteOff(uint8_t note)
{
    for(Voice& v : voices_)
    {
        if(!v.active || !v.key_down || v.note != note)
            continue;
        v.key_down = false;
        if(sustain_)
            v.sustained = true;
        else
            Release(v);
    }
}

void Synth::AllNotesOff()
{
    for(Voice& v : voices_)
    {
        if(!v.active)
            continue;
        v.key_down  = false;
        v.sustained = false;
        Release(v);
    }
}

void Synth::Release(Voice& v)
{
    v.amp_stage = 2;
    v.f_stage   = 2;
}

void Synth::UpdateControl(Voice& v, float dt)
{
    const Patch& p = *p_;
    if(dt > 0.f)
    {
        v.since_on += dt;
        switch(v.f_stage)
        {
            case 0:
                v.fenv += (p.f_peak - v.fenv) * f_att_c_;
                if(std::fabs(p.f_peak - v.fenv) < 0.01f)
                    v.f_stage = 1;
                break;
            case 1: v.fenv += (p.f_sustain - v.fenv) * f_dec_c_; break;
            default: v.fenv += (p.f_initial - v.fenv) * f_rel_c_; break;
        }
    }

    // Pitch: delayed vibrato, plus the mod wheel and aftertouch.
    const float press  = pressure_ + v.pressure;
    const float fade   = Clamp((v.since_on - p.vib_delay) / 0.5f, 0.f, 1.f);
    const float cents  = p.vib_depth * fade + mod_wheel_ * 40.f + press * 20.f;
    const float vib    = std::sin(kTwoPi * lfo_phase_) * cents;
    const float semis  = (static_cast<float>(v.note) - 69.f) + bend_ * 2.f + vib * 0.01f;
    const float freq   = 440.f * std::exp2(semis / 12.f);
    v.inc1 = Clamp(freq / detune_ / sr_, 0.f, 0.45f);
    v.inc2 = Clamp(freq * detune_ / sr_, 0.f, 0.45f);

    // Filter: envelope, key tracking, velocity, brightness, aftertouch.
    const float oct = v.fenv * p.env_octaves
                      + p.key_track * (static_cast<float>(v.note) - 60.f) / 12.f
                      + p.vel_bright * (v.vel - 0.6f) + (brightness_ - 0.5f) * 4.f
                      + press;
    const float fc  = Clamp(p.cutoff * std::exp2(oct), 20.f, 0.42f * sr_);
    const float g   = std::tan(3.14159265f * fc / sr_);
    const float res = Clamp(p.resonance + res_offset_, 0.f, 0.98f);
    v.k  = 2.f - 1.95f * res;
    v.a1 = 1.f / (1.f + g * (g + v.k));
    v.a2 = g * v.a1;
    v.a3 = g * v.a2;
}

void Synth::Process(float* out_l, float* out_r, size_t n)
{
    const Patch& p   = *p_;
    const float  cdt = kControlEvery / sr_;

    for(size_t i = 0; i < n; ++i)
    {
        if(control_count_ == 0)
        {
            lfo_phase_ += p.vib_rate * cdt;
            lfo_phase_ -= std::floor(lfo_phase_);
            pwm_phase_ += 0.35f * cdt;
            pwm_phase_ -= std::floor(pwm_phase_);
            ch_phase_slow_ += 0.5f * cdt;
            ch_phase_slow_ -= std::floor(ch_phase_slow_);
            ch_phase_fast_ += 5.8f * cdt;
            ch_phase_fast_ -= std::floor(ch_phase_fast_);
            pw_ = Clamp(p.pulse_width + p.pwm * std::sin(kTwoPi * pwm_phase_), 0.05f, 0.95f);
            ch_slow_   = std::sin(kTwoPi * ch_phase_slow_);
            ch_slow90_ = std::cos(kTwoPi * ch_phase_slow_);
            ch_fast_   = std::sin(kTwoPi * ch_phase_fast_);
            for(Voice& v : voices_)
                if(v.active)
                    UpdateControl(v, cdt);
        }
        const float pw = pw_;
        if(++control_count_ >= kControlEvery)
            control_count_ = 0;

        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;
        const float noise = static_cast<float>(noise_) * (2.f / 4294967296.f) - 1.f;

        float mix = 0.f;
        for(Voice& v : voices_)
        {
            if(!v.active)
                continue;

            // Line I: saw. Line II: pulse. Both band-limited (PolyBLEP).
            const float t1  = v.ph1;
            const float saw = 2.f * t1 - 1.f - PolyBlep(t1, v.inc1);
            const float t2  = v.ph2;
            float       t2b = t2 + 1.f - pw;
            if(t2b >= 1.f)
                t2b -= 1.f;
            const float pulse = (t2 < pw ? 1.f : -1.f) + PolyBlep(t2, v.inc2)
                                - PolyBlep(t2b, v.inc2);
            v.ph1 += v.inc1;
            if(v.ph1 >= 1.f)
                v.ph1 -= 1.f;
            v.ph2 += v.inc2;
            if(v.ph2 >= 1.f)
                v.ph2 -= 1.f;
            const float x = saw * p.saw + pulse * p.pulse + noise * p.noise;

            // High-pass, then a resonant 12 dB state-variable low-pass.
            const float hy = hp_a_ * (v.hp_y + x - v.hp_x);
            v.hp_x         = x;
            v.hp_y         = hy;
            const float in = SoftSat(hy * 0.5f);
            const float v3 = in - v.ic2;
            const float v1 = v.a1 * v.ic1 + v.a2 * v3;
            const float v2 = v.ic2 + v.a2 * v.ic1 + v.a3 * v3;
            v.ic1          = 2.f * v1 - v.ic1;
            v.ic2          = 2.f * v2 - v.ic2;

            // Amplifier envelope.
            switch(v.amp_stage)
            {
                case 0:
                    v.amp += (1.2f - v.amp) * amp_att_c_;
                    if(v.amp >= 1.f)
                    {
                        v.amp       = 1.f;
                        v.amp_stage = 1;
                    }
                    break;
                case 1:
                    v.amp += (p.sustain - v.amp) * amp_dec_c_;
                    if(p.sustain <= 0.f && v.amp < 1e-4f)
                        v.active = false; // a percussive note that has died away
                    break;
                default:
                    v.amp -= v.amp * amp_rel_c_;
                    if(v.amp < 1e-4f)
                        v.active = false;
                    break;
            }
            if(v.hold_left && --v.hold_left == 0 && v.key_down)
            {
                v.key_down = false;
                Release(v);
            }

            const float vel_gain = 1.f - p.vel_level + p.vel_level * v.vel;
            mix += v2 * v.amp * vel_gain;
        }
        mix *= kVoiceGain * p.level;

        // Stereo ensemble chorus: two modulated taps, a quarter-cycle apart.
        chorus_buf_[chorus_w_] = mix;
        const float slow   = ch_slow_;
        const float slow90 = ch_slow90_;
        const float fast   = ch_fast_;
        const float base   = 0.008f * sr_;
        const float d_l    = base + (0.003f * slow + 0.00015f * fast) * sr_;
        const float d_r    = base + (0.003f * slow90 - 0.00015f * fast) * sr_;
        auto tap = [&](float d) {
            float rp = static_cast<float>(chorus_w_) - d;
            if(rp < 0.f)
                rp += static_cast<float>(kChorusLen);
            const size_t i0 = static_cast<size_t>(rp);
            const float  fr = rp - static_cast<float>(i0);
            const size_t i1 = (i0 + 1) & (kChorusLen - 1);
            return chorus_buf_[i0 & (kChorusLen - 1)] * (1.f - fr) + chorus_buf_[i1] * fr;
        };
        const float wet = p.chorus;
        const float dry = mix * (1.f - 0.5f * wet);
        out_l[i]        = dry + tap(d_l) * 0.7f * wet;
        out_r[i]        = dry + tap(d_r) * 0.7f * wet;
        chorus_w_       = (chorus_w_ + 1) & (kChorusLen - 1);
    }
}

} // namespace synth
