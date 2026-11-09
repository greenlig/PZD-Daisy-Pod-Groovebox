# Pod Looper

A small, screenless 4-layer stereo looper for a **stock Electrosmith Daisy
Pod**, synced to a **Roland P-6** over MIDI. No OLED, no SD card, no extra
wiring: just the Pod's own two buttons, two knobs, encoder and two RGB LEDs.

It deliberately starts small, in the spirit of
[Ouroboros](https://github.com/kooliha/Ouroboros_Loop_Station), and uses the
same looping model as the
[PZD Daisy Pod Groovebox](https://github.com/Peteranthony1st/PZD-Daisy-Pod-Groovebox)
so it can grow towards PZD's feature set over time (see [Roadmap](#roadmap)).
All code here is original; neither project's code is copied.

> **Status:** the firmware builds (91 KB, fits the Pod's internal flash) and
> the loop engine passes its tests on a PC, but it has not been run on a Pod
> yet. Expect a round of tweaks after the first real session.

## What it does

- **4 stereo layers** that share one loop length. The first layer you
  record sets it; every other layer then records in time with that loop.
- **Rec / Play / Overdub / Mute / Clear** per layer, pause-all, and a volume
  per layer.
- **Up to 40 seconds** per loop (all four layers in the Seed's 64 MB SDRAM).
- **Locked to the P-6's MIDI clock** when it's playing: the first loop starts
  and ends on bar lines, so it's a whole number of bars. It's nudged back into
  time every bar if the two clocks drift. P-6 Stop pauses the loops, and P-6
  Play restarts them on beat 1.
- **Remote control from the P-6's pads** (bank H) and from MIDI CCs.
- Live input passes straight through (Knob 2), so you hear the P-6 while you
  loop it.

## Hook-up

```
 P-6 MIX OUT ──(3.5 mm stereo)──▶ Pod IN
 P-6 MIDI OUT ─(3.5 mm TRS-TRS)─▶ Pod MIDI IN
 Pod OUT ──────────────────────▶ headphones / speakers
```

- Don't also connect the Pod's output to the P-6's MIX IN. The P-6 sends MIX
  IN back out of MIX OUT, which would feed the loops back into themselves.
- The Pod records whatever level arrives. Set the P-6's volume so its loudest
  hits don't distort; the Pod has no input gain.
- **To check MIDI is arriving:** with the P-6 playing and no loop recorded,
  LED 1 blinks off once per bar. If it doesn't, the P-6 isn't sending clock
  on its MIDI OUT (check its sync/MIDI settings) or the cable isn't the right
  type. Both ends need the same TRS wiring standard.

## Controls

| Control | What it does |
|---|---|
| **Turn encoder** | Select layer 1-4 (LED 1 shows which) |
| **Click encoder** | Pause / resume every layer |
| **Button 1** | Rec / Play / Dub on the selected layer (see below) |
| **Button 2, tap** | Mute / unmute the selected layer. Also cancels a recording that's waiting to start |
| **Button 2, hold 1 s** | Clear the selected layer (LED 2 flashes white) |
| **Button 2, hold 3 s** | Clear all layers |
| **Knob 1** | Selected layer's volume. It takes over once you move it, so switching layers never makes a level jump |
| **Knob 2** | Live input level: how loud the P-6 comes through. Recording isn't affected |

**Button 1** works like a looper pedal:

```
Empty ─▶ Recording ─▶ Playing ─▶ Overdubbing ─▶ Playing ─▶ …
                         Muted ─▶ Playing
```

- **First loop:** press to start, press again to finish. That sets the loop
  length.
  - If the P-6 is playing, the first press arms recording (LED 2 flashes red)
    and recording starts on the next bar line.
  - Your second press finishes it on the following bar line (LED 2 flickers
    until then).
- **Other layers:** start straight away, in time with the loop, and keep
  layering until you press again. Press once you've played one full pass;
  a second pass gets added on top of the first.
- Pressing Button 1 on an empty layer while the first loop is still recording
  closes that loop and starts the new layer immediately.

## LEDs

**LED 1 (the selected layer):** layer 1 cyan, layer 2 magenta, layer 3
yellow, layer 4 white. It blinks off at the top of the loop. With no loop
recorded yet, it blinks on every P-6 bar instead.

**LED 2 (what that layer is doing):**

| LED 2 | Meaning |
|---|---|
| off | empty |
| flashing red | armed: waiting for the bar line |
| red | recording the first loop (flickering = will stop at the next bar) |
| amber | overdubbing |
| green | playing |
| blue | muted |
| slow blink | everything is paused |
| white flash | cleared |

## Playing with the Roland P-6

**Sync.** Hit play on the P-6, then record the first loop on the Pod: it
will be a whole number of bars and stay locked to the P-6.

- Stopping the P-6 pauses the loops.
- Starting it again brings them back on beat 1, in the right place, even
  for a 2-bar loop recorded starting on bar 3.
- If the P-6 isn't playing, the Pod is a free looper and the first loop is
  as long as you make it. A free loop still restarts on beat 1 when you
  start the P-6, but it won't stay locked.
- Don't change the P-6's tempo after recording a synced loop. The loop
  doesn't time-stretch, so it gets pulled back to the bar line every bar,
  which sounds choppy. Clear and re-record at the new tempo.

**Pads as remote buttons.** The P-6 sends a note for each pad, on channel 11:
bank A pad 1 is note 48 and bank H pad 6 is note 95. Bank H's six pads (notes
90-95) control the looper. Leave bank H empty so these pads make no sound of
their own.

| Bank H pad | Looper action |
|---|---|
| 1-4 | Rec / Play / Dub on layer 1-4 (and select it) |
| 5 | Mute / unmute the selected layer |
| 6 | Pause / resume all |

The P-6 sends note-ons only, with no release, so hold gestures like Clear
stay on the Pod's Button 2.

**CCs** (channel 11): 102-105 set layer 1-4 volume, 106 sets the live input
level. These numbers are undefined in the MIDI spec and don't appear in the
P-6's CC list, so they're for other controllers.

Channel, pad notes, CC numbers, beats per bar and whether P-6 Stop pauses
the loops are all in [`src/config.h`](src/config.h).

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
| `src/config.h` | Settings you might change (MIDI channel, pad notes, loop length...) |
| `src/looper.h/.cpp` | The loop engine: layers, recording, overdub, MIDI clock sync. Plain C++ with no hardware code, so it's testable on a PC |
| `src/main.cpp` | The Pod: buttons, encoder, knobs, LEDs, MIDI, audio callback |
| `src/spsc_queue.h` | Lock-free queue for passing MIDI to the audio callback |
| `test/engine_test.cpp` | Engine tests (recording, overdub, clear, bar-synced takes, drift correction, Start/Stop) |

MIDI clock messages are timestamped the moment they arrive, so the loop's
start and end land within a sample or two of the P-6's bar line. That keeps
the loop length accurate, and only small drift corrections are needed.

## Roadmap

Small steps towards the PZD Groovebox, roughly in order of payoff:

1. **Undo** the last overdub (PZD has it; needs a background copy of the layer).
2. **Per-layer speed and pan** (Ouroboros and PZD both have these).
3. **Count-in / click** for free-running use without the P-6.
4. **Per-layer filter and one character effect**, then a shared reverb and
   delay.
5. **An OLED screen** (a cheap 128x64 I2C display, four wires to the Pod's
   header) for a real menu.
6. **SD card**: save and load loops.
7. **Synth/sampler engines** played from MIDI, recorded into layers.

## Credits

- [Ouroboros Loop Station](https://github.com/kooliha/Ouroboros_Loop_Station)
  by Alex Kulinchenko: the original idea of a simple buttons-and-knobs looper
  on the Daisy.
- [PZD Daisy Pod Groovebox](https://github.com/Peteranthony1st/PZD-Daisy-Pod-Groovebox):
  the shared-loop-length layer model and where this is heading.
- [libDaisy](https://github.com/electro-smith/libDaisy) (MIT) by
  Electrosmith.
