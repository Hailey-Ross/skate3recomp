# Research hooks

Tracing tools for studying how Skate 3 behaves at runtime: which sounds it plays and how loud, how
pedestrians, traffic and other skaters react, where trigger volumes are, and so on. They are function
overrides (`REX_FUNC`) around the recompiled game's functions. Every hook calls straight through to the
original and only *reads* guest memory, so the game behaves as before; the one exception is
`hooks_queue.cpp`, which enlarges an audio command buffer while tracing is on (see below).

Not intended for the upstream project. No game code, executable image or game data is included here: the
hooks reference functions only by address and read fields by offset.

> **Warning: reference and information gathering only.** A recompilation is not a perfect copy of the
> console game, and these traces are not ground truth:
> - The frame rate is uncapped (several hundred fps on a PC, against about 30 on an Xbox 360), so anything the
>   game does once per rendered frame (some sound updates, one-frame trigger pulses) happens at a different rate.
> - Timing and threading differ: the audio thread can stall for up to about a second, and there are hitches
>   when streaming at speed.
> - Treat logged values and program logic as strong evidence. Treat timings, rates per frame and gaps as
>   specific to the recompilation until checked against the code or the console.
> - The hooks themselves are research code. They are tested only as far as described here, and the addresses
>   are for title update 3 only.

## Building

