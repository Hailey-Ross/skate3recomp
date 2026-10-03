// Research hooks (tracing tools; not intended for the upstream project): logs which
// sample each EA audio voice starts and the gain its Gain module applies, so
// retail's audible mix per gameplay action can be measured. Active only when
// SKATE3_AUDIO_TRACE_FILE is set (see rex/audio/audio_trace.h). Addresses are
// for the TU 3.0.3.0 build.
//
//   PLAY <ms> <player> <sndplayer> <sample addr> <level> <flags> <hex of 48 bytes at sample>
//       sub_82B32DC8 = SndPlayer1 play command (audio thread, every sample
//       start); record +4 SndPlayer1, +32 EAAC sample address (guest, inside
//       a loaded bank: join with READ lines), +46 flags byte, +48 level (f32).
//   GAIN <ms> <player> <module> <target> <applied>
//       sub_82B23B50 = Gain module process (per voice per block); module +12
//       owner player, +52 target gain, +56 applied gain. Logged on change only.
//   SEND <ms> <owner> <object> <target> <channels> <channel gains...>
//       sub_82B31838 = mix one source into its bus blocks; +12 owner, +52 target gain, +41 channels,
//       +80 per-channel gain (cell x target, so pan included). Logged on change only.
//   SPLC <ms> <bank index> <id> <caller chain>
//       sub_82975700 = SPLC id -> record/container resolution (r4 bank index, r5 id).
//   POST <ms> <object index> <payload> <caller chain>
//       sub_828E2B48 = post a message to an audio object; object = (slot - 0x8302EE28) / 8.
// PLAY also logs the record's name string at +56. Caller chains are return addresses walked up
// the guest stack back-chain (saved LR at [frame - 8]), which names the gameplay function.
// The player pointer joins PLAY and GAIN lines ([SndPlayer1+12] = player).
//
// Categories: SKATE3_TRACE=audio (events), dsp (GAIN/SEND per-voice lines, very many), dspmod (MOD) and
// audiox (GRECX, FIRSTHIT, SKID, EMITSLOT: extra per-frame player and emitter detail; BAILLOCAL, BAILSTEP, BAILREG: the
// ragdoll's per-region body impacts during the local player's bails; opt-in, see the end of the file).

#include "trace_common.h"

using namespace skate3_research;

// GAIN / SEND / MOD run on the audio render thread INSIDE the command-queue consumer's locked section
// (sub_82B48530 → voice graph). A global mutex there delayed the drain enough for the game's unbounded
// "set parameter" queue to overflow (crashes in sub_824C9058; see hooks_queue.cpp). Their last-value
// tables are therefore thread_local (only that thread calls them):
// no lock, no contention with the game thread.
namespace {
thread_local std::unordered_map<uint32_t, float> g_last_gain;  // Gain module -> last logged target
thread_local std::unordered_map<uint32_t, float> g_last_send;  // Send object -> last logged target
}  // namespace

extern "C" REX_FUNC(sub_82B32DC8) {
  if (On("audio")) {
    const uint32_t record = ctx.r3.u32;
    if (Plausible(record)) {
      const uint32_t snd_player = LoadU32(base, record + 4);
      const uint32_t player = Plausible(snd_player) ? LoadU32(base, snd_player + 12) : 0;
      const uint32_t sample = LoadU32(base, record + 32);
      char hex[97] = "-";
      if (Plausible(sample)) HexAt(base, sample, hex);
      rex::audio_trace::line("PLAY", "0x%08X\t0x%08X\t0x%08X\t%.4f\t%u\t%s", player, snd_player, sample,
                             LoadF32(base, record + 48), static_cast<unsigned>(base[record + 46]), hex);
    }
  }
  __imp__sub_82B32DC8(ctx, base);
}

extern "C" REX_FUNC(sub_82B23B50) {
  if (On("dsp")) {
    const uint32_t module = ctx.r3.u32;
    if (Plausible(module)) {
      const float target = LoadF32(base, module + 52);
      bool changed;
      {
        auto [it, inserted] = g_last_gain.try_emplace(module, target);
        changed = inserted || std::fabs(it->second - target) > 0.01f;
        if (changed) it->second = target;
      }
      if (changed) {
        rex::audio_trace::line("GAIN", "0x%08X\t0x%08X\t%.4f\t%.4f", LoadU32(base, module + 12), module,
                               target, LoadF32(base, module + 56));
      }
    }
  }
  __imp__sub_82B23B50(ctx, base);
}

extern "C" REX_FUNC(sub_82B31838) {
  if (On("dsp")) {
    const uint32_t object = ctx.r3.u32;
    if (Plausible(object)) {
      const float target = LoadF32(base, object + 52);
      bool changed;
      {
        auto [it, inserted] = g_last_send.try_emplace(object, target);
        changed = inserted || std::fabs(it->second - target) > 0.01f;
        if (changed) it->second = target;
      }
      if (changed) {
        const unsigned channels = base[object + 41] < 8 ? base[object + 41] : 8;
        char cells[96] = "";
        int used = 0;
        for (unsigned c = 0; c < channels && used < 80; ++c)
          used += std::snprintf(cells + used, sizeof(cells) - used, "%s%.3f", c ? "," : "",
                                LoadF32(base, object + 80 + 4 * c));
        rex::audio_trace::line("SEND", "0x%08X\t0x%08X\t%.4f\t%u\t%s", LoadU32(base, object + 12), object,
                               target, channels, cells);
      }
    }
  }
  __imp__sub_82B31838(ctx, base);
}

extern "C" REX_FUNC(sub_82975700) {
  if (On("audio")) {
    char chain[64];
    CallerChain(ctx, base, chain);
    rex::audio_trace::line("SPLC", "%d\t%d\t%s", static_cast<int>(ctx.r4.s8), ctx.r5.s32, chain);
  }
  __imp__sub_82975700(ctx, base);
}

// Board contact sounds (SFXObj_Contacts). sub_824B86E0 = per-frame contact handler: r3 = the contact
// object; [r3+32] = the skater/board state it reads: +208 impact speed (f32, scaled into AEMS param
// 5), +448..+460 per-wheel impact class (i32, the max over newly touching wheels = set variant),
// +620 state id, +716 / +814 flags. Logged only when a wheel class is non-zero (a contact happened).
//   CONTACT <ms> <object> <speed208> <class0> <class1> <class2> <class3> <state620> <f716> <f814>
// sub_824BA3F0 = contact set picker (r4 kind, r5 variant -> r3 SPLC id); sub_824BA310 = its tier.
//   CSET <ms> <kind> <variant> <id> <tier>
namespace {
thread_local int g_last_tier = -1;
}

extern "C" REX_FUNC(sub_824B86E0) {
  if (On("audio")) {
    const uint32_t object = ctx.r3.u32;
    const uint32_t state = Plausible(object) ? LoadU32(base, object + 32) : 0;
    if (Plausible(state)) {
      const uint32_t c0 = LoadU32(base, state + 448), c1 = LoadU32(base, state + 452);
      const uint32_t c2 = LoadU32(base, state + 456), c3 = LoadU32(base, state + 460);
      if (c0 | c1 | c2 | c3) {
        rex::audio_trace::line("CONTACT", "0x%08X\t%.4f\t%d\t%d\t%d\t%d\t%d\t%u\t%u", object,
                               LoadF32(base, state + 208), static_cast<int>(c0), static_cast<int>(c1),
                               static_cast<int>(c2), static_cast<int>(c3),
                               static_cast<int>(LoadU32(base, state + 620)), base[state + 716],
                               base[state + 814]);
      }
    }
  }
  __imp__sub_824B86E0(ctx, base);
}

