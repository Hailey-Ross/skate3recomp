// Research hooks (tracing tools; not intended for the upstream project): the loading screen around a teleport to a
// point that is not streamed in. Times the front-end update (fade steps, update rate, dt unit), the load screen
// manager's kind changes, the tips panel and hint picks and the game loading state.
// Category: SKATE3_TRACE=loadscreen (src/research/trace_common.h). Off = one table lookup per call.
//
// Objects: FE manager = [0x830CFE14]; load screen manager L = [FE+308] (+0 current kind, +4 requested, +8 close);
// fade F = [FE+304] (+12 alpha, +16 rate, +20 holder bits); tips object T = [[[0x830CFE1C]+8]+208]
// (+16 screen state, +232 online fetch state, +288 panel state, +196..201 shown last time, +208.. counts).
//
// Kinds (fields after ms):
//   LSFADE  <ms> event | update no | dt | alpha before | alpha after | rate | bits          (7 fields; fade update
//           825E69A0; event RATE / BITS on change, BLACK / CLEAR when alpha reaches 1 / 0, STEP each update while
//           the alpha moves, at most 4000 STEP lines per boot)
//   LSRATE  <ms> updates | dt sum | dt min | dt max | wall ms                                (5 fields; FE updates
//           counted by the fade update, one line per 5 s window while the category is on)
//   LSMGR   <ms> L kind | L requested | L close | T+16 | T+232 | T+288                      (6 fields; on change, read
//           once per FE update from the fade hook)
//   LSOPEN  <ms> lr | r3 | r4 | kind before | kind after                                     (5 fields; 825E6F30)
//   LSCLOSE <ms> lr | r3 | r4 | kind before | kind after                                     (5 fields; 825E7058)
//   LSHOLD  <ms> lr | bit | instant | alpha | bits after                                     (5 fields; 825E6B30)
//   LSREL   <ms> lr | bit | instant | alpha | bits after                                     (5 fields; 825E6BF8)
//   LSGS    <ms> lr | show | mode | instant                                                  (4 fields; 826E08A8)
//   LSSTATE <ms> event | this | ticks since enter                                            (3 fields; state fn
//           82707508, events other than 1 = tick)
//   LSHINT  <ms> lr | index (global 830CEBE0 after the pick)                                 (2 fields; 82606C98)
//   LSPANEL <ms> T | last-shown bytes 196..201 | shown-before bytes 202..207 | counts 0..5   (4 fields; 82608598)
//   LSTIPS  <ms> dt arg | T+288 | T+292 timer | T+232 | T+16                                 (5 fields; tips update
//           82605CC0, on panel state or fetch state change)
#include "trace_common.h"

#include <atomic>
#include <cstdio>
#include <mutex>

using namespace skate3_research;

namespace {

constexpr const char* kCat = "loadscreen";

uint32_t Word(uint8_t* base, uint32_t addr) { return Plausible(addr) ? TryU32(base, addr) : 0xFFFFFFFFu; }
float Float(uint8_t* base, uint32_t addr) { return Plausible(addr) ? TryF32(base, addr) : NAN; }
uint32_t Byte(uint8_t* base, uint32_t addr) {
  return (Plausible(addr) && Readable(base, addr, 1)) ? base[addr] : 255u;
}

uint32_t FeManager(uint8_t* base) { return Word(base, 0x830CFE14u); }
uint32_t TipsObject(uint8_t* base) {
  const uint32_t screens = Word(base, 0x830CFE1Cu);
  const uint32_t a = Word(base, screens + 8);
  return Word(base, a + 208);
}

std::mutex g_mutex;  // FE update and state calls are rare (tens per second); one lock keeps the state simple
uint64_t g_updates = 0;
uint32_t g_steps = 0;
float g_last_rate = 0.0f;
uint32_t g_last_bits = 0xFFFFFFFFu;
uint32_t g_mgr[6] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
uint64_t g_win_start = 0;
uint32_t g_win_updates = 0;
double g_win_sum = 0.0, g_win_min = 1e30, g_win_max = 0.0;
uint64_t g_state_enter_update = 0;
uint32_t g_tips_last288 = 0xFFFFFFFFu, g_tips_last232 = 0xFFFFFFFFu;

}  // namespace

