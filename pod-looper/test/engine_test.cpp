// Host-side tests for looper::Engine. No hardware needed:
//   make -C test        (builds with AddressSanitizer + UBSan and runs)
#include "../src/looper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

using looper::Engine;
using looper::Quantize;
using looper::RecordMode;
using looper::StartMode;
using looper::SyncEvent;
using looper::SyncType;
using looper::TrackState;

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

constexpr float  kSr     = 48000.f;
constexpr size_t kBlock  = 48;
constexpr size_t kFrames = 48000 * 12; // 12 s per track is plenty for tests
constexpr int    kTracks = 4;
constexpr double kQ      = 1.0 / 16384.0; // 16-bit storage step

// At 120 BPM and 48 kHz: a MIDI clock every 1000 frames, a beat every
// 24000, a bar every 96000.
constexpr double kTick = 1000.0;
constexpr size_t kBeat = 24000;
constexpr size_t kBar  = 96000;

constexpr RecordMode kBarNow{Quantize::Bar, StartMode::Immediate};
constexpr RecordMode kBarArmed{Quantize::Bar, StartMode::Armed};
constexpr RecordMode kBeatNow{Quantize::Beat, StartMode::Immediate};
constexpr RecordMode kFree{Quantize::Free, StartMode::Immediate};

// An engine plus its buffers, an absolute frame clock and a queue of timed
// MIDI sync events.
struct Rig
{
    std::vector<int16_t> l[kTracks], r[kTracks], ul, ur;
    Engine               e;
    size_t               now = 0;
    std::vector<std::pair<size_t, SyncType>> pending;
    std::vector<float>   out_l;
    std::function<float(size_t)> input = [](size_t) { return 0.f; };

    explicit Rig(RecordMode mode = kBarNow, size_t frames = kFrames, bool follow = true)
    {
        int16_t* lp[kTracks];
        int16_t* rp[kTracks];
        for(int t = 0; t < kTracks; ++t)
        {
            l[t].assign(frames, 0);
            r[t].assign(frames, 0);
            lp[t] = l[t].data();
            rp[t] = r[t].data();
        }
        ul.assign(frames, 0);
        ur.assign(frames, 0);
        looper::EngineConfig c;
        c.sample_rate  = kSr;
        c.max_frames   = frames;
        c.tempo        = 120.f;
        c.follow_clock = follow;
        c.record_mode  = mode;
        e.Init(c, lp, rp, ul.data(), ur.data(), kTracks);
        e.SetMonitor(0.f);
    }

    void At(size_t frame, SyncType t) { pending.push_back({frame, t}); }

    // MIDI clock every `period` frames (may be fractional) from `start`.
    void Clock(size_t start, double period, int ticks)
    {
        for(int i = 0; i < ticks; ++i)
            At(start + static_cast<size_t>(std::floor(i * period)), SyncType::Clock);
    }