extern "C" REX_FUNC(sub_824BA310) {
  __imp__sub_824BA310(ctx, base);
  g_last_tier = static_cast<int>(ctx.r3.s32);
}

extern "C" REX_FUNC(sub_824BA3F0) {
  const int kind = ctx.r4.s32, variant = ctx.r5.s32;
  g_last_tier = -1;
  __imp__sub_824BA3F0(ctx, base);
  if (On("audio")) {
    rex::audio_trace::line("CSET", "%d\t%d\t%d\t%d", kind, variant, ctx.r3.s32, g_last_tier);
  }
}

// Per-voice DSP modules (EA voice graph; offsets from upstream PR #4's notes). Change-only per module.
//   MOD <ms> <kind> <owner> <module> <v1> <v2> <v3>   (category dspmod, NOT in dsp: ~1,700 lines/s on the
//   audio thread; with it on, the game's audio command queue overflowed and crashed, 2026-10-02)
//   PITCH sub_82B2DAC8: v1 requested rate (+64), v2 scale (+52), v3 ratio (+56)
//   LPF sub_82B27E20 / HPF sub_82B26568: v1 cutoff input (+52)
//   SHELF sub_82B26740: v1 corner (+52), v2 gain (+60)
//   PEAK sub_82B2C658: v1 frequency (+52), v2 gain (+60), v3 quality (+68)
namespace {
thread_local std::unordered_map<uint32_t, uint64_t> g_mod_last;  // module -> hash of last logged values (audio thread only)

void LogModule(const char* kind, uint8_t* base, uint32_t module, uint32_t o1, uint32_t o2, uint32_t o3) {
  if (!On("dspmod") || !Plausible(module)) return;  // own category: very chatty on the audio thread
  const float v1 = o1 ? LoadF32(base, module + o1) : 0.0f;
  const float v2 = o2 ? LoadF32(base, module + o2) : 0.0f;
  const float v3 = o3 ? LoadF32(base, module + o3) : 0.0f;
  // Quantise so tiny drifts don't flood the log.
  const auto q = [](float v) { return static_cast<uint64_t>(static_cast<int64_t>(v * 100.0f) & 0x1FFFFF); };
  const uint64_t key = q(v1) | (q(v2) << 21) | (q(v3) << 42);
  {
    auto [it, inserted] = g_mod_last.try_emplace(module, key);
    if (!inserted && it->second == key) return;
    it->second = key;
  }
  rex::audio_trace::line("MOD", "%s\t0x%08X\t0x%08X\t%.4f\t%.4f\t%.4f", kind, LoadU32(base, module + 12), module,
                         v1, v2, v3);
}
}  // namespace

extern "C" REX_FUNC(sub_82B2DAC8) {
  LogModule("PITCH", base, ctx.r3.u32, 64, 52, 56);
  __imp__sub_82B2DAC8(ctx, base);
}
extern "C" REX_FUNC(sub_82B27E20) {
  LogModule("LPF", base, ctx.r3.u32, 52, 0, 0);
  __imp__sub_82B27E20(ctx, base);
}
extern "C" REX_FUNC(sub_82B26568) {
  LogModule("HPF", base, ctx.r3.u32, 52, 0, 0);
  __imp__sub_82B26568(ctx, base);
}
extern "C" REX_FUNC(sub_82B26740) {
  LogModule("SHELF", base, ctx.r3.u32, 52, 60, 0);
  __imp__sub_82B26740(ctx, base);
}
extern "C" REX_FUNC(sub_82B2C658) {
  LogModule("PEAK", base, ctx.r3.u32, 52, 60, 68);
  __imp__sub_82B2C658(ctx, base);
}

// Audio-state bridge (sub_824B0DA8) — after it runs, log the player-0 audio state's jump velocity
// (+468, f32) whenever it changes, with air (+332 u8) and wheel count (+200 u32). Record base per
// PR #1 notes: *(0x82083C38) + 0x2F070 + 240 (+544 × player).
//   ASTATE <ms> <jv468> <air332> <wheels200>
namespace {
float g_last_jv = -12345.0f;
}
extern "C" REX_FUNC(sub_824B0DA8) {
  // r3 = the audio-state object the bridge fills (+468 jump velocity, +332 air, +200 wheels);
  // its source record is *(0x83083C38) + 0x2F070 + 544*player + 240 (PhysOut bundle).
  const uint32_t state = ctx.r3.u32;
  __imp__sub_824B0DA8(ctx, base);
  if (On("audio") && Plausible(state)) {
    const float jv = LoadF32(base, state + 468);
    if (jv != g_last_jv) {
      g_last_jv = jv;
      rex::audio_trace::line("ASTATE", "%.4f	%u	%u", jv, static_cast<unsigned>(base[state + 332]),
                             LoadU32(base, state + 200));
    }
  }
}

extern "C" REX_FUNC(sub_828E2B48) {
  if (On("audio")) {
    char chain[64];
    CallerChain(ctx, base, chain);
    const int object = static_cast<int>(ctx.r3.u32 - 0x8302EE28u) / 8;
    rex::audio_trace::line("POST", "%d\t0x%08X\t%s", object, ctx.r4.u32, chain);
  }
  __imp__sub_828E2B48(ctx, base);
}

