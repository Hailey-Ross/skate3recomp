// Research hooks (tracing tools; not intended for the upstream project): physics probes (ground jump, board
// contacts, constraint solver, solver iteration count ITERSET/ITERTICK). Category: SKATE3_TRACE=physics (src/research/trace_common.h).

#include "trace_common.h"

using namespace skate3_research;

// Ground-jump probe (rolling-ollie jump velocity investigation). p = processed physics input;
// the player's p was 46953EE0 in every Aletown run so far (other p values are NPC skaters).
//   GJCALC <ms> p | out(x y z) active | flags2468 2484 2488 | v400 | v704 | n576 | p480 | up544 |
//          com592 | strength2624 ctl2628 ctl2632 g2648 speed2656 | t1420 t1552   (82D93618)
//   GJF    <ms> p | flags2468 2488 | v400 | com592 | up544 | w720 | speed2656 timer2664 wheels2556
//          (every GroundAnimation fill 82D34150; v400 = deck velocity the frame started with)
//   GJDECK <ms> p | toolkit deck up | forward | position (board+304 toolkit +80/+96/+112)
//   GJFILL <ms> p flags2468 | jump48 | v400 | delta | |delta| launched80
namespace {
void Vec3(uint8_t* base, uint32_t a, char* out, size_t n) {
  std::snprintf(out, n, "%.4f %.4f %.4f", LoadF32(base, a), LoadF32(base, a + 4), LoadF32(base, a + 8));
}
}  // namespace
extern "C" REX_FUNC(sub_82D93618) {
  const uint32_t out = ctx.r3.u32;
  const uint32_t tk = ctx.r4.u32;
  const uint32_t p = ctx.r5.u32;
  __imp__sub_82D93618(ctx, base);
  if (On("physics") && Plausible(out) && Plausible(p) && Plausible(tk) &&
      base[out + 20] != 0) {
    char o[64], v400[64], v704[64], n576[64], p480[64], up[64], com[64];
    Vec3(base, out, o, sizeof o);
    Vec3(base, p + 400, v400, sizeof v400);
    Vec3(base, p + 704, v704, sizeof v704);
    Vec3(base, p + 576, n576, sizeof n576);
    Vec3(base, p + 480, p480, sizeof p480);
    Vec3(base, p + 544, up, sizeof up);
    Vec3(base, p + 592, com, sizeof com);
    rex::audio_trace::line("GJCALC", "%08X\t"
                           "%s\t%u\t%08X\t%08X\t%08X\t%s\t%s\t%s\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f",
                           p, o, static_cast<unsigned>(base[out + 20]), LoadU32(base, p + 2468),
                           LoadU32(base, p + 2484), LoadU32(base, p + 2488), v400, v704, n576, p480, up, com,
                           LoadF32(base, p + 2624), LoadF32(base, p + 2628), LoadF32(base, p + 2632),
                           LoadF32(base, p + 2648), LoadF32(base, p + 2656), LoadF32(base, tk + 1420),
                           LoadF32(base, tk + 1552));
  }
}
// NPC skaters (category `aiskater`, separate from `physics`): every skater runs this GroundAnimation fill
// (the player is one of them; NPC skaters are the other p values). Logged per skater at most every 250 ms:
//   SKATER <ms> p | board position x y z (toolkit +112) | velocity v400 | flags2468 | flags2488 |
//          speed2656 | wheels2556
// The skater list itself (index 0 = player, 1.. = NPC skaters) is `*(*(ctx + 1588) + 8)` (vtable +4 count, +12 get),
// see sub_82C74EB8 (GetDistanceToNearestAISkater); the ctx comes from the Lua registry, so it isn't read here.
namespace {
std::mutex g_skater_mutex;
std::unordered_map<uint32_t, uint64_t> g_skater_last;
}  // namespace
static void LogSkater(uint8_t* base, uint32_t self) {
  const uint32_t p = TryU32(base, self + 16, 0);
  if (!Readable(base, p, 2672)) return;
  {
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_skater_mutex);
    uint64_t& last = g_skater_last[p];
    if (now - last < 250) return;
    last = now;
  }
  const uint32_t board = TryU32(base, self + 4, 0);
  const uint32_t tk = board ? TryU32(base, board + 304, 0) : 0;
  char pos[64], vel[64];
  Vec3Text(base, tk ? tk + 112 : 0, pos, sizeof pos);
  Vec3Text(base, p + 400, vel, sizeof vel);
  rex::audio_trace::line("SKATER", "%08X\t%s\t%s\t%08X\t%08X\t%.3f\t%u", p, pos, vel, LoadU32(base, p + 2468),
                         LoadU32(base, p + 2488), LoadF32(base, p + 2656), LoadU32(base, p + 2556));
}
extern "C" REX_FUNC(sub_82D34150) {
  const uint32_t self = ctx.r3.u32;
  if (On("aiskater") && Plausible(self)) LogSkater(base, self);
  if (On("physics") && Plausible(self)) {
    const uint32_t p = LoadU32(base, self + 16);
    if (Plausible(p)) {
      char c[64], v[64], u[64], w[64];
      Vec3(base, p + 592, c, sizeof c);
      Vec3(base, p + 400, v, sizeof v);
      Vec3(base, p + 544, u, sizeof u);
      Vec3(base, p + 720, w, sizeof w);
      rex::audio_trace::line("GJF", "%08X\t%08X\t%08X\t%s\t%s\t%s\t%s\t%.4f\t%.4f\t%u", p, LoadU32(base, p + 2468),
                             LoadU32(base, p + 2488), v, c, u, w, LoadF32(base, p + 2656),
                             LoadF32(base, p + 2664), static_cast<unsigned>(LoadU32(base, p + 2556)));
      const uint32_t board = LoadU32(base, self + 4);
      const uint32_t tk = Plausible(board) ? LoadU32(base, board + 304) : 0;
      if (Plausible(tk)) {
        char du[64], df[64], dp[64];
        Vec3(base, tk + 80, du, sizeof du);
        Vec3(base, tk + 96, df, sizeof df);
        Vec3(base, tk + 112, dp, sizeof dp);
        rex::audio_trace::line("GJDECK", "%08X\t%s\t%s\t%s", p, du, df, dp);
      }
    }
    if (Plausible(p) && (LoadU32(base, p + 2468) & 0x00400000u) != 0) {
      char j[64], v[64];
      Vec3(base, self + 48, j, sizeof j);
      Vec3(base, p + 400, v, sizeof v);
      const float dx = LoadF32(base, self + 48) - LoadF32(base, p + 400);
      const float dy = LoadF32(base, self + 52) - LoadF32(base, p + 404);
      const float dz = LoadF32(base, self + 56) - LoadF32(base, p + 408);
      rex::audio_trace::line("GJFILL", "%08X\t%08X\t%s\t%s\t%.4f %.4f %.4f\t%.4f\t%u", p, LoadU32(base, p + 2468), j, v,
                             dx, dy, dz, std::sqrt(dx * dx + dy * dy + dz * dz),
                             static_cast<unsigned>(base[self + 80]));
    }
  }
  __imp__sub_82D34150(ctx, base);
}