    void Run(size_t frames)
    {
        float in_l[kBlock], in_r[kBlock], ol[kBlock], orr[kBlock];
        while(frames > 0)
        {
            const size_t n = frames < kBlock ? frames : kBlock;
            for(size_t i = 0; i < n; ++i)
                in_l[i] = in_r[i] = input(now + i);
            std::stable_sort(pending.begin(), pending.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            SyncEvent ev[64];
            size_t    nev = 0;
            std::vector<std::pair<size_t, SyncType>> rest;
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
            now += n;
            frames -= n;
        }
    }

    // Actions land exactly on `t` when it is a multiple of the block size.
    void RunTo(size_t t) { Run(t > now ? t - now : 0); }
};

float Ramp(size_t i)
{
    return 0.25f * std::sin(0.001f * static_cast<float>(i)) + 0.1f;
}

float Sample(const Rig& rig, int t, size_t i)
{
    return rig.l[t][i] * static_cast<float>(kQ);
}

// ------------------------------------------------------------ grid takes

void TestRecFromStoppedSnapsToBar()
{
    std::printf("Rec from stopped starts the transport; end snaps to the bar\n");
    Rig rig;
    rig.input = [](size_t i) { return Ramp(i); };
    CHECK(!rig.e.Playing());
    rig.e.RecPlayDub(0);
    CHECK(rig.e.Playing());
    rig.Run(kBlock);
    CHECK(rig.e.State(0) == TrackState::Recording);

    rig.RunTo(kBar + kBar / 2); // press half way through bar 2
    rig.e.RecPlayDub(0);
    CHECK(rig.e.State(0) == TrackState::Recording);
    CHECK(rig.e.EndPending(0));
    rig.RunTo(2 * kBar + kBlock);
    CHECK(rig.e.State(0) == TrackState::Playing);
    CHECK(rig.e.Length(0) == 2 * kBar);
    CHECK(rig.e.OnGrid(0));
    CHECK_NEAR(rig.e.LengthBeats(0), 8.0, 1e-9);
    CHECK_NEAR(Sample(rig, 0, 5000), Ramp(5000), 2 * kQ);
    CHECK_NEAR(Sample(rig, 0, 150000), Ramp(150000), 2 * kQ);

    // It plays back what was recorded.
    rig.input = [](size_t) { return 0.f; };
    rig.RunTo(3 * kBar);
    CHECK_NEAR(rig.out_l[2 * kBar + 30000], Ramp(30000), 3 * kQ);
}

void TestLatePressTrimsToTheLineJustPassed()
{
    std::printf("a slightly late press ends on the bar line just passed\n");
    Rig rig;
    rig.e.RecPlayDub(0);
    rig.RunTo(2 * kBar + 2400); // 50 ms after bar 3 starts
    rig.e.RecPlayDub(0);
    CHECK(rig.e.State(0) == TrackState::Playing);
    CHECK(rig.e.Length(0) == 2 * kBar);
    CHECK(rig.e.Playhead(0) == 2400); // already 50 ms into the loop
    rig.RunTo(5 * kBar + kBlock);
    CHECK(rig.e.Playhead(0) == (5 * kBar + kBlock) % (2 * kBar));
    CHECK(std::abs(rig.e.LastPhaseError(0)) <= 1);
}

void TestImmediateStartMidBarKeepsItsPlace()
{
    std::printf("an immediate start mid-bar is whole bars and stays on the grid\n");
    Rig rig;
    rig.e.TransportToggle();
    rig.RunTo(kBar + kBar / 4); // a quarter into bar 2
    rig.input = [](size_t i) { return Ramp(i); };
    rig.e.RecPlayDub(1);
    CHECK(rig.e.State(1) == TrackState::Recording);
    rig.RunTo(2 * kBar); // only 3/4 of a bar played: keeps going to 1 bar
    rig.e.RecPlayDub(1);
    CHECK(rig.e.EndPending(1));
    rig.RunTo(2 * kBar + kBar / 4 + kBlock);
    CHECK(rig.e.State(1) == TrackState::Playing);
    CHECK(rig.e.Length(1) == kBar);
    // Frame 0 of the loop is what came in at bar 2 + 1/4.
    CHECK_NEAR(Sample(rig, 1, 1000), Ramp(kBar + kBar / 4 + 1000), 2 * kQ);
    // Much later its playhead still lines up with where it was recorded.
    rig.RunTo(7 * kBar + 12000);
    CHECK(rig.e.Playhead(1) == (7 * kBar + 12000 - (kBar + kBar / 4)) % kBar);
}

void TestArmedStartWaitsForTheBar()
{
    std::printf("armed start waits for the next bar line\n");
    Rig rig(kBarArmed);
    rig.e.TransportToggle();
    rig.RunTo(kBar + kBar / 2);
    rig.e.RecPlayDub(0);
    CHECK(rig.e.State(0) == TrackState::Armed);
    rig.RunTo(2 * kBar - kBlock);
    CHECK(rig.e.State(0) == TrackState::Armed);
    rig.RunTo(2 * kBar + kBlock);
    CHECK(rig.e.State(0) == TrackState::Recording);
    rig.RunTo(2 * kBar + kBar / 2);
    rig.e.RecPlayDub(0);
    rig.RunTo(3 * kBar + kBlock);
    CHECK(rig.e.Length(0) == kBar);
    rig.RunTo(6 * kBar + 4800);
    CHECK(rig.e.Playhead(0) == 4800);
}

void TestBeatQuantize()
{
    std::printf("beat mode: lengths are whole beats\n");
    Rig rig(kBeatNow);
    rig.e.RecPlayDub(0);
    rig.RunTo(2 * kBeat + 14400); // 2.6 beats
    rig.e.RecPlayDub(0);
    rig.RunTo(3 * kBeat + kBlock);
    CHECK(rig.e.Length(0) == 3 * kBeat);
    CHECK_NEAR(rig.e.LengthBeats(0), 3.0, 1e-9);
}

void TestFreeMode()
{
    std::printf("free mode: as long as you played, off the grid\n");
    Rig rig(kFree);
    rig.e.RecPlayDub(0);
    rig.RunTo(50016);
    rig.e.RecPlayDub(0);
    CHECK(rig.e.Length(0) == 50016);
    CHECK(!rig.e.OnGrid(0));
    CHECK(!rig.e.TempoLocked()); // free tracks don't care about tempo
}

void TestIndependentLengths()
{
    std::printf("tracks have their own lengths\n");
    Rig rig;
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar / 2);
    rig.e.RecPlayDub(0); // 1 bar
    rig.RunTo(kBar + kBlock);
    rig.e.RecPlayDub(1);
    rig.RunTo(4 * kBar + kBar / 2);
    rig.e.RecPlayDub(1); // started at bar 2 (+1 block), 4 bars
    rig.RunTo(5 * kBar + 2 * kBlock);
    CHECK(rig.e.Length(0) == kBar);
    CHECK(rig.e.Length(1) == 4 * kBar);
    CHECK_NEAR(rig.e.LengthBeats(1), 16.0, 1e-9);
    rig.RunTo(11 * kBar);
    CHECK(rig.e.Playhead(0) == 0);
    CHECK(rig.e.Playhead(1) == (11 * kBar - (kBar + kBlock)) % (4 * kBar));
}