// Random distant one-shots (sirens, dogs, bangs) — EmitterSystem at audio+488.
//   WPPOS  <ms> <listener x y z> <set key> <mode892> <state1096>   (sub_828EA918 query; every 500 ms
//          and whenever the key changes. Key = u64 at *(0x83083C38)+0x2F0B0, read by getter
//          sub_824A1560; mode = *(*(0x830CFDC4)+892); state = *(*(0x830CFDE4)+1096).)
//   WPSET  <ms> <set key>                  (sub_828EA4E0 SetRandomSet, r4)
//   WPINT  <ms> <seconds>                  (sub_824A1980 return f1: the next trigger interval)
//   WPFIRE <ms> <sound key> <caller chain> (sub_824A1A20 one-shot post, sound key = u64 at r4)
namespace {
uint64_t g_last_wp_key = ~0ull;
uint64_t g_last_wp_ms = 0;
uint64_t WpKey(uint8_t* base) {
  const uint32_t root = LoadU32(base, 0x83083C38u);
  return Plausible(root) ? LoadU64(base, root + 0x2F0B0u) : 0;
}
uint32_t WpWord(uint8_t* base, uint32_t global, uint32_t offset) {
  const uint32_t object = LoadU32(base, global);
  return Plausible(object) ? LoadU32(base, object + offset) : 0xFFFFFFFFu;
}
}  // namespace
extern "C" REX_FUNC(sub_828EA918) {
  if (On("audio") && Plausible(ctx.r4.u32)) {
    const uint64_t key = WpKey(base);
    const uint64_t now = GetTickCount64();
    if (key != g_last_wp_key || now - g_last_wp_ms >= 500) {
      g_last_wp_key = key;
      g_last_wp_ms = now;
      const uint32_t at = ctx.r4.u32;
      rex::audio_trace::line("WPPOS", "%.2f %.2f %.2f\t%016llX\t%u\t%u", LoadF32(base, at), LoadF32(base, at + 4),
                             LoadF32(base, at + 8), static_cast<unsigned long long>(key),
                             WpWord(base, 0x830CFDC4u, 892), WpWord(base, 0x830CFDE4u, 1096));
    }
  }
  __imp__sub_828EA918(ctx, base);
}
extern "C" REX_FUNC(sub_828EA4E0) {
  if (On("audio")) {
    rex::audio_trace::line("WPSET", "%016llX", static_cast<unsigned long long>(ctx.r4.u64));
  }
  __imp__sub_828EA4E0(ctx, base);
}
extern "C" REX_FUNC(sub_824A1980) {
  __imp__sub_824A1980(ctx, base);
  if (On("audio")) {
    rex::audio_trace::line("WPINT", "%.3f", ctx.f1.f64);
  }
}
// Zone ambience (SFXObj_Ambience): state machine, bed starts, crossfade posts.
//   AMBST  <ms> <zone key> <state> <t68> <t72> <dt>   (sub_824D3C28 update; obj+48 key, +64 state
//          0 silent / 3 fade-in / 1 steady / 2 fade-out, +68/+72 timers; logged when key or state change)
//   AMBBED <ms> <bed index>                            (sub_824D3590 stream start, r4)
//   AMBXF  <ms> <group> <level>                        (sub_824D0F38 c_main_ambience_crossfade post: r4
//          group, obj+116 level float)
namespace {
uint64_t g_last_amb_key = ~0ull;
uint32_t g_last_amb_state = ~0u;
}  // namespace
extern "C" REX_FUNC(sub_824D3C28) {
  const uint32_t obj = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  __imp__sub_824D3C28(ctx, base);
  if (On("audio") && Readable(base, obj + 48, 28)) {
    const uint64_t key = LoadU64(base, obj + 48);
    const uint32_t state = LoadU32(base, obj + 64);
    if (key != g_last_amb_key || state != g_last_amb_state) {
      g_last_amb_key = key;
      g_last_amb_state = state;
      rex::audio_trace::line("AMBST", "%016llX\t%u\t%.3f\t%.3f\t%.4f", static_cast<unsigned long long>(key), state,
                             LoadF32(base, obj + 68), LoadF32(base, obj + 72), dt);
    }
  }
}
extern "C" REX_FUNC(sub_824D3590) {
  if (On("audio")) {
    rex::audio_trace::line("AMBBED", "%d", static_cast<int>(ctx.r4.s32));
  }
  __imp__sub_824D3590(ctx, base);
}
extern "C" REX_FUNC(sub_824D0F38) {
  if (On("audio")) {
    const uint32_t obj = ctx.r3.u32;
    rex::audio_trace::line("AMBXF", "%d\t%.3f", static_cast<int>(ctx.r4.s32),
                           Readable(base, obj + 116, 4) ? LoadF32(base, obj + 116) : -1.0f);
  }
  __imp__sub_824D0F38(ctx, base);
}

extern "C" REX_FUNC(sub_824A1A20) {
  if (On("audio")) {
    char chain[64];
    CallerChain(ctx, base, chain);
    const uint32_t entry = ctx.r4.u32;
    rex::audio_trace::line("WPFIRE", "%016llX\t%s",
                           static_cast<unsigned long long>(Plausible(entry) ? LoadU64(base, entry) : 0), chain);
  }
  __imp__sub_824A1A20(ctx, base);
}