// Board post-physics contact pass (82C07D20, r3 = skateboard). After it: per-part contact byte
// +844+i, part normal +96+16i, point +208+16i, sampled part velocity +320+16i (i: 0..3 wheels,
// 4 front truck, 5 back truck, 6 deck).
//   BRD <ms> board contacts(7) | v4 | v5 | v6 | deck n | deck pt | back-wheel-2 pt | v2 | v3 | v0
//   (the player's board was 469434F0 in the Aletown runs)
// Category `aiskater`: every skater's board (player and NPC skaters) runs this pass each frame. Logged per board at
// most every 250 ms:
//   SKATEB <ms> board | contacts w0 w1 w2 w3 front-truck back-truck deck (1 = touching) | contact point x y z
//          (of the first touching part, wheels first; 0 0 0 in the air) | deck velocity x y z | part index (0-6,
//          -1 in the air)
// (trucks touching without wheels = grind; deck = slide; no contacts = air)
namespace {
std::unordered_map<uint32_t, uint64_t> g_board_last;
}  // namespace
extern "C" REX_FUNC(sub_82C07D20) {
  const uint32_t b = ctx.r3.u32;
  __imp__sub_82C07D20(ctx, base);
  if (On("aiskater") && Plausible(b)) {
    bool log = false;
    {
      const uint64_t now = GetTickCount64();
      std::lock_guard<std::mutex> lock(g_skater_mutex);
      uint64_t& last = g_board_last[b];
      if (now - last >= 250) {
        last = now;
        log = true;
      }
    }
    if (log && Readable(base, b, 852)) {  // guarded read only for lines that are logged
      // Position: the contact point of the first touching part (wheels 0-3, trucks 4-5, deck 6); each part's
      // point is only valid while that part touches (the deck's alone was 0 for 96 % of NPC samples).
      int part = -1;
      for (int i = 0; i < 7; ++i) {
        if (base[b + 844 + i] != 0) {
          part = i;
          break;
        }
      }
      char pt[64], v6[64];
      if (part >= 0) {
        Vec3Text(base, b + 208 + 16 * part, pt, sizeof pt);
      } else {
        std::snprintf(pt, sizeof pt, "0 0 0");
      }
      Vec3Text(base, b + 320 + 16 * 6, v6, sizeof v6);
      rex::audio_trace::line("SKATEB", "%08X\t%u%u%u%u%u%u%u\t%s\t%s\t%d", b, base[b + 844] != 0, base[b + 845] != 0,
                             base[b + 846] != 0, base[b + 847] != 0, base[b + 848] != 0, base[b + 849] != 0,
                             base[b + 850] != 0, pt, v6, part);
    }
  }
  if (On("physics") && Plausible(b)) {
    char v4[64], v5[64], v6[64], n6[64], p6[64], p5[64];
    Vec3(base, b + 320 + 16 * 4, v4, sizeof v4);
    Vec3(base, b + 320 + 16 * 5, v5, sizeof v5);
    Vec3(base, b + 320 + 16 * 6, v6, sizeof v6);
    Vec3(base, b + 96 + 16 * 6, n6, sizeof n6);
    Vec3(base, b + 208 + 16 * 6, p6, sizeof p6);
    Vec3(base, b + 208 + 16 * 2, p5, sizeof p5);
    char v2[64], v3[64], v0[64];
    Vec3(base, b + 320 + 16 * 2, v2, sizeof v2);
    Vec3(base, b + 320 + 16 * 3, v3, sizeof v3);
    Vec3(base, b + 320, v0, sizeof v0);
    rex::audio_trace::line("BRD", "%08X\t%u%u%u%u%u%u%u\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s", b, base[b + 844],
                           base[b + 845], base[b + 846], base[b + 847], base[b + 848], base[b + 849], base[b + 850],
                           v4, v5, v6, n6, p6, p5, v2, v3, v0);
  }
}

