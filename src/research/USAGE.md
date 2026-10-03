# Using the research hooks

A step-by-step guide to recording and reading traces with the research hooks. Build first (see
[README.md](README.md#building)); this page assumes a working `skate3.exe` built from the `research-hooks` branch.

> **Reference only.** The recompilation is not the console game: the frame rate is uncapped, and timing and
> threading differ. Trust logged values and program logic; check timings and per-frame rates against the code
> before relying on them. Addresses are for title update 3 only.

## 1. A first trace

Nothing is traced until `SKATE3_AUDIO_TRACE_FILE` is set. Set the variables in the shell that starts the game.

PowerShell:

```powershell
$env:SKATE3_AUDIO_TRACE_FILE = "$PWD\traces\first.tsv"
$env:SKATE3_TRACE = "audio"
.\out\build\relwithdebinfo\skate3.exe
```

cmd:

```bat
set SKATE3_AUDIO_TRACE_FILE=%CD%\traces\first.tsv
set SKATE3_TRACE=audio
out\build\relwithdebinfo\skate3.exe
```

Linux:

```sh
SKATE3_AUDIO_TRACE_FILE="$PWD/traces/first.tsv" SKATE3_TRACE=audio ./out/build/linux-relwithdebinfo/skate3
```

Create the output folder first; the writer does not make folders. The trace file is appended to, not replaced,
so give each session its own file name (a label and a timestamp work well); otherwise two runs end up mixed in
one file.

## 2. Choosing categories

`SKATE3_TRACE` is a comma list. Unset means every category, which is large; name only what the session needs.

| Goal | Categories |
|---|---|
| Which sounds play for an action, and when | `audio` |
| How loud each voice is, and where it is sent | `audio,dsp` |
| Pitch and filter values per voice | `audio,dsp,dspmod` (heaviest) |
| Board state, skids, bail first hit, emitter slots per frame | `audio,audiox` |
| Board contacts of every skater, player and NPC | `physics,aiskater` |
| Trigger volumes, teleports, world-painter keys | `world` |
| Pedestrians, moods, chases | `npc` |
| Vehicles, horns, traffic lights | `traffic` |
| Any memory you name in a file (see section 5) | `watch` |

`dsp`, `dspmod` and `audiox` add hundreds to thousands of lines a second. Keep those runs short (one or two
minutes around the action you are studying).

## 3. Reading a trace

The file is tab-separated text, one event per line:

```
KIND <ms> field field ...
```

- `<ms>` is milliseconds since the trace started.
- The first line is `CLOCK 0.0 <unix ms>`. Add it to `<ms>` to get wall-clock time, for example to line a
  trace up with screenshots or a video.
- Each hook file's header comment lists its line kinds and what every field means (for audio:
  `hooks_audio.cpp`; for the input script and the writer: the headers in the SDK patch).
- Pointers in a line (players, modules, objects) are guest addresses. They are stable for the life of an object,
  so they join lines together: for example `PLAY` and `GAIN` lines share the player pointer.
- Caller chains are return addresses up the guest stack. Look them up in your locally generated sources to see
  which gameplay function asked for the event.

**Check every trace for malformed lines before using it.** A line with the wrong field count or a non-numeric
time means the session is suspect; record it again rather than parsing around it. A minimal check in Python:

```python
import sys
bad = 0
for n, line in enumerate(open(sys.argv[1], encoding="utf-8", errors="replace"), 1):
    parts = line.rstrip("\n").split("\t")
    try:
        float(parts[1])
    except (IndexError, ValueError):
        bad += 1
        if bad <= 10:
            print(f"line {n}: {line[:120]!r}")
print(f"{bad} malformed")
```

## 4. Marking moments in a session

When you play by hand, note what you did and roughly when (wall clock), then use the `CLOCK` line to find that
point. For repeatable runs use an input script: labelled steps write `MARK <label>` lines into the trace, so every
action is located exactly.

## 5. Watching memory without writing a hook

`SKATE3_WATCH=<file>` with the `watch` category logs any guest memory you name, once per game frame:

```
# label   type  address chain                 [every=<ms>]
wp_key    u64   0x83083C38 @ +0x2F0B0
mode      u32   0x830CFDC4 @ +892             every=1000
```

- Types: `u8 u16 u32 s32 u64 f32 vec3 hex<N>` (N bytes, 1 to 64).
- Chain: a start address, then steps: `@` loads a 32-bit pointer from the current address, `+N` / `-N` adds an
  offset (hex or decimal). Every load is guarded.
- Output: `WATCH <ms> <label> <value>` when the value changes, and also every `<ms>` when `every=` is given.
  A pointer that can't be read logs `unreadable`.

## 6. Scripted and recorded input

For sessions that must be repeatable, drive the pad from a script instead of playing by hand.

- `SKATE3_INPUT_RECORD=<file>` records your real pad from the first moment of gameplay into a script.
- `SKATE3_INPUT_SCRIPT=<file>` plays a script. It starts when L3 and R3 are pressed together on the real pad, or
  automatically after `SKATE3_INPUT_SCRIPT_AUTOSTART_MS` of steady gameplay.
- `SKATE3_BACKGROUND=1` opens the window without taking focus and ignores the real pad, so an unattended run can
  go on while you use the PC for something else.

Script format, one step per line (`#` starts a comment; a comment after a step is its label):

```
10p gameplay                # wait until gameplay has run for 10 polls
2000 ly=1                   # push forward for 2 s  -> MARK push
500 a                       # hold A for 0.5 s     -> MARK ollie_setup
30p                         # neutral for 30 polls
```

- Duration: `<n>p` counts input polls (game frames; recordings use this so replays stay in step even when the
  game hitches), `<ms>` is wall time (easier for hand-written scripts).
- Tokens: `a b x y lb rb start back up down left right l3 r3`, `lt rt` (full) or `lt=<0..1> rt=<0..1>`, sticks
  `lx= ly= rx= ry=` from -1 to 1 (up and right positive). No tokens means a neutral pad.
- `gameplay`: hold neutral until gameplay has lasted the duration without a pause or loading break. Recordings
  insert one at every return to gameplay, so different loading times don't shift later inputs.

Long replays drift away from the original session (physics differs slightly run to run). Prefer short scripts
for a single action, repeated, over replaying a long recording.

## 7. Recording the game's own audio

`SKATE3_AUDIO_CAPTURE=<file>` also writes the mixed game output as raw samples with no header (stereo,
interleaved, little-endian float32 at 48 kHz; import it as raw data, or add a WAV header). It is taken before the
mute, so muted background runs still capture. `CAPTURE` lines in
the trace give the sample position against trace time, so a sound in the recording can be matched to the
`PLAY` line that started it.

## 8. Keeping the game stable while tracing

- Audio-thread categories (`dsp`, `dspmod`) are the costliest. If the game hitches or crashes under them, trace
  fewer categories or shorter sessions.
- `hooks_queue.cpp` enlarges the audio command buffer while tracing is on and logs its fill (`QSTAT`). A fill
  close to the limit means the trace is too heavy for that session.
- Run one traced instance at a time.

## Credits

Built on [skate3recomp](https://github.com/mchughalex/skate3recomp) by @mchughalex and the
[rexglue SDK](https://github.com/rexglue/rexglue-sdk), with credit to [Xenia](https://github.com/xenia-project/xenia)'s
Xbox 360 research.