// GREC (2026-10-02, spec from the audio port): the granular rolling bed's per-frame state, to settle the
// bed gain on a straight roll (gain A / (1 - max(I, Bk)) = level(1)) and the carve / manual layer questions.
// SFXObj_SkateBoard update sub_824C6BD8 (r3 = owner), logged after the call for the local player only
// (byte [[owner+28]+72] != 0, local72; before 2026-10-03 the word at [[owner+16]+72], which also let an NPC skater's
// board through), once per call (per frame). Category `audio`.
//   GREC <ms> owner | truck0: A gain pitch pos active, B gain pitch pos active, running | truck1: same |
//        owner +1160 turn intensity, +1164, +1168 brake slew, +1508 downhill, +1456, +1464, +1028, +1032,
//        +1152, +1156 | state [owner+36]: +204 turn input, +208 ground speed, +200 wheels, +332 air, +336 brake,
//        +340 balance (hex words), +620 material of wheel 0
namespace {
// The local player's audio state ([owner+36] of the SkateBoard object GREC accepts), published for TREAT /
// SEAMPAT / SEAMHIT as a second local-player test. Plain atomic, no lock.
std::atomic<uint32_t> g_local_audio_state{0};
// Every state GREC accepted, with its owner (two owners can pass the local test), for COLLPOST's owner field.
std::atomic<uint64_t> g_grec_states[4];  // owner << 32 | state
void NoteGrecState(uint32_t owner, uint32_t st) {
  const uint64_t v = (static_cast<uint64_t>(owner) << 32) | st;
  for (std::atomic<uint64_t>& s : g_grec_states) {
    uint64_t cur = s.load(std::memory_order_relaxed);
    if (cur == v) return;
    if (cur == 0 && s.compare_exchange_strong(cur, v, std::memory_order_relaxed)) return;
  }
  static std::atomic<uint32_t> next{0};  // all slots used (states change on reloads): replace the oldest
  g_grec_states[next.fetch_add(1, std::memory_order_relaxed) % 4].store(v, std::memory_order_relaxed);
}
// Local-rider test (2026-10-03): the byte at [[object+28]+72], as COLLPOST's local72 and as the game's own eqchain,
// grain-chain and trick code read it. The first hooks read the WORD at [[object+16]+72] instead, which also passed
// for an NPC skater's objects once one spawned, so GREC / GRECX / FIRSTHIT / SKID / TREAT / SEAMPAT / SEAMHIT
// alternated between two owners. -1 when the byte is not readable.
int LocalByte(uint8_t* base, uint32_t obj, uint32_t off) {
  const uint32_t ref = TryU32(base, obj + off, 0);
  return Readable(base, ref, 73) ? static_cast<int>(base[ref + 72]) : -1;
}
// LOCALTEST (category `audio`, at most 256 lines): per hook and object, the inputs of the old and the new local
// test whenever the pair (old test passed, new test passed) changes, so a trace shows every owner a hook rejected
// (an NPC skater) or accepted, and when.
//   LOCALTEST <ms> <hook> <object> <byte [[obj+16]+72]> <byte [[obj+28]+72]> <old word [[obj+16]+72] != 0> <accepted>
//   6 fields.
struct LocalTestSlot {
  std::atomic<uint64_t> key{0};  // tag << 32 | object
  std::atomic<uint32_t> code{0};  // 1 + old * 2 + accepted
};
LocalTestSlot g_localtest[64];
std::atomic<int> g_localtest_lines{0};
void NoteLocalTest(const char* hook, uint32_t tag, uint8_t* base, uint32_t obj, int b28) {
  const uint64_t v = (static_cast<uint64_t>(tag) << 32) | obj;
  const uint32_t ctl = TryU32(base, obj + 16, 0);
  const int old_word = Plausible(ctl) && TryU32(base, ctl + 72, 0) != 0 ? 1 : 0;
  const int accepted = b28 > 0 ? 1 : 0;
  const uint32_t code = 1u + static_cast<uint32_t>(old_word * 2 + accepted);
  for (LocalTestSlot& s : g_localtest) {
    uint64_t cur = s.key.load(std::memory_order_relaxed);
    if (cur == 0 && !s.key.compare_exchange_strong(cur, v, std::memory_order_relaxed) && cur != v) continue;
    if (cur != 0 && cur != v) continue;
    if (s.code.exchange(code, std::memory_order_relaxed) == code) return;
    if (g_localtest_lines.fetch_add(1, std::memory_order_relaxed) < 256)
      rex::audio_trace::line("LOCALTEST", "%s\t%08X\t%d\t%d\t%d\t%d", hook, obj, LocalByte(base, obj, 16), b28, old_word,
                             accepted);
    return;
  }
}
uint32_t GrecOwnerOf(uint32_t st) {
  if (st == 0) return 0;
  for (std::atomic<uint64_t>& s : g_grec_states) {
    const uint64_t v = s.load(std::memory_order_relaxed);
    if (v != 0 && static_cast<uint32_t>(v) == st) return static_cast<uint32_t>(v >> 32);
  }
  return 0;
}
void GrainPlayerText(uint8_t* base, uint32_t player, char* out, size_t n) {
  if (!Readable(base, player, 148)) {
    std::snprintf(out, n, "- - - -");
    return;
  }
  std::snprintf(out, n, "%.4f %.4f %.4f %u", LoadF32(base, player), LoadF32(base, player + 4),
                LoadF32(base, player + 12), static_cast<unsigned>(base[player + 144]));
}
}  // namespace
// GRECX (2026-10-03, category `audiox`, opt-in): extra per-frame state from the same hook and the same
// local-player test, as a separate kind so GREC's layout stays unchanged. 7 fields:
//   GRECX <ms> <owner> <state +332 word, hex> <+232 slip f32> <+690 revert u8> <owner +1516 revert counter>
//         <listener camera x y z> <listener view direction x y z>
//   The +332 word holds the bytes +332 air, +333 / +334 push plant, +335 plant edge. The listener record is
//   *(0x830CFDD4) (filled by sub_8248CC08): +0 camera position, +32 view direction. "nan nan nan" or -1
//   when a read is not possible.
namespace {
void GrecExtra(uint8_t* base, uint32_t owner, uint32_t st) {
  char cam[48], view[48];
  const uint32_t listener = TryU32(base, 0x830CFDD4u, 0);
  Vec3Text(base, listener, cam, sizeof cam);
  Vec3Text(base, listener + 32, view, sizeof view);
  const bool ok = Readable(base, st, 694);
  rex::audio_trace::line("GRECX", "%08X\t%08X\t%.4f\t%d\t%d\t%s\t%s", owner, ok ? LoadU32(base, st + 332) : 0u,
                         ok ? LoadF32(base, st + 232) : NAN, ok ? static_cast<int>(base[st + 690]) : -1,
                         static_cast<int>(TryU32(base, owner + 1516, 0xFFFFFFFFu)), cam, view);
}
// FIRSTHIT (2026-10-03, category `audiox`, opt-in): the bail "first hit" gate, sampled once per GREC call (per
// frame, local player). 6 fields:
//   FIRSTHIT <ms> <B> <B+16 u8> <B+24 f32> <state +676 bail u8> <state +677 end u8> <why>
//   B = *(*(0x83083C38) + 0x2FCB4) (lis -31992 / 15416, offset 0x2FCB4 as in sub_824BC188). The Contacts object
//   latches the rising edge of B+16 (sub_824BCEB0: Contacts input 7 pulse, torso post); input 8 = B+24 x 32767.
//   State = the GREC state [owner+36]. Logged when the byte, the float, +676 or +677 change, and as a 1 s
//   heartbeat. why = bit mask: 1 byte, 2 float, 4 bail / end bytes, 8 heartbeat. B 0 / -1 / nan / -1 when a
//   read is not possible.
struct FirstHitLast {
  uint32_t b = 0xFFFFFFFFu;
  int b16 = -2;
  uint32_t f24 = 0xFFFFFFFFu;  // raw bits, so NaN compares stable
  int bail = -2, end = -2;
  uint64_t ms = 0;
};
// Two GREC owners pass the local test (seen 2026-10-03: the board objects 40C33020 / 40C34020, each called once per
// frame), so the last values are kept per state; a single set alternated the bail / end bytes on every call. The
// local72 gate (LocalByte) now lets only the local rider through; the per-state slots stay for reloads.
struct FirstHitSlot {
  uint32_t st = 0;
  FirstHitLast last;
};
thread_local FirstHitSlot g_firsthit[4];
std::atomic<uint64_t> g_grec_bail_active{0};  // GetTickCount64 when a GREC-local state last had +676 or +677 set
void FirstHit(uint8_t* base, uint32_t st) {
  const uint32_t root = TryU32(base, 0x83083C38u, 0);
  const uint32_t b = Plausible(root) ? TryU32(base, root + 0x2FCB4u, 0) : 0;
  const bool b_ok = Readable(base, b, 28);
  const int b16 = b_ok ? static_cast<int>(base[b + 16]) : -1;
  const uint32_t f24 = b_ok ? LoadU32(base, b + 24) : 0x7FC00000u;
  const bool st_ok = Readable(base, st, 678);
  const int bail = st_ok ? static_cast<int>(base[st + 676]) : -1;
  const int end = st_ok ? static_cast<int>(base[st + 677]) : -1;
  const uint64_t now = GetTickCount64();
  if (bail > 0 || end > 0) g_grec_bail_active.store(now, std::memory_order_relaxed);
  FirstHitSlot* slot = nullptr;
  for (FirstHitSlot& s : g_firsthit)
    if (s.st == st) { slot = &s; break; }
  if (!slot) {
    slot = &g_firsthit[0];
    for (FirstHitSlot& s : g_firsthit)
      if (s.last.ms < slot->last.ms) slot = &s;  // empty or least recently logged
    *slot = FirstHitSlot{st, {}};
  }
  FirstHitLast& last = slot->last;
  unsigned why = 0;
  if (b16 != last.b16 || b != last.b) why |= 1;
  if (f24 != last.f24) why |= 2;
  if (bail != last.bail || end != last.end) why |= 4;
  if (now - last.ms >= 1000) why |= 8;
  if (why == 0) return;
  last = {b, b16, f24, bail, end, now};
  float f;
  std::memcpy(&f, &f24, 4);
  rex::audio_trace::line("FIRSTHIT", "%08X\t%d\t%.4f\t%d\t%d\t%u", b, b16, f, bail, end, why);
}
}  // namespace
extern "C" REX_FUNC(sub_824C6BD8) {
  const uint32_t owner = ctx.r3.u32;
  __imp__sub_824C6BD8(ctx, base);
  const bool grec = On("audio"), grecx = On("audiox");
  if ((!grec && !grecx) || !Readable(base, owner, 1512)) return;
  const int b28 = LocalByte(base, owner, 28);
  if (grec) NoteLocalTest("GREC", 1, base, owner, b28);
  if (b28 <= 0) return;  // local rider only (local72)
  const uint32_t st = TryU32(base, owner + 36, 0);
  if (Plausible(st)) {
    g_local_audio_state.store(st, std::memory_order_relaxed);
    NoteGrecState(owner, st);
  }
  if (grecx) {
    GrecExtra(base, owner, st);
    FirstHit(base, st);
  }
  if (!grec) return;
  char t[2][160];
  for (int i = 0; i < 2; ++i) {
    char a[64], b[64];
    GrainPlayerText(base, LoadU32(base, owner + 1176 + 8 * i), a, sizeof a);
    GrainPlayerText(base, LoadU32(base, owner + 1180 + 8 * i), b, sizeof b);
    std::snprintf(t[i], sizeof t[i], "%s | %s | %u", a, b, static_cast<unsigned>(base[owner + 1328 + i]));
  }
  char s[160] = "-";
  if (Readable(base, st, 624)) {
    // +332 air is a byte (as in ASTATE); +336 brake / +340 balance widths unconfirmed -> whole words in hex.
    std::snprintf(s, sizeof s, "%.4f %.4f %u %u %08X %08X %u", LoadF32(base, st + 204), LoadF32(base, st + 208),
                  LoadU32(base, st + 200), static_cast<unsigned>(base[st + 332]), LoadU32(base, st + 336),
                  LoadU32(base, st + 340), LoadU32(base, st + 620));
  }
  rex::audio_trace::line("GREC", "%08X\t%s\t%s\t%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\t%s", owner, t[0], t[1],
                         LoadF32(base, owner + 1160), LoadF32(base, owner + 1164), LoadF32(base, owner + 1168),
                         LoadF32(base, owner + 1508), LoadF32(base, owner + 1456), LoadF32(base, owner + 1464),
                         LoadF32(base, owner + 1028), LoadF32(base, owner + 1032), LoadF32(base, owner + 1152),
                         LoadF32(base, owner + 1156), s);
}