// ------------------------------------------------------------- overdub

void TestOverdubUndoRedo()
{
    std::printf("overdub, undo restores it exactly, redo puts it back\n");
    Rig rig;
    rig.input = [](size_t i) { return Ramp(i); };
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar / 2);
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar + kBlock);
    const std::vector<int16_t> before = rig.l[0];

    rig.input = [](size_t) { return 0.1f; };
    rig.RunTo(2 * kBar);
    rig.e.RecPlayDub(0);
    CHECK(rig.e.State(0) == TrackState::Overdubbing);
    rig.RunTo(3 * kBar + kBar / 2); // one and a half passes
    rig.e.RecPlayDub(0);
    rig.input = [](size_t) { return 0.f; };
    rig.RunTo(4 * kBar);
    CHECK_NEAR(Sample(rig, 0, 30000), Ramp(30000) + 0.2, 4 * kQ); // two layers there
    const std::vector<int16_t> after = rig.l[0];

    CHECK(rig.e.Undo());
    CHECK(rig.e.UndoBusy());
    rig.Run(9600);
    CHECK(!rig.e.UndoBusy());
    CHECK(std::equal(before.begin(), before.begin() + kBar, rig.l[0].begin()));

    CHECK(rig.e.Undo()); // redo
    rig.Run(9600);
    CHECK(std::equal(after.begin(), after.begin() + kBar, rig.l[0].begin()));
}

void TestUndoWhileOverdubbing()
{
    std::printf("undo during an overdub stops and removes it\n");
    Rig rig;
    rig.input = [](size_t i) { return Ramp(i); };
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar / 2);
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar + kBlock);
    const std::vector<int16_t> before = rig.l[0];
    rig.input = [](size_t) { return 0.3f; };
    rig.RunTo(kBar + 30000);
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar + 60000);
    CHECK(rig.e.Undo());
    CHECK(rig.e.State(0) == TrackState::Playing);
    rig.Run(9600);
    CHECK(std::equal(before.begin(), before.begin() + kBar, rig.l[0].begin()));
}

void TestDubRequestedDuringUndoStartsAfter()
{
    std::printf("Rec/Dub pressed while an undo is applied starts right after\n");
    Rig rig;
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar / 2);
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar + kBlock);
    rig.e.RecPlayDub(0); // dub
    rig.RunTo(2 * kBar);
    rig.e.RecPlayDub(0); // play
    rig.Run(kBlock);
    CHECK(rig.e.Undo());
    rig.e.RecPlayDub(0); // too soon: queued
    CHECK(rig.e.State(0) == TrackState::Playing);
    rig.Run(9600);
    CHECK(rig.e.State(0) == TrackState::Overdubbing);
}

// --------------------------------------------------------------- undo

