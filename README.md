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
| F6 | next starter tune |
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

**Control clauses**: anything after the pattern of the form `field=pattern`
sequences that control. The value is itself a pattern, and the structure always
comes from the left, so the rhythm stays the pattern's and the values are
sampled from the clause.

```
hh  x*8         g=29492949           quiet, loud, mid, loud, ...
saw c2 eb2 g2   c=<620 1400> q=.6    the filter alternates each cycle
bd  x*4         p=<.3 .7>            the kick moves across the stereo field
saw c3 eb3      nt+=<0 12>           up an octave every other cycle
hh  x*16        g*=.5                half as loud as it was
```

`field=` sets, `field+=` adds and `field*=` multiplies. Fields have short names
so a line still fits: `g` gain, `p` pan, `c` cutoff, `q` resonance, `nt` note
offset in semitones, `sp` speed, `lg` legato, `atk` `dec` `sus` `rel` envelope,
`shp` shape, `ob` orbit. The full names work too.

In a 0..1 control a run of digits is a per step grid where each digit is that
many ninths, which is the same ninth the velocity grid already uses: `g=29492949`
is eight steps. Write a decimal point when you mean a plain number: `g=.5`.

The head's `name:gain:cutoff:resonance` shorthand still works and is just a
shorter way of writing the first three clauses.

**Directives**: `bpm <n>`, `gain <0..2>`, `swing <0..0.75>`,
`delay <orbit> <time> <feedback> <mix>`, `reverb <size> <damp> <mix>`,
`room <orbit> <send>`. Drums play on orbit 0 and melodic parts on orbit 1,
which carries the delay by default.

A line that fails to parse is reported in the footer and skipped; the rest of
the program keeps playing.

## Starter tunes

F6 cycles four of them: `house`, `break` (a breakbeat in the amen idiom at
174), `liquid` (rolling two step drum and bass) and `dub` (sparse, mostly
delay). They are ordinary editor buffers, so every one of them is something you
could have typed, and every one can be pulled apart while it plays.

## Saving sets

| Key | Action |
|---|---|
| Ctrl+S | save to the current slot |
| Ctrl+O | load the current slot |
| Ctrl+1 to Ctrl+8 | choose a slot |

Other keys worth knowing, and all of them are listed on the badge under Ctrl+H:
Ctrl+M mutes a line, Alt and a digit mutes that part wherever the cursor is,
Ctrl+D duplicates, Ctrl+K deletes, Ctrl+Z undoes, Alt with up or down moves a
line, and Ctrl+Space cycles the step under the cursor. Escape has to be pressed
twice to leave, because one stray keypress should not end a set.

Sets are plain text in `live/setN.txt`. They go to the internal FAT partition,
because a clean Tanmatsu has nothing else and sets should be where you left
them; a microSD card is used when the internal partition is unavailable. Both
mounts refuse to reformat on failure, since that partition also holds the
launcher's record of every installed app.

Because they are plain text they can be pushed and pulled with badgelink:

```
cd badgelink/tools
./badgelink.sh fs download /int/live/set0.txt set0.txt
```

USB mass storage is a third possible home, but it needs USB host mode, which
conflicts with badge link, so it is not wired up.

## Layout

```
components/strudel_core/  pattern algebra and mini notation, no ESP-IDF
components/strudel_dsp/   voice engine, no ESP-IDF
main/app_audio.c          line parser, scheduler, I2S task
main/app_store.c          saving and loading sets
main/main.c               editor and drawing
test/host/                gcc build of the portable parts
```

`cd test/host && make check` renders four seconds of a beat to `beat.wav` and
checks it for NaNs, clipping and silence, without touching hardware.

## Next

- Sample playback from the SD card
- USB mass storage for sets
- Reverb, per part pan and swing
- Per step control patterns, so gain and cutoff can be sequenced too
- Save and load sets

## License

MIT.