// TREAT / SEAMPAT / SEAMHIT (2026-10-02, specs from the audio port). Logged after the call for the local
// player only, category `audio`. Each line ends with `<object> <local>`: local bit 4 = byte [[object+28]+72] != 0
// (local72, the gate since 2026-10-03), bit 1 = byte [[object+16]+72] != 0, bit 2 = the object's state is the one
// GREC's local SkateBoard uses. Logged only when bit 4 is set. Before 2026-10-03 the lines were logged when the WORD
// at [[object+16]+72] was non-zero (old bit 1) or the state matched (old bit 2): both passed for an NPC skater's
// objects, so older traces need an object filter. Cheap and lock-free: a few guarded reads and one line per call.
//   TREAT <ms> <+236 f32> <+240 f32> <+260 f32> <+224 u8> <+332 u8> <+200 u32> <object> <local>
//       Class_Treatment update sub_824DD6F0 (r3 = object), state [object+32]: air time, predicted time to
//       landing, jump height, the +224 byte, in the air, wheels on the ground. 8 fields.
//   SEAMPAT <ms> <+636 u32> <+648 u32> <+620 u32> <+208 f32> <frame ms> <wheel 0 xyz> <wheel 1 xyz>
//           <wheel 2 xyz> <wheel 3 xyz> <object> <local>
//       Class_Seams process sub_824C14C8 (r3 = object), state [object+32]: seam pattern of wheels 0 and 3,
//       material of wheel 0, speed, host time since the previous local SEAMPAT line (the rendered-frame
//       time; 0 on the first), wheel positions +384 + 16 w (three floats, space separated). 11 fields.
//   SEAMHIT <ms> <wheel r4> <single r5 u8> <transition r6> <object> <local>
//       Class_Seams hit sub_824C1DF8 (r3 = object). 5 fields.
namespace {
// Logged only when bit 4 (local72) is set; bits 1 and 2 stay as information.
uint32_t LocalMask(uint8_t* base, uint32_t obj, uint32_t st, const char* hook, uint32_t tag) {
  uint32_t mask = 0;
  const int b28 = LocalByte(base, obj, 28);
  NoteLocalTest(hook, tag, base, obj, b28);
  if (LocalByte(base, obj, 16) > 0) mask |= 1;
  if (st != 0 && st == g_local_audio_state.load(std::memory_order_relaxed)) mask |= 2;
  if (b28 > 0) mask |= 4;
  return (mask & 4) ? mask : 0;
}
std::atomic<int64_t> g_seampat_last_qpc{0};
double SeamFrameMs() {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<int64_t>(f.QuadPart);
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const int64_t last = g_seampat_last_qpc.exchange(now.QuadPart, std::memory_order_relaxed);
  return last == 0 ? 0.0 : static_cast<double>(now.QuadPart - last) * 1000.0 / static_cast<double>(freq);
}
}  // namespace
extern "C" REX_FUNC(sub_824DD6F0) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_824DD6F0(ctx, base);
  if (!On("audio") || !Readable(base, obj, 36)) return;
  const uint32_t st = LoadU32(base, obj + 32);
  if (!Readable(base, st, 336)) return;
  const uint32_t mask = LocalMask(base, obj, st, "TREAT", 2);
  if (mask == 0) return;
  rex::audio_trace::line("TREAT", "%.4f\t%.4f\t%.4f\t%u\t%u\t%u\t%08X\t%u", LoadF32(base, st + 236),
                         LoadF32(base, st + 240), LoadF32(base, st + 260), static_cast<unsigned>(base[st + 224]),
                         static_cast<unsigned>(base[st + 332]), LoadU32(base, st + 200), obj, mask);
}
extern "C" REX_FUNC(sub_824C14C8) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_824C14C8(ctx, base);
  if (!On("audio") || !Readable(base, obj, 36)) return;
  const uint32_t st = LoadU32(base, obj + 32);
  if (!Readable(base, st, 652)) return;
  const uint32_t mask = LocalMask(base, obj, st, "SEAMPAT", 3);
  if (mask == 0) return;
  char w[4][48];
  for (int i = 0; i < 4; ++i) Vec3Text(base, st + 384 + 16 * i, w[i], sizeof w[i]);
  rex::audio_trace::line("SEAMPAT", "%u\t%u\t%u\t%.4f\t%.3f\t%s\t%s\t%s\t%s\t%08X\t%u", LoadU32(base, st + 636),
                         LoadU32(base, st + 648), LoadU32(base, st + 620), LoadF32(base, st + 208), SeamFrameMs(),
                         w[0], w[1], w[2], w[3], obj, mask);
}
extern "C" REX_FUNC(sub_824C1DF8) {
  const uint32_t obj = ctx.r3.u32;
  const int wheel = ctx.r4.s32;
  const unsigned single = ctx.r5.u32 & 0xFF;
  const int transition = ctx.r6.s32;
  if (On("audio") && Readable(base, obj, 36)) {
    const uint32_t mask = LocalMask(base, obj, LoadU32(base, obj + 32), "SEAMHIT", 4);
    if (mask != 0) rex::audio_trace::line("SEAMHIT", "%d\t%u\t%d\t%08X\t%u", wheel, single, transition, obj, mask);
  }
  __imp__sub_824C1DF8(ctx, base);
}