void TestUndoTakeAndClear()
{
    std::printf("undo/redo a take and a clear\n");
    Rig rig;
    rig.input = [](size_t i) { return Ramp(i); };
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar / 2);
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar + kBlock);

    CHECK(rig.e.Undo());
    CHECK(rig.e.State(0) == TrackState::Empty);
    rig.RunTo(2 * kBar + 5000);
    CHECK(rig.e.Undo()); // redo
    CHECK(rig.e.State(0) == TrackState::Playing);
    CHECK(rig.e.Length(0) == kBar);
    CHECK(rig.e.Playhead(0) == 5000); // back in its place on the grid

    rig.e.Clear(0);
    CHECK(rig.e.State(0) == TrackState::Empty);
    rig.RunTo(3 * kBar + 7000);
    CHECK(rig.e.Undo());
    CHECK(rig.e.State(0) == TrackState::Playing);
    CHECK(rig.e.Playhead(0) == 7000);
    CHECK(rig.e.Undo()); // redo the clear
    CHECK(rig.e.State(0) == TrackState::Empty);
}

void TestUndoRefusedAfterTempoChange()
{
    std::printf("a cleared grid track can't come back at a different tempo\n");
    Rig rig;
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar / 2);
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar + kBlock);
    rig.e.Clear(0);
    CHECK(rig.e.SetTempo(100.f)); // nothing left on the grid, so allowed
    CHECK(!rig.e.Undo());
    CHECK(rig.e.State(0) == TrackState::Empty);
    CHECK(rig.e.SetTempo(120.f));
    CHECK(rig.e.Undo()); // same tempo again: fine
    CHECK(rig.e.State(0) == TrackState::Playing);
}

void TestUndoCancelsRecording()
{
    std::printf("undo while recording cancels the take\n");
    Rig rig;
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar / 2);
    CHECK(rig.e.Undo());
    CHECK(rig.e.State(0) == TrackState::Empty);
    CHECK(rig.e.Length(0) == 0);
    CHECK(!rig.e.CanUndo());
}

void TestNewTakeInvalidatesUndo()
{
    std::printf("recording over a cleared track forgets its undo\n");
    Rig rig;
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar / 2);
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar + kBlock);
    rig.e.Clear(0);
    rig.e.RecPlayDub(0);
    CHECK(!rig.e.CanUndo());
}

// --------------------------------------------------------- tempo/transport

void TestTempo()
{
    std::printf("tempo: settable when free, locked once grid audio exists\n");
    Rig rig;
    CHECK(rig.e.SetTempo(100.f));
    CHECK_NEAR(rig.e.Tempo(), 100.0, 1e-3);
    rig.e.RecPlayDub(0);
    rig.RunTo(kBlock * 100);
    CHECK(rig.e.TempoLocked());
    CHECK(!rig.e.SetTempo(110.f));
    rig.RunTo(115200 + kBlock); // one bar at 100 BPM, end pending would be there
    rig.e.RecPlayDub(0);
    rig.RunTo(2 * 115200 + kBlock);
    CHECK(rig.e.Length(0) == 115200);
    rig.e.StartFresh();
    CHECK(rig.e.SetTempo(140.f));
    CHECK(!rig.e.SetTempo(10.f) || rig.e.Tempo() >= 40.f);
}

void TestBeatCounter()
{
    std::printf("beat counter ticks every beat, downbeats flagged\n");
    Rig rig;
    rig.e.TransportToggle();
    rig.Run(kBlock);
    CHECK(rig.e.BeatCount() == 1);
    CHECK(rig.e.LastBeatWasBar());
    rig.RunTo(kBeat + kBlock);
    CHECK(rig.e.BeatCount() == 2);
    CHECK(!rig.e.LastBeatWasBar());
    rig.RunTo(kBar + kBlock);
    CHECK(rig.e.BeatCount() == 5);
    CHECK(rig.e.LastBeatWasBar());
}

void TestStopAndPlay()
{
    std::printf("Stop freezes and silences; Play restarts from bar 1 in place\n");
    Rig rig;
    rig.input = [](size_t) { return 0.3f; };
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar / 2);
    rig.e.RecPlayDub(0);
    rig.input = [](size_t) { return 0.f; };
    rig.RunTo(kBar + 30000);
    rig.e.TransportToggle();
    rig.Run(4800);
    CHECK(!rig.e.Playing());
    const size_t ph = rig.e.Playhead(0);
    rig.Run(4800);
    CHECK(rig.e.Playhead(0) == ph);
    CHECK_NEAR(rig.out_l.back(), 0.0, 1e-5);
    rig.e.TransportToggle();
    CHECK(rig.e.Playhead(0) == 0);
    rig.Run(4800);
    CHECK_NEAR(rig.out_l.back(), 0.3, 1e-3);
}

