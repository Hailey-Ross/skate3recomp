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
// Categories: SKATE3_TRACE=audio (events) and dsp (GAIN/SEND/MOD per-voice module lines, very many).

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
// ([owner+16]+72 != 0), once per call (per frame). Category `audio`.
//   GREC <ms> owner | truck0: A gain pitch pos active, B gain pitch pos active, running | truck1: same |
//        owner +1160 turn intensity, +1164, +1168 brake slew, +1508 downhill, +1456, +1464, +1028, +1032,
//        +1152, +1156 | state [owner+36]: +204 turn input, +208 ground speed, +200 wheels, +332 air, +336 brake,
//        +340 balance (hex words), +620 material of wheel 0
namespace {
void GrainPlayerText(uint8_t* base, uint32_t player, char* out, size_t n) {
  if (!Readable(base, player, 148)) {
    std::snprintf(out, n, "- - - -");
    return;
  }
  std::snprintf(out, n, "%.4f %.4f %.4f %u", LoadF32(base, player), LoadF32(base, player + 4),
                LoadF32(base, player + 12), static_cast<unsigned>(base[player + 144]));
}
}  // namespace
extern "C" REX_FUNC(sub_824C6BD8) {
  const uint32_t owner = ctx.r3.u32;
  __imp__sub_824C6BD8(ctx, base);
  if (!On("audio") || !Readable(base, owner, 1512)) return;
  const uint32_t ctl = TryU32(base, owner + 16, 0);
  if (!ctl || TryU32(base, ctl + 72, 0) == 0) return;  // local player only
  char t[2][160];
  for (int i = 0; i < 2; ++i) {
    char a[64], b[64];
    GrainPlayerText(base, LoadU32(base, owner + 1176 + 8 * i), a, sizeof a);
    GrainPlayerText(base, LoadU32(base, owner + 1180 + 8 * i), b, sizeof b);
    std::snprintf(t[i], sizeof t[i], "%s | %s | %u", a, b, static_cast<unsigned>(base[owner + 1328 + i]));
  }
  const uint32_t st = TryU32(base, owner + 36, 0);
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