// SKID / EMITSLOT (2026-10-03, category `audiox`, opt-in). Logged after the call; a few guarded reads and one
// formatted line per call, lock-free (both functions run on the game thread; their small memories are
// thread_local).
//   SKID <ms> <owner> <holder> <handle> <w0..w17> <+1516 revert counter> <state +232 slip f32> <state +690 u8>
//       Class_wheels_skid updater sub_824C7A20 (r3 = the SFXObj_SkateBoard owner, local player only as GREC).
//       Holder = owner +1288 (+0 sound handle, +4.. the 18 packet words, space separated, signed). Logged
//       every call while a holder exists, plus one line when it goes away (holder 0, words "-"). 7 fields.
//   EMITSLOT <ms> <object> <state> <info +0> <info +4> <info +8 level f32> <info +12> <info +16> <mixmap key>
//            <handle> <w0..w8> <out0..out9>
//       SFXObj_Emitter per-frame update sub_824DCF08 (r3 = object), for emitters whose state ([object+28])
//       is active (+52 byte) on entry; at most one line per object per 100 ms. Info = tEmitterInfo
//       [object+32] (+0 patch index, +4 != 0 = positional branch, +8 level, +12 == 1 = fixed pan, +16 pan).
//       Mixmap key = [[object+12]+4] (the controller's key, 0x40060000 + g * 0x800). Handle and w0..w8 =
//       the c_emitter packet at [object+40] (+0 handle, words at +4..+36: w1 = slot +8 dry, w2 = +12 send,
//       w3 = +16 pan, w4 = +20 pitch, w5 = +24 low-pass). out0..out9 = the controller's raw output halves
//       ([[object+12]+12], id n = half (n & 1 ? high : low) of word n / 2), 4 hex digits each. 11 fields.
namespace {
// Last holder per owner (an owner's object can change on reloads).
struct SkidLast {
  uint32_t owner = 0, holder = 0;
};
thread_local SkidLast g_skid_last[4];
uint32_t& SkidLastHolder(uint32_t owner) {
  for (SkidLast& s : g_skid_last)
    if (s.owner == owner) return s.holder;
  for (SkidLast& s : g_skid_last)
    if (s.owner == 0 || s.holder == 0) {
      s = {owner, 0};
      return s.holder;
    }
  g_skid_last[0] = {owner, 0};
  return g_skid_last[0].holder;
}
}  // namespace
extern "C" REX_FUNC(sub_824C7A20) {
  const uint32_t owner = ctx.r3.u32;
  __imp__sub_824C7A20(ctx, base);
  if (!On("audiox") || !Readable(base, owner, 1520)) return;
  if (LocalByte(base, owner, 28) <= 0) return;  // local rider only (local72, as GREC)
  const uint32_t holder = LoadU32(base, owner + 1288);
  uint32_t& last_holder = SkidLastHolder(owner);
  if (holder == 0 && last_holder == 0) return;
  last_holder = holder;
  char words[18 * 12 + 2] = "-";
  uint32_t handle = 0;
  if (holder != 0 && Readable(base, holder, 76)) {
    handle = LoadU32(base, holder);
    int used = 0;
    for (int i = 0; i < 18; ++i)
      used += std::snprintf(words + used, sizeof words - used, "%s%d", i ? " " : "",
                            static_cast<int>(LoadU32(base, holder + 4 + 4 * i)));
  }
  const uint32_t st = TryU32(base, owner + 36, 0);
  const bool ok = Readable(base, st, 694);
  rex::audio_trace::line("SKID", "%08X\t%08X\t%08X\t%s\t%d\t%.4f\t%d", owner, holder, handle, words,
                         static_cast<int>(LoadU32(base, owner + 1516)), ok ? LoadF32(base, st + 232) : NAN,
                         ok ? static_cast<int>(base[st + 690]) : -1);
}

namespace {
struct EmitSeen {
  uint32_t object = 0;
  uint64_t ms = 0;
};
thread_local EmitSeen g_emit_seen[16];
// True when `object` may log now (first sighting, or 100 ms since its last line); records the time.
bool EmitDue(uint32_t object, uint64_t now) {
  EmitSeen* oldest = &g_emit_seen[0];
  for (EmitSeen& e : g_emit_seen) {
    if (e.object == object) {
      if (now - e.ms < 100) return false;
      e.ms = now;
      return true;
    }
    if (e.ms < oldest->ms) oldest = &e;
  }
  oldest->object = object;
  oldest->ms = now;
  return true;
}
}  // namespace
extern "C" REX_FUNC(sub_824DCF08) {
  const uint32_t obj = ctx.r3.u32;
  bool active = false;
  if (On("audiox") && Readable(base, obj, 48)) {
    const uint32_t state = LoadU32(base, obj + 28);
    active = Readable(base, state, 53) && base[state + 52] != 0;
  }
  __imp__sub_824DCF08(ctx, base);
  if (!active || !Readable(base, obj, 48) || !EmitDue(obj, GetTickCount64())) return;
  const uint32_t info = LoadU32(base, obj + 32);
  const bool info_ok = Readable(base, info, 20);
  const uint32_t mix = LoadU32(base, obj + 12);
  const uint32_t key = TryU32(base, mix + 4, 0);
  const uint32_t block = TryU32(base, mix + 12, 0);
  const uint32_t holder = LoadU32(base, obj + 40);
  char words[9 * 12 + 2] = "-";
  uint32_t handle = 0;
  if (Readable(base, holder, 40)) {
    handle = LoadU32(base, holder);
    int used = 0;
    for (int i = 0; i < 9; ++i)
      used += std::snprintf(words + used, sizeof words - used, "%s%d", i ? " " : "",
                            static_cast<int>(LoadU32(base, holder + 4 + 4 * i)));
  }
  char outs[10 * 5 + 2] = "-";
  if (Readable(base, block, 20)) {
    int used = 0;
    for (int id = 0; id < 10; ++id) {
      const uint32_t w = LoadU32(base, block + 4 * (id >> 1));
      used += std::snprintf(outs + used, sizeof outs - used, "%s%04X", id ? " " : "",
                            static_cast<unsigned>((w >> ((id & 1) * 16)) & 0xFFFF));
    }
  }
  rex::audio_trace::line("EMITSLOT", "%08X\t%08X\t%d\t%d\t%.4f\t%d\t%d\t%08X\t%08X\t%s\t%s", obj,
                         LoadU32(base, obj + 28), info_ok ? static_cast<int>(LoadU32(base, info)) : -1,
                         info_ok ? static_cast<int>(LoadU32(base, info + 4)) : -1,
                         info_ok ? LoadF32(base, info + 8) : NAN, info_ok ? static_cast<int>(LoadU32(base, info + 12)) : -1,
                         info_ok ? static_cast<int>(LoadU32(base, info + 16)) : -1, key, handle, words, outs);
}