void TestStopMidTakeKeepsWholeBars()
{
    std::printf("stopping mid-take keeps the whole bars recorded so far\n");
    Rig rig;
    rig.e.RecPlayDub(0);
    rig.RunTo(2 * kBar + 40000);
    rig.e.TransportToggle();
    CHECK(rig.e.State(0) == TrackState::Playing);
    CHECK(rig.e.Length(0) == 2 * kBar);
}

void TestMaxLengthTrimsToBars()
{
    std::printf("running out of memory keeps the whole bars\n");
    Rig rig(kBarNow, 250000);
    rig.e.RecPlayDub(0);
    rig.RunTo(260000);
    CHECK(rig.e.State(0) == TrackState::Playing);
    CHECK(rig.e.Length(0) == 2 * kBar);
}

void TestMuteAndStartFresh()
{
    std::printf("mute, unmute, start fresh\n");
    Rig rig;
    rig.input = [](size_t) { return 0.3f; };
    rig.e.RecPlayDub(0);
    rig.RunTo(kBar / 2);
    rig.e.RecPlayDub(0);
    rig.input = [](size_t) { return 0.f; };
    rig.RunTo(kBar + 4800);
    rig.e.StopToggle(0);
    CHECK(rig.e.State(0) == TrackState::Muted);
    rig.Run(4800);
    CHECK_NEAR(rig.out_l.back(), 0.0, 1e-4);
    rig.e.StopToggle(0);
    rig.Run(4800);
    CHECK_NEAR(rig.out_l.back(), 0.3, 1e-3);

    rig.e.StartFresh();
    CHECK(!rig.e.Playing());
    CHECK(rig.e.State(0) == TrackState::Empty);
    CHECK(!rig.e.CanUndo());
    rig.Run(4800);
    CHECK_NEAR(rig.out_l.back(), 0.0, 1e-5);
}

// --------------------------------------------------------- external clock

void TestExternalClockTakeAndDrift()
{
    std::printf("MIDI clock: follows tempo, bar takes, stays locked when it drifts\n");
    Rig rig(kBarArmed);
    rig.At(0, SyncType::Start);
    rig.Clock(0, kTick, 96 * 5);
    rig.RunTo(4800);
    CHECK(rig.e.ExternalClock());
    CHECK(rig.e.Playing());
    CHECK_NEAR(rig.e.Tempo(), 120.0, 0.5);
    CHECK(rig.e.TempoLocked());
    CHECK(!rig.e.TransportToggle()); // the P-6 is in charge

    rig.e.RecPlayDub(0); // armed: bar 2
    rig.RunTo(kBar + kBar / 2);
    CHECK(rig.e.State(0) == TrackState::Recording);
    rig.e.RecPlayDub(0); // ends on bar 3
    rig.RunTo(2 * kBar + kBlock);
    CHECK(rig.e.Length(0) == kBar);
    rig.RunTo(4 * kBar + 100);
    CHECK(std::abs(rig.e.LastPhaseError(0)) <= 1);
    CHECK(rig.e.PhaseCorrections() == 0);

    // Now the clock runs 0.05 % slow (far worse than real crystals): 48
    // frames of drift per bar.
    const size_t t0 = 5 * kBar;
    rig.Clock(t0, kTick * 1.0005, 96 * 40 + 1);
    int worst = 0;
    for(int b = 1; b <= 40; ++b)
    {
        rig.RunTo(t0 + static_cast<size_t>(std::floor(b * 96 * kTick * 1.0005)) + 1);
        worst = std::max(worst, std::abs(rig.e.LastPhaseError(0)));
    }
    std::printf("  worst error at a beat: %d frames, %u corrections\n", worst,
                static_cast<unsigned>(rig.e.PhaseCorrections()));
    CHECK(worst <= 72 + 12 + 2); // 1.5 ms threshold + one beat of drift
    CHECK(rig.e.PhaseCorrections() >= 10);
}