// Constraint solve stage (82AE30F0: r3 = rw::physics::Simulation, r4 = partition). The loop count is
// Simulation+176; also +100/+108/+116 = contact/joint/drive counts. Logged once per distinct
// (partition, iterations) pair.
//   SOLV30F0 <ms> sim partition iterations contacts joints drives
extern "C" REX_FUNC(sub_82AE30F0) {
  static std::mutex s_mutex;
  static uint32_t s_seen[64][2];
  static int s_count = 0;
  if (On("physics") && Plausible(ctx.r3.u32)) {
    const uint32_t p = ctx.r3.u32;
    const uint32_t part = ctx.r4.u32, iterations = LoadU32(base, p + 176);
    std::lock_guard<std::mutex> lock(s_mutex);
    bool seen = false;
    for (int i = 0; i < s_count; ++i) seen = seen || (s_seen[i][0] == part && s_seen[i][1] == iterations);
    if (!seen && s_count < 64) {
      s_seen[s_count][0] = part;
      s_seen[s_count][1] = iterations;
      ++s_count;
      rex::audio_trace::line("SOLV30F0", "%08X\t%u\t%u\t%u\t%u\t%u", p, part, iterations, LoadU32(base, p + 100),
                             LoadU32(base, p + 108), LoadU32(base, p + 116));
    }
  }
  __imp__sub_82AE30F0(ctx, base);
}

// Simulation setup (82DC2840: r3 = owner, r5 = config; Simulation = *(owner)). Logs the config's
// iteration count (+16) and Simulation+176 right after setup.
extern "C" REX_FUNC(sub_82DC2840) {
  const uint32_t owner = ctx.r3.u32;
  const uint32_t cfg = ctx.r5.u32;
  const uint32_t cfg16 = Plausible(cfg) ? LoadU32(base, cfg + 16) : 0;
  __imp__sub_82DC2840(ctx, base);
  if (On("physics") && Plausible(owner)) {
    const uint32_t sim = LoadU32(base, owner);
    rex::audio_trace::line("SIMSETUP", "%08X\tcfg16=%u\tsim=%08X\tsim176=%u", owner, cfg16, sim,
                           Plausible(sim) ? LoadU32(base, sim + 176) : 0);
  }
}

