// Host-side tests for synth::Synth. No hardware needed: make -C test
#include "../src/synth.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0;

#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if(!(cond))                                                       \
        {                                                                 \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while(0)

#define CHECK_NEAR(a, b, tol)                                            \
    do                                                                   \
    {                                                                    \
        const double va = (a), vb = (b);                                 \
        if(std::fabs(va - vb) > (tol))                                   \
        {                                                                \
            std::printf("  FAIL %s:%d: %s = %g, expected %g (+/- %g)\n", \
                        __FILE__, __LINE__, #a, va, vb, (double)(tol));  \
            ++failures;                                                  \
        }                                                                \
    } while(0)

constexpr float kSr = 48000.f;

struct Out
{
    std::vector<float> l, r;
};

Out Run(synth::Synth& s, size_t frames)
{
    Out o;
    o.l.resize(frames);
    o.r.resize(frames);
    for(size_t i = 0; i < frames; i += 48)
    {
        const size_t n = frames - i < 48 ? frames - i : 48;
        s.Process(&o.l[i], &o.r[i], n);
    }
    return o;
}

double Rms(const std::vector<float>& x, size_t from = 0)
{
    double acc = 0;
    for(size_t i = from; i < x.size(); ++i)
        acc += x[i] * x[i];
    return std::sqrt(acc / (x.size() - from));
}

double Peak(const std::vector<float>& x)
{
    double p = 0;
    for(float v : x)
        p = std::fmax(p, std::fabs(v));
    return p;
}

bool Finite(const std::vector<float>& x)
{
    for(float v : x)
        if(!std::isfinite(v))
            return false;
    return true;
}

// Rising zero crossings per second.
double Frequency(const std::vector<float>& x)
{
    int crossings = 0;
    for(size_t i = 1; i < x.size(); ++i)
        if(x[i - 1] < 0.f && x[i] >= 0.f)
            ++crossings;
    return crossings * kSr / static_cast<double>(x.size());
}

// RMS of the sample-to-sample difference: weights energy by frequency, so
// relative to the plain RMS it measures brightness.
double Slope(const std::vector<float>& x)
{
    double acc = 0;
    for(size_t i = 1; i < x.size(); ++i)
        acc += (x[i] - x[i - 1]) * (x[i] - x[i - 1]);
    return std::sqrt(acc / x.size());
}

void TestSilentAtRest()
{
    std::printf("silent with no notes\n");
    synth::Synth s;
    s.Init(kSr);
    Out o = Run(s, 4800);
    CHECK(Peak(o.l) == 0.0 && Peak(o.r) == 0.0);
}

void TestNoteOnAndOff()
{
    std::printf("a note sounds, and dies away after note-off\n");
    synth::Synth s;
    s.Init(kSr);
    s.NoteOn(60, 100);
    Out a = Run(s, 48000);
    CHECK(Rms(a.l, 24000) > 0.02);
    CHECK(Finite(a.l) && Finite(a.r));
    s.NoteOff(60);
    Run(s, 48000 * 4); // Blade releases over ~1.2 s
    CHECK(s.ActiveVoices() == 0);
    Out c = Run(s, 4800);
    CHECK(Peak(c.l) < 1e-4);
}

void TestPitch()
{
    std::printf("pitch is right, and pitch bend moves it 2 semitones\n");
    synth::Synth s;
    s.Init(kSr);
    s.SetPatch(2);         // Bass
    s.SetBrightness(0.f);  // filter right down: close to a sine
    s.NoteOn(57, 127);     // A3 = 220 Hz
    Run(s, 24000);
    Out o = Run(s, 48000);
    const double f = Frequency(o.l);
    std::printf("  A3 measured %.1f Hz\n", f);
    CHECK_NEAR(f, 220.0, 220.0 * 0.03);
    s.SetPitchBend(1.f);
    Run(s, 4800);
    Out b = Run(s, 48000);
    CHECK_NEAR(Frequency(b.l), 246.94, 246.94 * 0.03);
}

void TestPolyphonyAndRetrigger()
{
    std::printf("8 voices; a 9th note steals; the same note reuses its voice\n");
    synth::Synth s;
    s.Init(kSr);
    for(int i = 0; i < 8; ++i)
        s.NoteOn(static_cast<uint8_t>(60 + i), 100);
    CHECK(s.ActiveVoices() == 8);
    s.NoteOn(80, 100);
    CHECK(s.ActiveVoices() == 8);
    s.AllNotesOff();
    Run(s, 48000 * 4);
    CHECK(s.ActiveVoices() == 0);
    s.NoteOn(60, 100);
    s.NoteOn(60, 100);
    CHECK(s.ActiveVoices() == 1);
}

void TestSustainPedal()
{
    std::printf("sustain pedal holds released notes\n");
    synth::Synth s;
    s.Init(kSr);
    s.SetSustain(true);
    s.NoteOn(64, 100);
    Run(s, 4800);
    s.NoteOff(64);
    Run(s, 48000 * 3);
    CHECK(s.ActiveVoices() == 1);
    s.SetSustain(false);
    Run(s, 48000 * 4);
    CHECK(s.ActiveVoices() == 0);
}

void TestHoldLimit()
{
    std::printf("a note with no note-off releases itself after its hold limit\n");
    synth::Synth s;
    s.Init(kSr);
    s.SetPatch(2); // Bass: short release
    s.NoteOn(48, 100, 0.5f);
    Run(s, 24000 - 480);
    CHECK(s.ActiveVoices() == 1);
    Run(s, 48000);
    CHECK(s.ActiveVoices() == 0);
}

void TestPercussiveNotesEndOnTheirOwn()
{
    std::printf("Bells notes die away even with the key held\n");
    synth::Synth s;
    s.Init(kSr);
    s.SetPatch(3);
    s.NoteOn(72, 100);
    Run(s, 48000 * 8);
    CHECK(s.ActiveVoices() == 0);
}

void TestVelocityBrightens()
{
    std::printf("harder notes are brighter\n");
    double slope[2];
    const uint8_t vel[2] = {30, 127};
    for(int i = 0; i < 2; ++i)
    {
        synth::Synth s;
        s.Init(kSr);
        s.NoteOn(57, vel[i]);
        Run(s, 24000);
        Out o = Run(s, 24000);
        slope[i] = Slope(o.l) / Rms(o.l);
    }
    std::printf("  brightness ratio soft:hard = 1:%.2f\n", slope[1] / slope[0]);
    CHECK(slope[1] > slope[0] * 1.3);
}

void TestEveryPatchStaysSane()
{
    std::printf("every patch: full chord, everything turned up, stays bounded\n");
    for(int p = 0; p < synth::kPatches; ++p)
    {
        synth::Synth s;
        s.Init(kSr);
        s.SetPatch(p);
        s.SetBrightness(1.f);
        s.SetResonanceOffset(0.5f);
        s.SetModWheel(1.f);
        s.SetPressure(1.f);
        for(int i = 0; i < 8; ++i)
            s.NoteOn(static_cast<uint8_t>(36 + i * 7), 127);
        Out o = Run(s, 48000 * 2);
        s.SetPitchBend(-1.f);
        Out o2 = Run(s, 24000);
        const double peak = std::fmax(Peak(o.l), Peak(o.r));
        std::printf("  %-8s peak %.2f\n", synth::GetPatch(p).name, peak);
        CHECK(Finite(o.l) && Finite(o.r) && Finite(o2.l));
        CHECK(peak < 2.0);
        CHECK(peak > 0.1);
    }
}

void TestTypicalLevel()
{
    std::printf("a 4-note chord sits below clipping\n");
    for(int p = 0; p < synth::kPatches; ++p)
    {
        synth::Synth s;
        s.Init(kSr);
        s.SetPatch(p);
        for(uint8_t n : {48, 55, 60, 63})
            s.NoteOn(n, 100);
        Out o = Run(s, 48000 * 2);
        CHECK(Peak(o.l) < 0.9);
    }
}

void TestPatchChangeWhilePlaying()
{
    std::printf("changing patch mid-note is safe\n");
    synth::Synth s;
    s.Init(kSr);
    s.NoteOn(60, 100);
    s.NoteOn(67, 100);
    Run(s, 4800);
    for(int p = 0; p < synth::kPatches; ++p)
    {
        s.SetPatch(p);
        Out o = Run(s, 4800);
        CHECK(Finite(o.l) && Peak(o.l) < 2.0);
    }
}
} // namespace

int main()
{
    TestSilentAtRest();
    TestNoteOnAndOff();
    TestPitch();
    TestPolyphonyAndRetrigger();
    TestSustainPedal();
    TestHoldLimit();
    TestPercussiveNotesEndOnTheirOwn();
    TestVelocityBrightens();
    TestEveryPatchStaysSane();
    TestTypicalLevel();
    TestPatchChangeWhilePlaying();
    if(failures)
    {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall synth tests passed\n");
    return 0;
}
