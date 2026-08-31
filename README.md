# Tanmatsu live coding instrument

A standalone live coding music instrument for [Tanmatsu](https://tanmatsu.cloud):
type patterns on the built in keyboard, hit eval, hear the change without the
transport stopping. Inspired by [Strudel](https://strudel.cc) and TidalCycles,
but it is a native C engine, not a port — no browser, no JavaScript, no host
computer.

## Status

Milestone 1 is done: keyboard, display and audio work together as one loop.

- 24 voice polyphonic engine on the ES8156 codec, 48 kHz stereo over I2S
- Synthesized drums (bd sd hh oh cp rim tom), so a beat plays with no samples
- Band limited saw and square, sine, triangle, noise, per voice state variable
  filter, waveshaper, and a delay send per orbit
- Sample accurate step timing: the sequencer runs inside the audio task and
  renders in sub blocks up to each step boundary
- Full screen editor with live step lanes showing the playhead

## Building

```
make build DEVICE=tanmatsu
make flash DEVICE=tanmatsu PORT=/dev/ttyACM0
```

The build needs ESP-IDF v6.0.2. Either run `make prepare`, or point the project
at an existing checkout with a `.IDF_PATH` file or an `esp-idf` symlink.

## Keys

| Key | Action |
|---|---|
| F1 | play / stop |
| F2, Ctrl+Enter | evaluate the buffer |
| F3 / F4 | tempo down / up |
| F5 | panic, stop everything |
| F6 | reload the demo |
| Volume up/down | master gain |
| ESC | back to the launcher (Tanmatsu has no F7-F12) |

## The language

One line is one part. Every part loops on its own length, so parts of different
lengths drift against each other, which is free polymeter.

```
# comments start with a hash
bpm 128
delay 1 0.1875 0.42 0.30

bd      x ~ ~ x ~ ~ x ~
hh:0.26 x*8?0.2
cp:0.34 x(3,8,3)

saw:0.26:620:0.55  <c2 g1 c2 bb1> ~ [~ c2] ~
```

**Head token**: `name`, or `name:gain`, `name:gain:cutoff`,
`name:gain:cutoff:resonance`. `name` is a drum (`bd sd hh oh cp rim tom`) or a
waveform (`sine saw square tri noise`).

**Pattern**: mini notation, the terse syntax from TidalCycles.

| | |
|---|---|
| `bd sd` | two steps in a cycle |
| `bd [sd sd]` | brackets subdivide a step |
| `<bd sd>` | angle brackets pick one per cycle |
| `bd*4` `bd/2` | faster within the step, or stretched over cycles |
| `bd(3,8)` `bd(3,8,2)` | euclidean rhythm, third argument rotates |
| `bd!3` | three separate steps |
| `bd@3 sd` | first step is three times as wide |
| `bd _ sd` | `_` extends the previous step |
| `bd?` `bd?0.3` | randomly drop, optionally with a probability |
| `bd:2` | sample index |
| `bd, hh*4` | comma stacks layers |
| `~` or `.` | a rest |

Steps are `x` for a hit and `X` for an accent, `1`..`9` for velocity on a drum
line, and note names like `c2` `eb3` `f#4` on a melodic one, where `c3` is midi
48. A bare number on a melodic line is a semitone offset from c2.

The older step grid still works and is often clearer for drums: a pattern of
only `x X . ~ - _ 0-9` with no spaces, such as `bd x...x...x...x...`, is read as
a grid and turned into the same pattern a sequence would make.

**Directives**: `bpm <n>`, `gain <0..2>`,
`delay <orbit> <time> <feedback> <mix>`. Drums play on orbit 0 and melodic
parts on orbit 1, which carries the delay by default.

A line that fails to parse is reported in the footer and skipped; the rest of
the program keeps playing.

## Layout

```
components/strudel_core/  pattern algebra and mini notation, no ESP-IDF
components/strudel_dsp/   voice engine, no ESP-IDF
main/app_audio.c          line parser, scheduler, I2S task
main/main.c               editor and drawing
test/host/                gcc build of the portable parts
```

`cd test/host && make check` renders four seconds of a beat to `beat.wav` and
checks it for NaNs, clipping and silence, without touching hardware.

## Next

- Sample playback from the SD card
- Reverb, per part pan and swing
- Per step control patterns, so gain and cutoff can be sequenced too
- Save and load sets

## License

MIT.