// Fade update (r3 = F, f1 = dt from the FE update, an integer converted to float).
extern "C" REX_FUNC(sub_825E69A0) {
  if (!On(kCat)) {
    __imp__sub_825E69A0(ctx, base);
    return;
  }
  const uint32_t f = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  const float a0 = Float(base, f + 12);
  __imp__sub_825E69A0(ctx, base);
  const float a1 = Float(base, f + 12);
  const float rate = Float(base, f + 16);
  const uint32_t bits = Word(base, f + 20);

  std::lock_guard<std::mutex> lock(g_mutex);
  const uint64_t n = ++g_updates;
  if (rate != g_last_rate) {
    rex::audio_trace::line("LSFADE", "RATE\t%llu\t%.3f\t%.3f\t%.3f\t%.3f\t%08X", (unsigned long long)n, dt, a0, a1,
                           rate, bits);
    g_last_rate = rate;
  }
  if (bits != g_last_bits) {
    rex::audio_trace::line("LSFADE", "BITS\t%llu\t%.3f\t%.3f\t%.3f\t%.3f\t%08X", (unsigned long long)n, dt, a0, a1,
                           rate, bits);
    g_last_bits = bits;
  }
  if (a1 != a0) {
    const char* ev = a1 >= 1.0f ? "BLACK" : (a1 <= 0.0f ? "CLEAR" : "STEP");
    if (ev[0] != 'S' || g_steps < 4000) {
      if (ev[0] == 'S') ++g_steps;
      rex::audio_trace::line("LSFADE", "%s\t%llu\t%.3f\t%.3f\t%.3f\t%.3f\t%08X", ev, (unsigned long long)n, dt, a0,
                             a1, rate, bits);
    }
  }

  // Load screen manager and tips object state, on change.
  const uint32_t fe = FeManager(base);
  const uint32_t l = Word(base, fe + 308);
  const uint32_t t = TipsObject(base);
  const uint32_t now[6] = {Word(base, l), Word(base, l + 4), Word(base, l + 8), Word(base, t + 16),
                           Word(base, t + 232), Word(base, t + 288)};
  bool changed = false;
  for (int i = 0; i < 6; ++i) changed |= now[i] != g_mgr[i];
  if (changed) {
    rex::audio_trace::line("LSMGR", "%d\t%d\t%d\t%d\t%d\t%d", (int)now[0], (int)now[1], (int)now[2], (int)now[3],
                           (int)now[4], (int)now[5]);
    for (int i = 0; i < 6; ++i) g_mgr[i] = now[i];
  }

  // Update rate window.
  const uint64_t wall = GetTickCount64();
  if (g_win_start == 0) g_win_start = wall;
  ++g_win_updates;
  g_win_sum += dt;
  if (dt < g_win_min) g_win_min = dt;
  if (dt > g_win_max) g_win_max = dt;
  if (wall - g_win_start >= 5000) {
    rex::audio_trace::line("LSRATE", "%u\t%.1f\t%.1f\t%.1f\t%llu", g_win_updates, g_win_sum, g_win_min, g_win_max,
                           (unsigned long long)(wall - g_win_start));
    g_win_start = wall;
    g_win_updates = 0;
    g_win_sum = 0.0;
    g_win_min = 1e30;
    g_win_max = 0.0;
  }
}

// Load screen manager open / close (r3 = L, r4 = kind on open).
#define LS_OPEN_CLOSE(addr, kind)                                                                  \
  extern "C" REX_FUNC(sub_##addr) {                                                                \
    if (!On(kCat)) return __imp__sub_##addr(ctx, base);                                            \
    const uint32_t lr = static_cast<uint32_t>(ctx.lr), r3 = ctx.r3.u32, r4 = ctx.r4.u32;           \
    const uint32_t before = Word(base, r3);                                                        \
    __imp__sub_##addr(ctx, base);                                                                  \
    rex::audio_trace::line(kind, "%08X\t%08X\t%d\t%d\t%d", lr, r3, (int)r4, (int)before,           \
                           (int)Word(base, r3));                                                   \
  }

LS_OPEN_CLOSE(825E6F30, "LSOPEN")
LS_OPEN_CLOSE(825E7058, "LSCLOSE")

