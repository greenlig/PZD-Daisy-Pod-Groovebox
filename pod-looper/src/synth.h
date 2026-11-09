// Pod Looper - a small CS-80-inspired polysynth.
//
// Not an emulation, just the recipe that makes a CS-80 sound like one:
//  - two oscillator "lines" per voice (saw and pulse with slow PWM),
//    detuned against each other
//  - a high-pass into a resonant 12 dB low-pass, as on the CS-80
//  - a CS-80-style filter envelope: it starts at an "initial level" and
//    swells to an "attack level" before settling, which gives the brass
//    its slow "waah"
//  - velocity opens the filter (the CS-80's touch response), and channel or
//    poly aftertouch adds brightness and vibrato when a controller sends it
//  - delayed vibrato and a stereo ensemble chorus
//
// Plain C++ with no libDaisy dependency, so it can be tested on a PC.
// All methods are for the audio thread.
#pragma once

#include <cstddef>
#include <cstdint>

namespace synth
{
constexpr int kVoices  = 8;
constexpr int kPatches = 4;

struct Patch
{
    const char* name;
    // oscillators: line I saw, line II pulse
    float saw, pulse;
    float pulse_width; // 0.5 = square
    float pwm;         // slow pulse-width modulation depth
    float detune;      // cents between the two lines
    float noise;
    // filters
    float hpf;         // Hz
    float cutoff;      // Hz with the filter envelope closed
    float resonance;   // 0..1
    float env_octaves; // how far the filter envelope opens it
    float key_track;   // 0..1
    float vel_bright;  // octaves of cutoff across the velocity range
    // filter envelope: initial level -> attack level -> sustain
    float f_initial, f_peak, f_attack, f_decay, f_sustain, f_release;
    // amplifier
    float attack, decay, sustain, release;
    float vel_level; // 0 = every note equally loud, 1 = fully velocity sensitive
    // modulation
    float vib_rate, vib_depth, vib_delay; // Hz, cents, seconds
    float chorus;                         // 0..1
    float level;
};

// The built-in patches: Blade (brass), Strings, Bass, Bells.
const Patch& GetPatch(int index);

class Synth
{
  public:
    void Init(float sample_rate);

    // max_hold_s > 0 releases the note by itself after that long, for
    // controllers that might not send note-offs.
    void NoteOn(uint8_t note, uint8_t velocity, float max_hold_s = 0.f);
    void NoteOff(uint8_t note);
    void AllNotesOff();

    void SetPatch(int index);
    int  PatchIndex() const { return patch_index_; }
    // Brightness macro, 0..1: 0.5 = the patch as designed, +/- 2 octaves.
    void SetBrightness(float b);
    void SetModWheel(float v);                // adds vibrato
    void SetPitchBend(float v);               // -1..1 = -/+ 2 semitones
    void SetPressure(float v);                // channel aftertouch
    void SetPolyPressure(uint8_t note, float v);
    void SetSustain(bool down);
    void SetResonanceOffset(float v);         // CC 71, -0.5..0.5

    // Mono voices into a stereo chorus. Writes (does not add) n samples.
    void Process(float* out_l, float* out_r, size_t n);

    int ActiveVoices() const;

  private:
    struct Voice
    {
        bool     active   = false;
        bool     key_down = false;
        bool     sustained = false; // released while the pedal was down
        uint8_t  note     = 0;
        float    vel      = 0.f;
        float    pressure = 0.f;
        uint32_t age      = 0;
        uint32_t hold_left = 0; // samples until auto-release (0 = none)

        float ph1 = 0.f, ph2 = 0.f; // oscillator phases
        float inc1 = 0.f, inc2 = 0.f;
        float hp_x = 0.f, hp_y = 0.f;   // high-pass state
        float ic1 = 0.f, ic2 = 0.f;     // low-pass state
        float a1 = 0.f, a2 = 0.f, a3 = 0.f, k = 2.f;

        int   amp_stage = 0; // 0 attack, 1 decay/sustain, 2 release
        float amp       = 0.f;
        int   f_stage   = 0;
        float fenv      = 0.f;
        float since_on  = 0.f; // seconds, for delayed vibrato
    };

    void UpdateControl(Voice& v, float control_dt);
    void Release(Voice& v);

    float sr_         = 48000.f;
    int   patch_index_ = 0;
    const Patch* p_   = nullptr;
    Voice voices_[kVoices];
    uint32_t age_counter_ = 0;

    float brightness_ = 0.5f;
    float mod_wheel_  = 0.f;
    float bend_       = 0.f;
    float pressure_   = 0.f;
    float res_offset_ = 0.f;
    bool  sustain_    = false;

    float lfo_phase_ = 0.f; // vibrato
    float pwm_phase_ = 0.f;
    uint32_t noise_  = 22222u;
    int   control_count_ = 0;

    // per-patch coefficients
    float amp_att_c_ = 0.f, amp_dec_c_ = 0.f, amp_rel_c_ = 0.f;
    float f_att_c_ = 0.f, f_dec_c_ = 0.f, f_rel_c_ = 0.f;
    float hp_a_    = 0.f;
    float detune_  = 1.f; // frequency ratio between the two lines (square root of)

    // chorus
    static constexpr size_t kChorusLen = 2048;
    float  chorus_buf_[kChorusLen] = {};
    size_t chorus_w_ = 0;
    float  ch_phase_slow_ = 0.f, ch_phase_fast_ = 0.f;
    // LFO values, refreshed at control rate
    float pw_ = 0.5f, ch_slow_ = 0.f, ch_slow90_ = 1.f, ch_fast_ = 0.f;
};

} // namespace synth
