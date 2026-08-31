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

One line is one part. Everything loops on its own length, so parts of different
lengths drift against each other, which is free polymeter.

```
# comments start with a hash
bpm 124

bd     x...x...x...x...
sd     ....x.......x..x
hh:0.35 x.x.x.x.x.x.x.x.

saw:0.30:700 c2 . eb2 . g2 . eb2 .
```

**Head token**: `name` or `name:gain` or `name:gain:cutoff` or
`name:gain:cutoff:resonance`.

`name` is a drum (`bd sd hh oh cp rim tom`) or a waveform
(`sine saw square tri noise`).

**Steps**: if the rest of the line contains a space it is read as whitespace
separated tokens, otherwise every character is one step. So drums stay a compact
grid and melodies stay readable.

| Step | Meaning |
|---|---|
| `.` `~` `-` `_` | rest |
| `x` | hit, or the base note on a melodic line |
| `X` | accent |
| `1`..`9` | velocity, on a drum line |
| `c2` `eb3` `f#4` | note name, c3 is midi 48 |

**Directives**: `bpm <n>`, `gain <0..2>`, `delay <orbit> <time> <feedback> <mix>`.

Drums play on orbit 0, melodic parts on orbit 1, which has the delay by default.

## Layout

```
components/strudel_dsp/   voice engine, pure C, no ESP-IDF, host testable
main/app_audio.c          parser, step sequencer, I2S task
main/main.c               editor and drawing
test/host/                gcc build of the portable parts
```

`cd test/host && make check` renders four seconds of a beat to `beat.wav` and
checks it for NaNs, clipping and silence, without touching hardware.

## Next

- Move the parser into `components/strudel_core` so it is host testable too
- Real pattern algebra: `[a b]`, `<a b>`, `a*2`, euclid `bd(3,8)`, `?`
- Sample playback from the SD card
- Reverb, per part pan and swing
- Save and load sets

## License

MIT.