1. Build [skate3recomp](https://github.com/mchughalex/skate3recomp) as its README describes, from this branch
   (`research-hooks`). You supply your own legally owned copy of the game, and the code generation step creates
   the recompiled sources locally (they are not in this repository).
2. Apply the SDK side: from `third_party/rexglue-sdk`, run `git apply ../../src/research/sdk/rexglue-sdk-research.patch`.
3. Configure and build as usual. The hooks in `src/research/` are compiled in by `CMakeLists.txt`.
4. Set the environment variables below and start the game. Any paths (trace file, capture, watch list,
   input script) are your own; nothing here assumes a particular folder layout.

## Files

| File | Category (`SKATE3_TRACE`) | What it logs |
|---|---|---|
| `trace_common.h` | — | guarded reads, caller chains, the category gate, `FIRST_PASS_HOOK` |
| `hooks_audio.cpp` | `audio`, `audiox`, `dsp`, `dspmod` | sounds posted/played (POST, PLAY, SPLC), board contacts (CONTACT, CSET), audio state (ASTATE), the rolling grain bed (GREC), treatment state (TREAT), seam patterns and hits (SEAMPAT, SEAMHIT), the local-player test's decision per object (LOCALTEST; the per-player kinds log only the local rider, byte `[[object+28]+72]`), world-painter audio regions (WP\*) and ambience (AMB\*); `dsp`: voice gains and sends (GAIN, SEND); `dspmod`: per-voice DSP module values (MOD: pitch, filters); `audiox`: extra per-frame detail, the board state word, slip, revert and listener position (GRECX), the bail first-hit flag and its strength on change plus once a second (FIRSTHIT), the held wheel-skid packet (SKID), each active world emitter's parameter slots (EMITSLOT), and during the local player's bails the ragdoll's per-region body impacts with their velocity change, contact normal, mass and surface tag per update (BAILLOCAL, BAILSTEP, BAILREG; BAILCAND shows the local-player lookup's inputs, rate-limited), and every message posted to the collision sound manager with its poster, owner, materials and tiers (COLLPOST), and every impact-band query with the impact, tier and band edges (BANDQ), and the posts, Splice sounds and wheel-spin streams of each per-player sound object (the local rider's and the nearby NPC skater's) with the object's local flag and MixMap key (PLAYERPOST) |
| `hooks_physics.cpp` | `physics`, `aiskater` | ground jump and board contact internals; the solver iteration count writer and its owner mode (ITERSET, ITERTICK); `aiskater`: every skater's board (player and NPC skaters): contacts, contact point, velocity (SKATEB, SKATER) |
| `hooks_world.cpp` | `world` | trigger volumes (stream-in, registration, enter/exit), teleports, world-painter key changes |
| `hooks_npc.cpp` | `npc` | pedestrian/vehicle population, mood reactions (MOODOUT), named per-pedestrian timers (PEDTIMER), chases, takedowns, tazers, positions (PEDXYZ, PEDSEE), pedestrian audio state per instance (PEDAUD) |
| `hooks_traffic.cpp` | `traffic` | vehicle driving state and position (VEHSTATE), vehicle audio record per engine instance (VEHAUD), horns, skids, traffic lights, car alarm trigger (VEHHIT collision callback, VEHALARMSTOP, VEHPARK); each car's junction state, segment and lane changes, i.e. turn choices at junctions (VEHCONN) |
| `hooks_vehicles.cpp` | `skitch`, `traffic` | `skitch`: skitching from both sides: the car's held latches with the car's speed, acceleration and speed cap and the skater's offset from the car (SKITCH), the skitch setter's calls (SKITCHCALL), the skater's state selector around Skitching (SKITCHST) and the grab candidates (SKITCHCAND), and the skater-vs-car wipeout term with its contact, limit and outcome (VEHBAIL); `traffic`: the four traffic-light controllers' phase programmes and phase changes (TRAFPROG, TRAFLIGHT2) and each car's junction query with the signal group of its approach (VEHJUNC); every hook writes one HOOKARMED line on its first call |
| `hooks_cas.cpp` | `cas` | which character record each skater actor is built from: the actor constructor with its record slot (CASACTOR), the record id and name it resolves (CASID, CASCHAR), the character settings getter with the natural stance byte and style it returns (CASSET), and each slot record once (CASREC) |
| `hooks_queue.cpp` | `audio` | the audio command queue: growth at start (QGROW), a watchdog every 100 ms (QSTAT) |
| `hooks_marker.cpp` | `audiox` | session-marker returns and the teleport flow: the request with its distance and hold time (TPMARK), the streamed-or-load decision (TPDEC, TPSTREAM), the teleport screen effect amount (TPFX), every front-end sound request (FEREQ), state changes and state events (GSTATE, GEVENT, rate-limited) and posted hub messages (HUBMSG, two per second per id) |
| `hooks_watch.cpp` | `watch` | a watch list of guest memory read from a file, logged on change |
| `sdk/rexglue-sdk-research.patch` | — | the SDK side (see below) |

Each file's header comment documents its line kinds and fields.

## Using it

A step-by-step guide (first trace, choosing categories, reading and checking a trace, watch lists, input scripts,
audio capture) is in [USAGE.md](USAGE.md). The reference:

Environment variables (all optional; nothing is traced unless `SKATE3_AUDIO_TRACE_FILE` is set):

| Variable | Meaning |
|---|---|
| `SKATE3_AUDIO_TRACE_FILE` | output file (tab-separated, one event per line: `KIND <ms> fields…`) |
| `SKATE3_TRACE` | comma list of categories to log (unset = all), e.g. `audio,dsp,npc,world,traffic,aiskater` (`skitch` = skitching and car contacts); `audiox` adds several hundred lines a second while skating, so name it only for runs that need it |
| `SKATE3_AUDIO_CAPTURE` | also record the mixed game output (stereo float32, 48 kHz); `CAPTURE` lines align it with the trace |
| `SKATE3_WATCH` | watch-list file for `hooks_watch.cpp` |
| `SKATE3_INPUT_SCRIPT` | play a pad input script (timed steps; see `input_script.h` in the patch) |
| `SKATE3_INPUT_SCRIPT_AUTOSTART_MS` | start the script after gameplay has been steady this long |
| `SKATE3_INPUT_RECORD` | record the real pad input to a script file |
| `SKATE3_BACKGROUND` | `1` = open the window without taking focus and ignore the real pad (unattended runs) |

The trace's first line is `CLOCK 0.0 <unix ms>`, which aligns trace time with wall-clock time (screenshots).

### Notes

- **One writer per process.** The exe (hooks) and the runtime DLL (file reads, XMA) share one trace writer:
  the first module to start owns the file and publishes an append function; lines are formatted on the
  calling thread and written in batches every 100 ms, so tracing barely touches the game threads.
- **Audio-thread hooks must stay light.** GAIN, SEND, PLAY and MOD run on the audio render thread inside the
  command-queue consumer. Heavy work there delays the queue's drain; the game appends to that queue without a
  bounds check, and an overflow crashes it. `hooks_queue.cpp` enlarges the buffer (4 MB instead of
  256,000 bytes) while tracing is on and logs its fill level; `dspmod` is the heaviest category.
- **Guarded reads only.** Every read through a pointer whose meaning is inferred goes through `Readable()` /
  `Try*` (an exception-guarded probe).
- Check a new trace for malformed lines before using it; there should be none.

## SDK patch

`sdk/rexglue-sdk-research.patch` applies to `third_party/rexglue-sdk` (`git apply`, from that directory). It
adds the trace writer (`include/rex/audio/audio_trace.h`), scripted/recorded pad input
(`include/rex/input/input_script.h`), trace points for file reads, XMA decoding and the audio output, and the
background-window option; it also comments out two `RasterizerGamma` lines that the public imgui pin lacks.
It also adds `InputSystem::SetGuestPadObserver`, which hands the app the pad state the game receives (after
the background isolation and any input script); `src/skate3_app_common.cpp` uses it for the settings hold-chord
below.

## Controller: open the settings / quit

Holding **LB + RB + Back for 1 second** opens the settings screen (the same one as Escape / F1), so a controller
alone can reach it, for example from a couch over Steam Link. In the settings screen the controller navigates
(D-pad / left stick, A select, B back); **Close Game** under the categories quits to the desktop. The chord fires
once per hold, only after its buttons have been released since the last time, and only opens the screen (B or
Close Settings closes it).

- `skate3_menu_pad_chord` (default `lb+rb+back`): buttons joined by `+`: `a b x y lb rb lt rt l3 r3 back start
  up down left right` (`lt`/`rt` count as held over half way). Empty disables it.
- `skate3_menu_pad_hold_ms` (default `1000`): how long the chord must be held.

No button is taken from the game: it still sees the chord's buttons while they are held. With the default chord,
Back opens the game's Instant Replay underneath, and LB / RB move through it until the settings screen opens.
The chord reads the same pad state the game does, so an input script step such as `1500 lb rb back` triggers
it too. The SDK's own `menu_chord` (default `rb+start`, no hold) is unchanged.

## Credits

Built on [skate3recomp](https://github.com/mchughalex/skate3recomp) by @mchughalex and the
[rexglue SDK](https://github.com/rexglue/rexglue-sdk), with credit to [Xenia](https://github.com/xenia-project/xenia)'s
Xbox 360 research.
