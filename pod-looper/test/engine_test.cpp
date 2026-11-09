// Host-side tests for looper::Engine. No hardware needed:
//   make -C test        (builds with AddressSanitizer + UBSan and runs)
#include "../src/looper.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

using looper::Engine;
using looper::LayerState;
using looper::SyncEvent;
using looper::SyncType;

namespace
{
int failures = 0;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if(!(cond))                                                     \
        {                                                               \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while(0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do                                                                         \
    {                                                                          \
        const double va = (a), vb = (b);                                       \
        if(std::fabs(va - vb) > (tol))                                         \
        {                                                                      \
            std::printf("  FAIL %s:%d: %s = %g, expected %g (+/- %g)\n",       \
                        __FILE__, __LINE__, #a, va, vb, (double)(tol));        \
            ++failures;                                                        \
        }                                                                      \
    } while(0)

constexpr float  kSr     = 48000.f;
constexpr size_t kBlock  = 48;
constexpr size_t kFrames = 48000 * 12; // 12 s per layer is plenty for tests
constexpr int    kLayers = 4;

// A test rig: an engine plus its buffers, an absolute frame clock, and a
// queue of timed MIDI sync events.
struct Rig
{
    std::vector<float> l[kLayers], r[kLayers];
    Engine             e;
    size_t             now = 0; // absolute frame
    std::vector<std::pair<size_t, SyncType>> pending;
    std::vector<float> out_l, out_r; // everything output so far
    std::function<float(size_t)> input = [](size_t) { return 0.f; };

    explicit Rig(bool quantize = true, bool follow = true, size_t frames = kFrames)
    {
        float* lp[kLayers];
        float* rp[kLayers];
        for(int k = 0; k < kLayers; ++k)
        {
            l[k].assign(frames, 0.f);
            r[k].assign(frames, 0.f);
            lp[k] = l[k].data();
            rp[k] = r[k].data();
        }
        looper::EngineConfig c;
        c.sample_rate      = kSr;
        c.max_frames       = frames;
        c.beats_per_bar    = 4;
        c.quantize_to_bars = quantize;
        c.follow_transport = follow;
        e.Init(c, lp, rp, kLayers);
        e.SetMonitor(0.f);
    }

    void At(size_t frame, SyncType t) { pending.push_back({frame, t}); }

    // Clock ticks every `period` frames (may be fractional) from `start`.
    void Clock(size_t start, double period, int ticks)
    {
        for(int i = 0; i < ticks; ++i)
            At(start + static_cast<size_t>(std::floor(i * period)), SyncType::Clock);
    }

    // Do what the firmware's main loop does: zero cleared layers.
    void Zero()
    {
        for(int k = 0; k < kLayers; ++k)
        {
            if(e.NeedsZero(k))
            {
                const size_t n = e.ZeroFrames(k);
                std::fill(l[k].begin(), l[k].begin() + n, 0.f);
                std::fill(r[k].begin(), r[k].begin() + n, 0.f);
                e.MarkZeroed(k);
            }
        }
    }

    void Run(size_t frames)
    {
        float in_l[kBlock], in_r[kBlock], ol[kBlock], orr[kBlock];
        while(frames > 0)
        {
            const size_t n = frames < kBlock ? frames : kBlock;
            for(size_t i = 0; i < n; ++i)
                in_l[i] = in_r[i] = input(now + i);
            SyncEvent ev[64];
            size_t    nev = 0;
            std::vector<std::pair<size_t, SyncType>> rest;
            std::stable_sort(pending.begin(), pending.end(),
                             [](auto& a, auto& b) { return a.first < b.first; });
            for(auto& p : pending)
            {
                if(p.first < now + n && nev < 64)
                    ev[nev++] = {p.second, static_cast<uint32_t>(p.first < now ? 0 : p.first - now)};
                else
                    rest.push_back(p);
            }
            pending.swap(rest);
            e.Process(in_l, in_r, ol, orr, n, ev, nev);
            out_l.insert(out_l.end(), ol, ol + n);
            out_r.insert(out_r.end(), orr, orr + n);
            now += n;
            frames -= n;
            Zero();
        }
    }

    // Run until the absolute frame `t` (which should be block aligned for
    // actions to land exactly there).
    void RunTo(size_t t) { Run(t > now ? t - now : 0); }

};

float Ramp(size_t i)
{
    return 0.25f * std::sin(0.001f * static_cast<float>(i)) + 0.1f;
}

// ------------------------------------------------------------------ tests

void TestFreeRecordAndPlay()
{
    std::printf("free record and play back\n");
    Rig rig;
    rig.input = [](size_t i) { return Ramp(i); };
    rig.e.RecPlayDub(0);
    CHECK(rig.e.State(0) == LayerState::Recording); // no clock: starts at once
    rig.Run(48000);
    rig.e.RecPlayDub(0);
    CHECK(rig.e.State(0) == LayerState::Playing);
    CHECK(rig.e.LoopLength() == 48000);
    CHECK(!rig.e.Synced());

    // Silence in, the loop should come back out (after its fade-in).
    rig.input = [](size_t) { return 0.f; };
    rig.Run(48000);
    const size_t start = 48000;
    for(size_t i : {200u, 2000u, 10000u, 30000u, 47000u})
        CHECK_NEAR(rig.out_l[start + i], Ramp(i), 1e-4);
    // Second pass identical.
    rig.Run(48000);
    CHECK_NEAR(rig.out_l[2 * 48000 + 25000], Ramp(25000), 1e-4);
    // The seam was faded: first sample of the loop is silent.
    CHECK_NEAR(rig.l[0][0], 0.0, 1e-9);
}

void TestOverdub()
{
    std::printf("overdub adds to the loop\n");
    Rig rig;
    rig.input = [](size_t i) { return Ramp(i); };
    rig.e.RecPlayDub(0);
    rig.Run(48000);
    rig.e.RecPlayDub(0); // -> play
    rig.input = [](size_t) { return 0.f; };
    rig.Run(48000);       // one clean pass
    rig.e.RecPlayDub(0);  // -> overdub, at the loop start
    CHECK(rig.e.State(0) == LayerState::Overdubbing);
    rig.input = [](size_t) { return 0.1f; };
    rig.Run(48000);
    rig.e.RecPlayDub(0); // -> play
    CHECK(rig.e.State(0) == LayerState::Playing);
    rig.input = [](size_t) { return 0.f; };
    rig.Run(48000);
    const size_t start = 3 * 48000;
    CHECK_NEAR(rig.out_l[start + 20000], Ramp(20000) + 0.1f, 1e-4);
}

void TestSecondLayerIsAligned()
{
    std::printf("a second layer records in time with the first\n");
    Rig rig;
    rig.input = [](size_t i) { return i < 48000 ? 0.2f : 0.f; };
    rig.e.RecPlayDub(0);
    rig.Run(48000);
    rig.e.RecPlayDub(0);
    rig.Run(12000); // a quarter of the way into the loop
    rig.e.RecPlayDub(1);
    CHECK(rig.e.State(1) == LayerState::Overdubbing); // starts straight away
    rig.input = [](size_t) { return 0.05f; };
    rig.Run(48000);
    rig.e.RecPlayDub(1);
    CHECK(rig.e.State(1) == LayerState::Playing);
    // Layer 2 covers the whole loop with 0.05 (away from the punch points).
    CHECK_NEAR(rig.l[1][24000], 0.05, 1e-4);
    CHECK_NEAR(rig.l[1][5000], 0.05, 1e-4);
}

void TestPressRecOnAnotherLayerWhileRecording()
{
    std::printf("Rec on layer 2 while layer 1's first take runs\n");
    Rig rig;
    rig.input = [](size_t) { return 0.3f; };
    rig.e.RecPlayDub(0);
    rig.Run(24000);
    rig.e.RecPlayDub(1);
    CHECK(rig.e.LoopLength() == 24000);
    CHECK(rig.e.State(0) == LayerState::Playing);
    CHECK(rig.e.State(1) == LayerState::Overdubbing);
}

void TestMuteAndPause()
{
    std::printf("mute, unmute, pause\n");
    Rig rig;
    rig.input = [](size_t) { return 0.3f; };
    rig.e.RecPlayDub(0);
    rig.Run(24000);
    rig.e.RecPlayDub(0);
    rig.input = [](size_t) { return 0.f; };
    rig.Run(4800);
    rig.e.StopToggle(0);
    CHECK(rig.e.State(0) == LayerState::Muted);
    rig.Run(4800);
    CHECK_NEAR(rig.out_l.back(), 0.0, 1e-4);
    rig.e.StopToggle(0);
    CHECK(rig.e.State(0) == LayerState::Playing);
    rig.Run(4800);
    CHECK_NEAR(rig.out_l.back(), 0.3, 1e-3);

    rig.e.TogglePause();
    CHECK(rig.e.Paused());
    rig.Run(4800);
    const size_t ph = rig.e.Playhead();
    rig.Run(4800);
    CHECK(rig.e.Playhead() == ph); // frozen
    CHECK_NEAR(rig.out_l.back(), 0.0, 1e-4);
    rig.e.TogglePause();
    rig.Run(4800);
    CHECK(rig.e.Playhead() != ph);
}

void TestClearAndReuse()
{
    std::printf("clear, zeroing handshake, reset when all empty\n");
    Rig rig;
    rig.input = [](size_t) { return 0.3f; };
    rig.e.RecPlayDub(0);
    rig.Run(24000);
    rig.e.RecPlayDub(0);
    rig.e.RecPlayDub(1);
    rig.Run(24000);
    rig.e.RecPlayDub(1);
    CHECK(rig.l[1][100] != 0.f);

    // Clearing layer 2 keeps the loop; its buffer is zeroed after fade-out.
    rig.e.Clear(1);
    CHECK(rig.e.State(1) == LayerState::Empty);
    CHECK(rig.e.HasLoop());
    // Re-arm immediately: it must wait for the zeroing, then dub.
    rig.e.RecPlayDub(1);
    CHECK(rig.e.State(1) == LayerState::Armed);
    rig.Run(4800);
    CHECK(rig.e.State(1) == LayerState::Overdubbing);
    rig.e.StopToggle(1);
    rig.Run(4800);

    // Clearing everything forgets the loop length.
    rig.e.ClearAll();
    rig.Run(4800);
    CHECK(!rig.e.HasLoop());
    CHECK(rig.e.LoopLength() == 0);
    CHECK_NEAR(rig.l[0][100], 0.0, 1e-9);
    CHECK_NEAR(rig.l[1][100], 0.0, 1e-9);

    // And a new, different length can be recorded.
    rig.e.RecPlayDub(2);
    CHECK(rig.e.State(2) == LayerState::Recording);
    rig.Run(9600);
    rig.e.RecPlayDub(2);
    CHECK(rig.e.LoopLength() == 9600);
}

void TestTooShortTakeIsDropped()
{
    std::printf("a double-tap doesn't make a loop\n");
    Rig rig;
    rig.e.RecPlayDub(0);
    rig.Run(480);
    rig.e.RecPlayDub(0);
    CHECK(!rig.e.HasLoop());
    CHECK(rig.e.State(0) == LayerState::Empty);
}

void TestMaxLength()
{
    std::printf("first take stops itself when memory is full\n");
    Rig rig(true, true, 48000);
    rig.input = [](size_t) { return 0.2f; };
    rig.e.RecPlayDub(0);
    rig.Run(60000);
    CHECK(rig.e.LoopLength() == 48000);
    CHECK(rig.e.State(0) == LayerState::Playing);
}

// 120 BPM at 48 kHz: 24 ticks per beat, a beat every 24000 frames.
constexpr double kTick = 1000.0;
constexpr size_t kBar  = 96000;

void TestSyncedFirstTake()
{
    std::printf("synced take starts and ends on bar lines\n");
    Rig rig;
    rig.input = [](size_t i) { return Ramp(i); };
    rig.At(0, SyncType::Start);
    rig.Clock(0, kTick, 96 * 6);
    rig.Run(4800);
    CHECK(rig.e.ClockPresent());
    CHECK(rig.e.TransportRunning());

    // Press mid-bar 1: armed until bar 2.
    rig.RunTo(48000);
    rig.e.RecPlayDub(0);
    CHECK(rig.e.State(0) == LayerState::Armed);
    rig.RunTo(kBar - 48);
    CHECK(rig.e.State(0) == LayerState::Armed);
    rig.RunTo(kBar + 48);
    CHECK(rig.e.State(0) == LayerState::Recording);

    // Press mid-bar 3: keeps recording until the bar 4 line.
    rig.RunTo(kBar * 2 + 30000);
    rig.e.RecPlayDub(0);
    CHECK(rig.e.State(0) == LayerState::Recording);
    CHECK(rig.e.StopPending());
    rig.RunTo(kBar * 3 + 48);
    CHECK(rig.e.State(0) == LayerState::Playing);
    CHECK(rig.e.Synced());
    CHECK(rig.e.LoopLength() == 2 * kBar); // exactly two bars

    // The loop holds what came in from the bar line on.
    CHECK_NEAR(rig.l[0][5000], Ramp(kBar + 5000), 1e-5);
    CHECK_NEAR(rig.l[0][100000], Ramp(kBar + 100000), 1e-5);
}

void TestDriftCorrection()
{
    std::printf("loop stays locked to a clock that runs slightly slow\n");
    Rig rig;
    rig.At(0, SyncType::Start);
    // Record one bar at exactly 120 BPM...
    rig.Clock(0, kTick, 96 * 5);
    rig.RunTo(4800);
    rig.e.RecPlayDub(0);           // armed: starts on bar 2 (frame 96000)
    rig.RunTo(kBar + kBar / 2);
    rig.e.RecPlayDub(0);           // ends on bar 3: a one-bar loop
    rig.RunTo(kBar * 2 + kBar / 2);
    CHECK(rig.e.Synced());
    CHECK(rig.e.LoopLength() == kBar);
    // A steady clock needs no correcting.
    rig.RunTo(kBar * 4 + 100);
    CHECK(std::abs(rig.e.LastPhaseError()) <= 1);
    CHECK(rig.e.PhaseCorrections() == 0);

    // ...then the external clock runs 0.05 % slow (far worse than real
    // crystals) for 40 bars: 48 frames of drift per bar.
    const size_t t0 = 5 * kBar;
    rig.Clock(t0, kTick * 1.0005, 96 * 40 + 1);
    int worst = 0;
    for(int bar = 1; bar <= 40; ++bar)
    {
        const size_t bar_time = t0 + static_cast<size_t>(std::floor(bar * 96 * kTick * 1.0005));
        rig.RunTo(bar_time + 1);
        const int err = std::abs(rig.e.LastPhaseError());
        if(err > worst)
            worst = err;
    }
    // Uncorrected it would be 40 * 48 = 1920 frames (40 ms) out by now.
    std::printf("  worst error at a bar line: %d frames, %u corrections\n",
                worst, (unsigned)rig.e.PhaseCorrections());
    CHECK(worst <= 72 + 48 + 2); // threshold (1.5 ms) + one bar of drift
    CHECK(rig.e.PhaseCorrections() >= 10);
}

void TestStopAndStartRealign()
{
    std::printf("MIDI Stop pauses, Start restarts in phase\n");
    Rig rig;
    rig.At(0, SyncType::Start);
    // This clock keeps going a little after Stop (as some devices do) and
    // ends well before the second Start.
    rig.Clock(0, kTick, 96 * 2 + 30);
    rig.RunTo(4800);
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar + kBar / 2);
    rig.e.RecPlayDub(0); // one-bar take, bar 2 to bar 3
    rig.RunTo(2 * kBar + kBar / 4);
    CHECK(rig.e.Synced());
    CHECK(rig.e.LoopLength() == kBar);

    rig.At(rig.now + 10, SyncType::Stop);
    rig.Run(9600);
    CHECK(rig.e.Paused());
    CHECK(!rig.e.TransportRunning());
    const size_t ph = rig.e.Playhead();
    rig.Run(9600);
    CHECK(rig.e.Playhead() == ph);

    // Start: playhead jumps to the top of the bar on the first clock.
    const size_t s = rig.now + 1000;
    rig.At(s, SyncType::Start);
    rig.Clock(s + 500, kTick, 96 * 3);
    rig.RunTo(s + 500 + 48);
    CHECK(!rig.e.Paused());
    CHECK(rig.e.Playhead() < 100);
    const uint32_t corrections = rig.e.PhaseCorrections();
    rig.RunTo(s + 500 + 2 * kBar + 96);
    CHECK(std::abs(rig.e.LastPhaseError()) <= 2);
    CHECK(rig.e.PhaseCorrections() == corrections);
}

void TestClockLoss()
{
    std::printf("clock dropping out falls back to free running\n");
    Rig rig;
    rig.At(0, SyncType::Start);
    rig.Clock(0, kTick, 50);
    rig.Run(10000);
    CHECK(rig.e.ClockPresent());
    rig.RunTo(49000 + 30000); // last tick at 49000, timeout is 24000 frames
    CHECK(!rig.e.ClockPresent());
    CHECK(!rig.e.TransportRunning());
    rig.e.RecPlayDub(0);
    CHECK(rig.e.State(0) == LayerState::Recording); // no bar to wait for
}

void TestArmedCancelledByStop()
{
    std::printf("an armed first take is cancelled by MIDI Stop\n");
    Rig rig;
    rig.At(0, SyncType::Start);
    rig.Clock(0, kTick, 60);
    rig.Run(9600);
    rig.e.RecPlayDub(0);
    CHECK(rig.e.State(0) == LayerState::Armed);
    rig.At(rig.now + 5, SyncType::Stop);
    rig.Run(96);
    CHECK(rig.e.State(0) == LayerState::Empty);
}

void TestNoFollowTransport()
{
    std::printf("with follow-transport off, Stop doesn't pause\n");
    Rig rig(true, false);
    rig.input = [](size_t) { return 0.3f; };
    rig.e.RecPlayDub(0);
    rig.Run(24000);
    rig.e.RecPlayDub(0);
    rig.At(rig.now + 5, SyncType::Stop);
    rig.Run(4800);
    CHECK(!rig.e.Paused());
}

void TestMonitorAndClip()
{
    std::printf("monitor passes input through; output never exceeds 1\n");
    Rig rig;
    rig.e.SetMonitor(1.f);
    rig.input = [](size_t) { return 0.5f; };
    rig.Run(4800);
    CHECK_NEAR(rig.out_l.back(), 0.5, 1e-3);
    rig.input = [](size_t) { return 3.f; };
    rig.Run(4800);
    CHECK(rig.out_l.back() <= 1.f);
    CHECK(rig.out_l.back() > 0.95f);
}
} // namespace

int main()
{
    TestFreeRecordAndPlay();
    TestOverdub();
    TestSecondLayerIsAligned();
    TestPressRecOnAnotherLayerWhileRecording();
    TestMuteAndPause();
    TestClearAndReuse();
    TestTooShortTakeIsDropped();
    TestMaxLength();
    TestSyncedFirstTake();
    TestDriftCorrection();
    TestStopAndStartRealign();
    TestClockLoss();
    TestArmedCancelledByStop();
    TestNoFollowTransport();
    TestMonitorAndClip();
    if(failures)
    {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall tests passed\n");
    return 0;
}