// BAILLOCAL / BAILSTEP / BAILREG (2026-10-03, category `audiox`, opt-in): the ragdoll's per-region body impacts during
// a bail of the local player, with their inputs, to compare the per-step velocity change against a reimplementation.
// Logged only while a bail is active (and 500 ms after it ends), only for the local player's physics output block, so
// a quiet session adds nothing. Active = the entry's bail / end bits, or +676 / +677 of a GREC-local audio state
// (FIRSTHIT's read; needs that hook, i.e. category `audiox`).
//   Local player: the skater-entry update sub_827A1B78 (r4 = character, r5 = skater entry, r6 = controlled flag) runs
//   per skater from the loop in sub_827A11B0 (return address 0x827A13FC). The entries it gets are a stack array of that
//   loop (its r1 + 1064 - 152, stride 544), not the global table *(0x83083C38) + 0x2F070 (the first build checked the
//   global table, so nothing matched). After the call, entry +152 bit 31 = r6 marks the controlled skater and entry
//   +148 bits 5 / 4 are the bail / end bits. Physics output block X = [[character + 1808] + 1800]: vfunc +92 of that
//   object (sub_82DB1680) returns +1800; the vtable slot is checked at run time (how 1 = checked, 2 = slot differs).
//   BAILLOCAL <ms> <character> <object +1808> <X> <how> <entry index in the loop>
//       when the local X or how changes (at most 64 lines). Expected once at spawn. 5 fields.
//   BAILCAND <ms> <src> <lr> <a> <b> <flag> <c> <d>: the lookup's raw inputs, so a failed lookup is visible. 7 fields.
//       src `entry`: the first 32 calls of sub_827A1B78: lr, character, entry, bit 31, object +1808, X.
//       src `pass`: each new X seen by sub_82BD60C8 (up to 8): lr, SC, X, 0, 0, X.
//   Per physics output pass sub_82BD60C8 (r3 = SkeletonCollision SC, r4 = X; Collision = [X+24]; SkeletonState SS =
//   [SC+4016]; called once per sub_82BE1AE8 pass), logged after the call for the local X:
//   BAILSTEP <ms> <X> <step ms> <SS +5220 dt> <bail> <end> <regions with a part> <SC +4036> <SC +4040> <cfg +164> <how>
//       step ms = host time since the previous pass for the local X (the real update interval; SS velocities are
//       finite differences x 60 in sub_82BEBD28, constant 0x822F860C). cfg = [[*(0x830CFDA4)+268]+4] (+164 = the x10
//       impact scale). SC +4036 / +4040 = the two accumulators the region loop adds to. 10 fields.
//   BAILREG <ms> <X> <region i 0..7> <part> <impact> <|dv.n|> <v_old.n> <v_new.n> <|dv|> <normal x y z> <mass> <slide>
//           <tag>
//       one line per region with a contact part (SC +1200 + 4i != -1): impact = Collision +80 + 4i (what the game wrote:
//       clamp01(max(0.001, |dv.n| x mass x cfg+164))); dv = SS +4048 + 16 part (the part's velocity change this pass);
//       n = SC +1008 + 16i (region normal); v_new = SS +3632 + 16 part, v_old = v_new - dv (both signed along n);
//       mass = SS +4560 + 4 part; slide = Collision +112 + 4i; tag = Collision +144 + 4i (hex). 12 fields.
//   A pass's BAILREG lines come just before its BAILSTEP line. "nan" when a read is not possible.
namespace {
std::atomic<uint32_t> g_bail_local_x{0};
std::atomic<uint32_t> g_bail_local_how{0};
std::atomic<uint32_t> g_bail_bits{0};          // entry +148 bits 5 (bail) and 4 (end), as bail * 2 + end
std::atomic<uint64_t> g_bail_last_active{0};   // GetTickCount64 when the bits were last non-zero
std::atomic<int64_t> g_bail_last_qpc{0};
double BailStepMs() {
  static const int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<int64_t>(f.QuadPart);
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const int64_t last = g_bail_last_qpc.exchange(now.QuadPart, std::memory_order_relaxed);
  return last == 0 ? 0.0 : static_cast<double>(now.QuadPart - last) * 1000.0 / static_cast<double>(freq);
}
}  // namespace
namespace {
// BAILCAND: rate-limited raw view of the lookup's inputs, so a failed local-player lookup is visible in a trace.
std::atomic<int> g_bailcand_entry{0};
std::atomic<int> g_bailcand_pass{0};
std::atomic<uint32_t> g_bailcand_seen[8];  // distinct X values seen by the physics output pass
}  // namespace
extern "C" REX_FUNC(sub_827A1B78) {
  const uint32_t character = ctx.r4.u32;
  const uint32_t entry = ctx.r5.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t caller_r1 = ctx.r1.u32;
  __imp__sub_827A1B78(ctx, base);
  if (!On("audiox")) return;
  // Both callers pass a stack copy of the entry, never the global table: the per-skater loop in sub_827A11B0
  // (return 0x827A13FC; entries at its r1 + 1064 - 152, stride 544; r6 = the "controlled skater" test) and a
  // single-entry caller sub_827A1030 (return 0x827A10A0; r1 + 272, r6 = 1 always). Only the loop counts.
  const uint32_t w152 = TryU32(base, entry + 152, 0);
  const uint32_t flag = w152 >> 31;
  const uint32_t object = TryU32(base, character + 1808, 0);
  const uint32_t vtable = Plausible(object) ? TryU32(base, object, 0) : 0;
  const uint32_t how = Plausible(vtable) && TryU32(base, vtable + 92, 0) == 0x82DB1680u ? 1u : 2u;
  const uint32_t x = Plausible(object) ? TryU32(base, object + 1800, 0) : 0;
  if (g_bailcand_entry.load(std::memory_order_relaxed) < 32 &&
      g_bailcand_entry.fetch_add(1, std::memory_order_relaxed) < 32)
    rex::audio_trace::line("BAILCAND", "entry\t%08X\t%08X\t%08X\t%u\t%08X\t%08X", lr, character, entry, flag, object, x);
  if (lr != 0x827A13FCu || flag == 0) return;  // the per-skater loop, controlled skater only
  const uint32_t first = caller_r1 + 1064u - 152u;
  const uint32_t index = entry >= first && (entry - first) % 544u == 0 ? (entry - first) / 544u : 0xFFu;
  const uint32_t w148 = TryU32(base, entry + 148, 0);
  const uint32_t bits = ((w148 >> 5) & 1u) * 2u + ((w148 >> 4) & 1u);
  g_bail_bits.store(bits, std::memory_order_relaxed);
  if (bits != 0) g_bail_last_active.store(GetTickCount64(), std::memory_order_relaxed);
  if (!Plausible(x)) return;
  const uint32_t old_x = g_bail_local_x.exchange(x, std::memory_order_relaxed);
  const uint32_t old_how = g_bail_local_how.exchange(how, std::memory_order_relaxed);
  static std::atomic<int> lines{0};
  if ((old_x != x || old_how != how) && lines.fetch_add(1, std::memory_order_relaxed) < 64)
    rex::audio_trace::line("BAILLOCAL", "%08X\t%08X\t%08X\t%u\t%u", character, object, x, how, index);
}
extern "C" REX_FUNC(sub_82BD60C8) {
  const uint32_t sc = ctx.r3.u32;
  const uint32_t x = ctx.r4.u32;
  __imp__sub_82BD60C8(ctx, base);
  if (!On("audiox")) return;
  if (g_bailcand_pass.load(std::memory_order_relaxed) < 8) {
    for (std::atomic<uint32_t>& seen : g_bailcand_seen) {
      uint32_t expected = 0;
      if (seen.load(std::memory_order_relaxed) == x) break;
      if (seen.compare_exchange_strong(expected, x, std::memory_order_relaxed)) {
        g_bailcand_pass.fetch_add(1, std::memory_order_relaxed);
        rex::audio_trace::line("BAILCAND", "pass\t%08X\t%08X\t%08X\t0\t00000000\t%08X", static_cast<uint32_t>(ctx.lr),
                               sc, x, x);
        break;
      }
    }
  }
  if (x == 0 || x != g_bail_local_x.load(std::memory_order_relaxed)) return;
  const double step_ms = BailStepMs();
  const uint32_t bits = g_bail_bits.load(std::memory_order_relaxed);
  const uint64_t now = GetTickCount64();
  if (bits == 0 && now - g_bail_last_active.load(std::memory_order_relaxed) > 500 &&
      now - g_grec_bail_active.load(std::memory_order_relaxed) > 500)
    return;
  if (!Readable(base, sc, 4044)) return;
  const uint32_t ss = LoadU32(base, sc + 4016);
  const uint32_t coll = TryU32(base, x + 24, 0);
  const bool ss_ok = Readable(base, ss, 5224);
  const bool coll_ok = Readable(base, coll, 176);
  const uint32_t holder = TryU32(base, 0x830CFDA4u, 0);
  const uint32_t cfg = Plausible(holder) ? TryU32(base, TryU32(base, holder + 268, 0) + 4, 0) : 0;
  int contacts = 0;
  for (int i = 0; i < 8; ++i) {
    const int part = static_cast<int>(LoadU32(base, sc + 1200 + 4 * i));
    if (part < 0 || part > 25) continue;
    ++contacts;
    char normal[48];
    Vec3Text(base, sc + 1008 + 16 * i, normal, sizeof normal);
    const float nx = LoadF32(base, sc + 1008 + 16 * i), ny = LoadF32(base, sc + 1012 + 16 * i),
                nz = LoadF32(base, sc + 1016 + 16 * i);
    float dvn = NAN, vold = NAN, vnew = NAN, dvl = NAN, mass = NAN;
    if (ss_ok) {
      const uint32_t dv = ss + 4048 + 16 * part, v = ss + 3632 + 16 * part;
      const float dx = LoadF32(base, dv), dy = LoadF32(base, dv + 4), dz = LoadF32(base, dv + 8);
      const float d = dx * nx + dy * ny + dz * nz;
      vnew = LoadF32(base, v) * nx + LoadF32(base, v + 4) * ny + LoadF32(base, v + 8) * nz;
      vold = vnew - d;
      dvn = std::fabs(d);
      dvl = std::sqrt(dx * dx + dy * dy + dz * dz);
      mass = LoadF32(base, ss + 4560 + 4 * part);
    }
    rex::audio_trace::line("BAILREG", "%08X\t%d\t%d\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%s\t%.6f\t%.4f\t%08X", x, i, part,
                           coll_ok ? LoadF32(base, coll + 80 + 4 * i) : NAN, dvn, vold, vnew, dvl, normal, mass,
                           coll_ok ? LoadF32(base, coll + 112 + 4 * i) : NAN,
                           coll_ok ? LoadU32(base, coll + 144 + 4 * i) : 0u);
  }
  rex::audio_trace::line("BAILSTEP", "%08X\t%.3f\t%.5f\t%u\t%u\t%d\t%.4f\t%.4f\t%.4f\t%u", x, step_ms,
                         ss_ok ? LoadF32(base, ss + 5220) : NAN, bits >> 1, bits & 1u, contacts, LoadF32(base, sc + 4036),
                         LoadF32(base, sc + 4040), Plausible(cfg) ? TryF32(base, cfg + 164) : NAN,
                         g_bail_local_how.load(std::memory_order_relaxed));
}

