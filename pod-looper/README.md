# Pod Looper

A screenless 4-track looper for a **stock Electrosmith Daisy Pod**, with its
own tempo or synced to a **Roland P-6** over MIDI. No OLED, no extra wiring:
just the Pod's own two buttons, two knobs, encoder and two RGB LEDs.

It deliberately starts small, in the spirit of
[Ouroboros](https://github.com/kooliha/Ouroboros_Loop_Station), and aims
towards the feature set of the
[PZD Daisy Pod Groovebox](https://github.com/Peteranthony1st/PZD-Daisy-Pod-Groovebox)
(see [Roadmap](#roadmap)). All code here is original; neither project's code
is copied.

> **Status:** step 1 of the V1 spec (audio tracks on a tempo grid). The
> firmware builds (95 KB, fits the Pod's internal flash) and the loop engine
> passes 26 tests on a PC, but it has not been run on a Pod yet. Expect a
> round of tweaks after the first real session.

## What it does

- **4 independent stereo tracks**, each with its **own length**: whole bars,
  whole beats, or free. A 1-bar drum loop and a 4-bar chord loop play side
  by side.
- **Tempo grid** in 4/4: the Pod's own tempo (40-240 BPM), or the P-6's MIDI
  clock whenever it's sending. **LED 1 pulses on every beat.**
- **Press to record, play, press again to close the loop.** The end snaps to
  the bar (or beat). Five record modes: bar or beat quantise, each with an
  immediate or armed start, plus free.
- **Overdub**, **mute**, **long-press clear**, **Start Fresh**.
- **Single-level undo/redo** of the last take, overdub or clear.
- **Up to 40 seconds** per track (20 bars at 120 BPM).
- **Remote control** from the P-6's pads and from MIDI CCs.
- Live input passes straight through (Knob 2), so you hear what you're
  looping.

## Hook-up

```
 P-6 MIX OUT ──(3.5 mm stereo)──▶ Pod IN
 P-6 MIDI OUT ─(3.5 mm TRS-TRS)─▶ Pod MIDI IN
 Pod OUT ──────────────────────▶ headphones / speakers
```

Any line-level source works in place of the P-6's audio (a Poly D, say),
and the P-6 can still provide the clock.

- Don't also connect the Pod's output to the P-6's MIX IN. The P-6 sends MIX
  IN back out of MIX OUT, which would feed the loops back into themselves.
- The Pod records whatever level arrives. Set the source's volume so its
  loudest hits don't distort; the Pod has no input gain.
- **To check MIDI clock is arriving:** start the P-6. LED 1 should pulse in
  time with it, and a click of the encoder should flash LED 2 red, because
  the P-6 is in charge of Play/Stop. If neither happens, the P-6 isn't
  sending clock on its MIDI OUT (check its sync/MIDI settings) or the cable
  is the wrong type. Both ends need the same TRS wiring standard.

## Controls

| Control | What it does |
|---|---|
| **Turn encoder** | Select track 1-4 (LED 1's colour) |
| **Click encoder** | Play / Stop (Play always starts from bar 1) |
| **Button 1** | Rec / Play / Dub on the selected track (see below) |
| **Button 2, tap** | Mute / unmute the selected track. Also cancels a recording that's waiting to start |
| **Button 2, hold 1 s** | Clear the selected track (LED 2 flashes white) |
| **Button 2, hold 3 s** | **Start Fresh**: clear every track and stop |
| **Knob 1** | Selected track's volume. It takes over once you move it, so switching tracks never makes a level jump |
| **Knob 2** | Live input level. Recording isn't affected |

**The encoder is also Shift.** Hold it down and:

| While holding the encoder | What it does |
|---|---|
| **Turn** | Tempo down / up 1 BPM |
| **Button 1** | **Undo / redo** |
| **Button 2** | Next record mode |

A red flash on LED 2 means "can't do that right now". For example:

- changing the tempo while bar-synced tracks exist
- pressing Play/Stop while the P-6 is in charge
- undo with nothing to undo

### Recording

**Button 1** cycles a track like a looper pedal:

```
Empty ─▶ Recording ─▶ Playing ─▶ Overdubbing ─▶ Playing ─▶ …
                         Muted ─▶ Playing
```

- If everything is stopped, Button 1 starts the transport and recording
  begins on beat 1. If the P-6 is in charge, the track arms and starts
  recording when you press Play on the P-6.
- **Closing a loop** (bar or beat modes):
  - Press any time in the last bar and recording carries on to the bar line
    (LED 2 flickers until then).
  - Press just after a bar line (up to 150 ms late) and the loop ends on
    that line instead; nothing you meant to keep is lost.
  - The result is a whole number of bars (or beats), counted from where you
    started.
- **An immediate start mid-bar** keeps its place: the loop is still whole
  bars long, and what you played on beat 3 plays back on beat 3.
- **Overdubbing** layers on top each pass until you press again.

**Record modes** (Shift + Button 2 steps through them; LED 2 shows the
current one while you hold the encoder):

| Mode | LED 2 | Start | Length |
|---|---|---|---|
| Bar, immediate *(default)* | green | on the press | whole bars |
| Bar, armed | green, blinking | on the next bar line | whole bars |
| Beat, immediate | cyan | on the press | whole beats |
| Beat, armed | cyan, blinking | on the next beat | whole beats |
| Free | white | on the press | exactly as played, off the grid |

### Undo

Shift + Button 1 undoes the last take, overdub or clear, on whichever track
it happened. Press again to redo.

- During an overdub, undo stops the overdub and removes it.
- During a recording, undo cancels the recording.
- An undone overdub is put back in the background, starting at the
  playhead, so you never hear a glitch. Button 1 pressed in that moment
  (well under a second) takes effect as soon as it's done.

### Tempo

- With nothing synced to the bars yet, set the tempo with Shift + turn.
  LED 1's beat pulse shows it.
- Once a bar or beat track exists the tempo locks, because changing it
  would break the loops. Free tracks don't lock it.
- When MIDI clock is arriving, the P-6 sets the tempo and runs Play/Stop.
- If the clock stops, the Pod keeps the P-6's tempo, so your tracks still
  fit.

## LEDs

**LED 1 (the selected track and the beat):** the track's colour (1 cyan,
2 magenta, 3 yellow, 4 white), dim between beats, bright on each beat and
white on the first beat of every bar. Steady dim = stopped.

**LED 2 (what that track is doing):**

| LED 2 | Meaning |
|---|---|
| off | empty |
| flashing red | armed: waiting for the beat/bar line (or for Play) |
| red | recording (flickering = will stop at the next line) |
| amber | overdubbing |
| green | playing |
| blue | muted |
| white flash | done (clear, undo, Start Fresh) |
| red flicker | can't do that right now |

## Playing with the Roland P-6

**Sync.** While the P-6 is sending MIDI clock it sets the tempo.

- P-6 Stop stops the looper.
- P-6 Play restarts it on bar 1, with every track in its right place.
- Tracks are nudged back into time on every beat if the two clocks drift
  (a 5 ms crossfade hides it).
- Don't change the P-6's tempo after recording bar tracks. They don't
  time-stretch, so they'd keep being pulled back into place, which sounds
  choppy.
- If your controller sends clock even while stopped and you want the Pod's
  own tempo instead, set `kFollowMidiClock = false` in
  [`src/config.h`](src/config.h).

**Pads as remote buttons.** The P-6 sends a note for each pad, on channel 11:
bank A pad 1 is note 48 and bank H pad 6 is note 95. Bank H's six pads (notes
90-95) control the looper. Leave bank H empty so these pads make no sound of
their own.

| Bank H pad | Looper action |
|---|---|
| 1-4 | Rec / Play / Dub on track 1-4 (and select it) |
| 5 | Mute / unmute the selected track |
| 6 | Undo / redo |

The P-6 sends note-ons only, with no release, so hold gestures like Clear
stay on the Pod's Button 2.

**CCs** (channel 11): 102-105 set track 1-4 volume, 106 sets the live input
level. These numbers are undefined in the MIDI spec and don't appear in the
P-6's CC list, so they're for other controllers.

The channel, pad notes, CC numbers, default tempo, default record mode and
clock following are all in [`src/config.h`](src/config.h).

## Flashing (no SD card needed)

A ready-built binary is in [`firmware/pod-looper.bin`](firmware/pod-looper.bin).
It runs from the Seed's internal flash, so a stock Pod needs no bootloader
and no SD card.

1. Connect the Pod's Daisy Seed to your computer with USB.
2. Put it in DFU mode: hold **BOOT**, tap **RESET**, release **BOOT**.
3. Open the [Daisy Web Programmer](https://flash.daisy.audio/) in Chrome,
   connect, choose `pod-looper.bin` and flash it.

If you've ever installed the Daisy bootloader on this Seed (for example for
PZD's firmware), this overwrites it, which is fine. To go back to PZD
later, follow PZD's own flashing steps.

## Building from source

You need the `arm-none-eabi` GCC toolchain (part of the
[Daisy toolchain](https://daisy.audio/tutorials/cpp-dev-env/)).

```bash
git submodule update --init --recursive
```

```bash
make -C pod-looper/libDaisy
```

```bash
make -C pod-looper
```

That builds `pod-looper/build/pod-looper.bin`. With the Pod in DFU mode,
`make -C pod-looper program-dfu` flashes it.

The loop engine has tests that run on a normal computer, no Pod needed:

```bash
make -C pod-looper/test
```

## How the code is laid out

| File | What's in it |
|---|---|
| `src/config.h` | Settings you might change (MIDI channel, pad notes, tempo, record mode...) |
| `src/looper.h/.cpp` | The engine: tempo grid, tracks, record modes, overdub, undo, MIDI clock sync. Plain C++ with no hardware code, so it's testable on a PC |
| `src/main.cpp` | The Pod: buttons, encoder, knobs, LEDs, MIDI, audio callback |
| `src/spsc_queue.h` | Lock-free queue for passing MIDI to the audio callback |
| `test/engine_test.cpp` | Engine tests: snapping, late presses, immediate/armed starts, independent lengths, undo/redo, tempo lock, Stop/Play, MIDI clock and drift |

**Notes on the code:**
- Audio is stored as 16-bit with 6 dB of headroom for overdubs.
- 4 tracks plus one undo buffer use 37.5 MB of the Seed's 64 MB SDRAM. The
  rest is kept for samples.
- MIDI clock messages are timestamped the moment they arrive, so takes start
  and end within a sample or two of the P-6's beat lines.

## Roadmap

Towards the V1 spec:

1. ~~**Tempo grid and audio tracks**: independent bar lengths, record modes,
   undo, beat LED.~~ *(this version)*
2. **Session save and restore on the SD card**: tracks written in the
   background as you go (the Pod gets no warning before power is cut),
   restored stopped until Play. Start Fresh already exists.
3. **MIDI tracks** driving an onboard synth and sampler, with MIDI overdub
   (stack or replace) and optional note quantise.

After V1: song sections, an OLED screen, and more of PZD's per-track effects.

## Credits

- [Ouroboros Loop Station](https://github.com/kooliha/Ouroboros_Loop_Station)
  by Alex Kulinchenko: the original idea of a simple buttons-and-knobs looper
  on the Daisy.
- [PZD Daisy Pod Groovebox](https://github.com/Peteranthony1st/PZD-Daisy-Pod-Groovebox):
  where this is heading.
- [libDaisy](https://github.com/electro-smith/libDaisy) (MIT) by
  Electrosmith.