extern "C" REX_FUNC(sub_825E6B30) {
  if (!On(kCat)) return __imp__sub_825E6B30(ctx, base);
  const uint32_t lr = static_cast<uint32_t>(ctx.lr), f = ctx.r3.u32, bit = ctx.r4.u32, instant = ctx.r6.u32 & 255u;
  __imp__sub_825E6B30(ctx, base);
  rex::audio_trace::line("LSHOLD", "%08X\t%u\t%u\t%.3f\t%08X", lr, bit, instant, Float(base, f + 12),
                         Word(base, f + 20));
}

extern "C" REX_FUNC(sub_825E6BF8) {
  if (!On(kCat)) return __imp__sub_825E6BF8(ctx, base);
  const uint32_t lr = static_cast<uint32_t>(ctx.lr), f = ctx.r3.u32, bit = ctx.r4.u32, instant = ctx.r5.u32 & 255u;
  __imp__sub_825E6BF8(ctx, base);
  rex::audio_trace::line("LSREL", "%08X\t%u\t%u\t%.3f\t%08X", lr, bit, instant, Float(base, f + 12),
                         Word(base, f + 20));
}

extern "C" REX_FUNC(sub_826E08A8) {
  if (!On(kCat)) return __imp__sub_826E08A8(ctx, base);
  rex::audio_trace::line("LSGS", "%08X\t%u\t%u\t%u", static_cast<uint32_t>(ctx.lr), ctx.r3.u32 & 255u,
                         ctx.r4.u32, ctx.r5.u32 & 255u);
  __imp__sub_826E08A8(ctx, base);
}

extern "C" REX_FUNC(sub_82707508) {
  if (!On(kCat) || ctx.r4.u32 == 1) return __imp__sub_82707508(ctx, base);
  const uint32_t ev = ctx.r4.u32, self = ctx.r3.u32;
  uint64_t since = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (ev == 0) g_state_enter_update = g_updates;
    since = g_updates - g_state_enter_update;
  }
  rex::audio_trace::line("LSSTATE", "%u\t%08X\t%llu", ev, self, (unsigned long long)since);
  __imp__sub_82707508(ctx, base);
}

extern "C" REX_FUNC(sub_82606C98) {
  if (!On(kCat)) return __imp__sub_82606C98(ctx, base);
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82606C98(ctx, base);
  rex::audio_trace::line("LSHINT", "%08X\t%d", lr, (int)Word(base, 0x830CEBE0u));
}

extern "C" REX_FUNC(sub_82608598) {
  if (!On(kCat)) return __imp__sub_82608598(ctx, base);
  const uint32_t t = ctx.r3.u32;
  __imp__sub_82608598(ctx, base);
  char last[16], before[16], counts[80];
  std::snprintf(last, sizeof(last), "%u%u%u%u%u%u", Byte(base, t + 196), Byte(base, t + 197), Byte(base, t + 198),
                Byte(base, t + 199), Byte(base, t + 200), Byte(base, t + 201));
  std::snprintf(before, sizeof(before), "%u%u%u%u%u%u", Byte(base, t + 202), Byte(base, t + 203),
                Byte(base, t + 204), Byte(base, t + 205), Byte(base, t + 206), Byte(base, t + 207));
  std::snprintf(counts, sizeof(counts), "%d %d %d %d %d %d", (int)Word(base, t + 208), (int)Word(base, t + 212),
                (int)Word(base, t + 216), (int)Word(base, t + 220), (int)Word(base, t + 224),
                (int)Word(base, t + 228));
  rex::audio_trace::line("LSPANEL", "%08X\t%s\t%s\t%s", t, last, before, counts);
}

extern "C" REX_FUNC(sub_82605CC0) {
  if (!On(kCat)) return __imp__sub_82605CC0(ctx, base);
  const uint32_t t = ctx.r3.u32, dt = ctx.r4.u32;
  __imp__sub_82605CC0(ctx, base);
  const uint32_t s288 = Word(base, t + 288), s232 = Word(base, t + 232);
  bool log = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (s288 != g_tips_last288 || s232 != g_tips_last232) {
      g_tips_last288 = s288;
      g_tips_last232 = s232;
      log = true;
    }
  }
  if (log)
    rex::audio_trace::line("LSTIPS", "%u\t%d\t%.3f\t%d\t%d", dt, (int)s288, Float(base, t + 292), (int)s232,
                           (int)Word(base, t + 16));
}