// BANDQ (2026-10-03, category `audiox`, opt-in): every impact-band query sub_82497088 (r3 material, f1 impact, r5 / r6
// outputs = the band's low / high edge, r7 flag; returns the tier 0 / 1 / 2, or 3 under the floor). Logged after the call,
// so the band edges the game really used can be read per material and compared with an exported band table:
// tier 2 -> (b0, b4), tier 1 -> (b8, b0), tier 0 -> (b12, b8); tier 3 leaves the outputs untouched.
//   BANDQ <ms> <caller> <material> <impact> <flag> <tier> <low> <high>
//       caller = the return address (e.g. 0x824BC4D4 / 0x824BC534: the body poster's material A / B). 7 fields.
extern "C" REX_FUNC(sub_82497088) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const int material = ctx.r3.s32;
  const float impact = static_cast<float>(ctx.f1.f64);
  const uint32_t out_low = ctx.r5.u32, out_high = ctx.r6.u32;
  const unsigned flag = ctx.r7.u32 & 0xFF;
  __imp__sub_82497088(ctx, base);
  if (!On("audiox")) return;
  const int tier = ctx.r3.s32;
  const bool edges = tier >= 0 && tier <= 2;
  rex::audio_trace::line("BANDQ", "%08X\t%d\t%.5f\t%u\t%d\t%.5f\t%.5f", lr, material, impact, flag, tier,
                         edges ? TryF32(base, out_low) : NAN, edges ? TryF32(base, out_high) : NAN);
}

// COLLPOST (2026-10-03, category `audiox`, opt-in): every message posted to the collision sound manager,
// sub_82486EF0, which copies its arguments into a 48-byte message: +0 r4 material A, +4 r5 material B, +8 r6 tier A,
// +12 r7 tier B, +16 the vector at r8 (contact position), +32 r9 / +36 r10 (the sound picked for A against B and for B
// against A by sub_82496C58 in the body poster; 0 = none), +40 / +41 / +42 three bytes from the caller's stack
// (caller r1 + 87 / 95 / 103; the body poster passes +42 = [[Contacts+28]+72]). Logged before the call.
//   COLLPOST <ms> <caller> <object> <state> <grec owner> <local72> <mat A> <mat B> <tier A> <tier B> <sound A> <sound B>
//            <b40> <b41> <b42> <pos x y z> <caller chain>
//       caller = the posting function, from the return address: 82496258, 824BA630, 824BB0E0, 824BC188 (body
//       poster), 824BCEB0 (first-hit torso), 824BD000 (deck impact), 824E3BE0 (physics prop); 0 = other. object = that
//       function's `this` (the register it keeps r3 in). state = [object+32]; grec owner = the GREC board object whose
//       state [owner+36] equals it (0 = none, i.e. not a GREC-local player); local72 = [[object+28]+72] (u8, -1 when not
//       readable). 16 fields.
namespace {
struct PostCaller {
  uint32_t lo, hi, fn;
  int reg;
};
constexpr PostCaller kPostCallers[] = {
    {0x82496258u, 0x82496420u, 0x82496258u, 30}, {0x824BA630u, 0x824BB0E0u, 0x824BA630u, 23},
    {0x824BB0E0u, 0x824BB330u, 0x824BB0E0u, 25}, {0x824BC188u, 0x824BCBA0u, 0x824BC188u, 31},
    {0x824BCEB0u, 0x824BD000u, 0x824BCEB0u, 31}, {0x824BD000u, 0x824BD358u, 0x824BD000u, 20},
    {0x824E3BE0u, 0x824E3FA0u, 0x824E3BE0u, 31},
};
uint32_t Gpr(const PPCContext& ctx, int reg) {
  switch (reg) {
    case 20: return ctx.r20.u32;
    case 23: return ctx.r23.u32;
    case 25: return ctx.r25.u32;
    case 30: return ctx.r30.u32;
    case 31: return ctx.r31.u32;
    default: return 0;
  }
}
}  // namespace
extern "C" REX_FUNC(sub_82486EF0) {
  if (On("audiox")) {
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    uint32_t fn = 0, object = 0;
    for (const PostCaller& c : kPostCallers)
      if (lr > c.lo && lr < c.hi) {
        fn = c.fn;
        object = Gpr(ctx, c.reg);
        break;
      }
    const uint32_t state = Plausible(object) ? TryU32(base, object + 32, 0) : 0;
    const uint32_t comp = Plausible(object) ? TryU32(base, object + 28, 0) : 0;
    const int local72 = Readable(base, comp, 73) ? static_cast<int>(base[comp + 72]) : -1;
    const uint32_t sp = ctx.r1.u32;
    const bool sp_ok = Readable(base, sp + 87, 17);
    char pos[48];
    Vec3Text(base, ctx.r8.u32 & ~0xFu, pos, sizeof pos);
    char chain[64];
    CallerChain(ctx, base, chain);
    rex::audio_trace::line("COLLPOST", "%08X\t%08X\t%08X\t%08X\t%d\t%d\t%d\t%d\t%d\t%08X\t%08X\t%d\t%d\t%d\t%s\t%s", fn,
                           object, state, GrecOwnerOf(state), local72, static_cast<int>(ctx.r4.u32),
                           static_cast<int>(ctx.r5.u32), static_cast<int>(ctx.r6.u32), static_cast<int>(ctx.r7.u32),
                           ctx.r9.u32, ctx.r10.u32, sp_ok ? static_cast<int>(base[sp + 87]) : -1,
                           sp_ok ? static_cast<int>(base[sp + 95]) : -1, sp_ok ? static_cast<int>(base[sp + 103]) : -1,
                           pos, chain);
  }
  __imp__sub_82486EF0(ctx, base);
}