// Solver iteration count writer (82763E00: r3 = owner; Simulation = *(*(owner + 12))). It stores 50 into
// Simulation+176 when the owner's mode word (+8) is 3, else 25. Called from the frame update 82859E70 for
// two owners (lr 8285A1E8 = slot 0 = *(*(game + 188) + 8), lr 8285A1F8 = slot 1 = *(... + 12)). Logged per
// owner when (mode, count) changes, plus a 1 Hz heartbeat; `calls` = calls of that owner since its last line.
//   ITERSET <ms> owner slot mode sim before after calls why(1 change, 2 heartbeat, 4 first) chain
// The frame update itself, once a second: number of calls by flag bits r4 & 3 and the game byte +145.
//   ITERTICK <ms> game calls f0 f1 f2 f3 b145
namespace {
struct IterOwner {
  uint32_t owner, mode, after, calls;
  uint64_t last;
};
std::mutex g_iter_mutex;
IterOwner g_iter[8];
int g_iter_count = 0;
}  // namespace
extern "C" REX_FUNC(sub_82763E00) {
  if (!On("physics") || !Plausible(ctx.r3.u32)) {
    __imp__sub_82763E00(ctx, base);
    return;
  }
  const uint32_t owner = ctx.r3.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const int slot = lr == 0x8285A1E8u ? 0 : lr == 0x8285A1F8u ? 1 : -1;
  char chain[64];
  CallerChain(ctx, base, chain);
  const uint32_t mode = TryU32(base, owner + 8);
  const uint32_t holder = TryU32(base, owner + 12, 0);
  const uint32_t sim = Plausible(holder) ? TryU32(base, holder, 0) : 0;
  const uint32_t before = Plausible(sim) ? TryU32(base, sim + 176) : 0xFFFFFFFFu;
  __imp__sub_82763E00(ctx, base);
  const uint32_t after = Plausible(sim) ? TryU32(base, sim + 176) : 0xFFFFFFFFu;
  const uint64_t now = GetTickCount64();
  uint32_t why = 0, calls = 0;
  {
    std::lock_guard<std::mutex> lock(g_iter_mutex);
    IterOwner* e = nullptr;
    for (int i = 0; i < g_iter_count; ++i)
      if (g_iter[i].owner == owner) e = &g_iter[i];
    if (!e) {
      if (g_iter_count >= 8) return;
      e = &g_iter[g_iter_count++];
      *e = {owner, mode, after, 0, now};
      why = 4;
    }
    ++e->calls;
    if (e->mode != mode || e->after != after) why |= 1;
    if (now - e->last >= 1000) why |= 2;
    if (!why) return;
    calls = e->calls;
    *e = {owner, mode, after, 0, now};
  }
  rex::audio_trace::line("ITERSET", "%08X\t%d\t%u\t%08X\t%u\t%u\t%u\t%u\t%s", owner, slot, mode, sim, before, after,
                         calls, why, chain);
}
extern "C" REX_FUNC(sub_82859E70) {
  static std::mutex s_mutex;
  static uint32_t s_calls[4] = {0, 0, 0, 0};
  static uint64_t s_last = 0;
  if (On("physics") && Plausible(ctx.r3.u32)) {
    const uint32_t game = ctx.r3.u32;
    const uint64_t now = GetTickCount64();
    uint32_t c[4] = {0, 0, 0, 0};
    bool log = false;
    {
      std::lock_guard<std::mutex> lock(s_mutex);
      ++s_calls[ctx.r4.u32 & 3];
      if (now - s_last >= 1000) {
        log = true;
        s_last = now;
        for (int i = 0; i < 4; ++i) {
          c[i] = s_calls[i];
          s_calls[i] = 0;
        }
      }
    }
    if (log) {
      const uint32_t b145 = Readable(base, game + 145, 1) ? base[game + 145] : 0xFFu;
      rex::audio_trace::line("ITERTICK", "%08X\t%u\t%u\t%u\t%u\t%u\t%u", game, c[0] + c[1] + c[2] + c[3], c[0], c[1],
                             c[2], c[3], b145);
    }
  }
  __imp__sub_82859E70(ctx, base);
}