void TestExternalStopStart()
{
    std::printf("MIDI Stop stops; Start restarts on beat 1 in place\n");
    Rig rig;
    rig.At(0, SyncType::Start);
    rig.Clock(0, kTick, 96 * 2 + 30); // keeps ticking a little after Stop
    rig.RunTo(kBar + kBar / 4);
    rig.e.RecPlayDub(0); // immediate, at bar 2 + 1/4
    rig.RunTo(2 * kBar);
    rig.e.RecPlayDub(0); // ends one bar after it began
    rig.RunTo(2 * kBar + kBar / 4 + kBlock);
    CHECK(rig.e.Length(0) == kBar);

    rig.At(rig.now + 10, SyncType::Stop);
    rig.Run(4800);
    CHECK(!rig.e.Playing());

    const size_t s = rig.now + 1000;
    rig.At(s, SyncType::Start);
    rig.Clock(s + 500, kTick, 96 * 3);
    rig.RunTo(s + 500 + kBlock);
    CHECK(rig.e.Playing());
    // At bar 1 beat 1 this loop (which began a quarter-bar after a bar line)
    // is three quarters of the way through.
    CHECK_NEAR(static_cast<double>(rig.e.Playhead(0)), 3.0 * kBar / 4.0 + kBlock, 50.0);
    const uint32_t corrections = rig.e.PhaseCorrections();
    rig.RunTo(s + 500 + 2 * kBar + 96);
    CHECK(std::abs(rig.e.LastPhaseError(0)) <= 2);
    CHECK(rig.e.PhaseCorrections() == corrections);
}

void TestClockLossFallsBackToInternal()
{
    std::printf("clock dropping out falls back to the Pod's tempo\n");
    Rig rig;
    rig.At(0, SyncType::Start);
    rig.Clock(0, kTick * 0.8, 200); // 150 BPM
    rig.Run(100000);
    CHECK(rig.e.ExternalClock());
    CHECK_NEAR(rig.e.Tempo(), 150.0, 0.5);
    rig.RunTo(200000 + 30000);
    CHECK(!rig.e.ExternalClock());
    CHECK(!rig.e.Playing());
    CHECK_NEAR(rig.e.Tempo(), 150.0, 0.5); // carried over
    CHECK(rig.e.TransportToggle());
    CHECK(rig.e.Playing());
}

void TestExternalTakesOverInternal()
{
    std::printf("an arriving clock takes over from the internal transport\n");
    Rig rig;
    rig.e.TransportToggle();
    rig.RunTo(30000);
    CHECK(rig.e.Playing());
    rig.At(rig.now + 100, SyncType::Start);
    rig.Clock(rig.now + 200, kTick, 50);
    rig.Run(300);
    CHECK(rig.e.ExternalClock());
    CHECK(rig.e.Playing());
    CHECK(rig.e.PositionBeats() < 0.1);
}

void TestIgnoreClockWhenDisabled()
{
    std::printf("with clock following off, MIDI clock is ignored\n");
    Rig rig(kBarNow, kFrames, false);
    rig.At(0, SyncType::Start);
    rig.Clock(0, kTick, 50);
    rig.Run(20000);
    CHECK(!rig.e.ExternalClock());
    CHECK(!rig.e.Playing());
    CHECK(rig.e.TransportToggle());
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
    TestRecFromStoppedSnapsToBar();
    TestLatePressTrimsToTheLineJustPassed();
    TestImmediateStartMidBarKeepsItsPlace();
    TestArmedStartWaitsForTheBar();
    TestBeatQuantize();
    TestFreeMode();
    TestIndependentLengths();
    TestOverdubUndoRedo();
    TestUndoWhileOverdubbing();
    TestDubRequestedDuringUndoStartsAfter();
    TestUndoTakeAndClear();
    TestUndoRefusedAfterTempoChange();
    TestUndoCancelsRecording();
    TestNewTakeInvalidatesUndo();
    TestTempo();
    TestBeatCounter();
    TestStopAndPlay();
    TestStopMidTakeKeepsWholeBars();
    TestMaxLengthTrimsToBars();
    TestMuteAndStartFresh();
    TestExternalClockTakeAndDrift();
    TestExternalStopStart();
    TestClockLossFallsBackToInternal();
    TestExternalTakesOverInternal();
    TestIgnoreClockWhenDisabled();
    TestMonitorAndClip();
    if(failures)
    {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall tests passed\n");
    return 0;
}
