// Research hooks (tracing tools; not intended for the upstream project): session-marker returns and the teleport
// flow behind them — distance and hold time, the streamed-or-load decision, game state changes, front-end sound
// requests and hub messages around a teleport. Category: SKATE3_TRACE=audiox (src/research/trace_common.h).
//
//   TPMARK   <ms> lr | distance | hold s | destination x y z | +80 | flags +84 +85 +86 | callers
//            (teleport request constructor sub_825582D0; for the session marker's call (lr 828993AC) the distance
//             is the caller's stack word +96 and the hold time its f31; other callers log nan)
//   TPDEC    <ms> destination x y z | r5 | r6 | r7 r8 r9 | load | busy | callers
//            (teleport decision sub_82706F50: load = 1 when the destination needs a streaming load (the loading
//             state is entered), 0 = placed at once; busy = the state's +40 byte before the call)
//   TPSTREAM <ms> destination x y z | r4 | result | callers   (streamed-in check sub_82864C40, ≤ 10 / s)
//   TPFX     <ms> object | amount                              (VisualDirector teleport-effect handler sub_827A9C60, on change)
//   FEREQ    <ms> key | r4 | callers                           (front-end sound request sub_825DFAF0, every call)
//   GSTATE   <ms> machine | state function | r5 | r6 | callers (state change sub_826DFB30, ≤ 30 / s)
//   GEVENT   <ms> queue | event | callers                      (state event push sub_826DB648, ≤ 30 / s)
//   HUBMSG   <ms> id | r5 | callers                            (message post sub_82B938D8, ≤ 2 / s per id)
#include "trace_common.h"

using namespace skate3_research;

namespace {

struct Limit {
  std::mutex mutex;
  uint64_t second = 0;
  uint32_t lines = 0;
};

bool Allow(Limit& limit, uint32_t per_second) {
  std::lock_guard<std::mutex> lock(limit.mutex);
  const uint64_t second = GetTickCount64() / 1000;
  if (second != limit.second) {
    limit.second = second;
    limit.lines = 0;
  }
  if (limit.lines >= per_second) return false;
  ++limit.lines;
  return true;
}

void Vec(uint8_t* base, uint32_t addr, char (&out)[64]) {
  if (Readable(base, addr, 12)) {
    std::snprintf(out, sizeof(out), "%.2f %.2f %.2f", LoadF32(base, addr), LoadF32(base, addr + 4),
                  LoadF32(base, addr + 8));
  } else {
    std::snprintf(out, sizeof(out), "nan nan nan");
  }
}

uint32_t Byte(uint8_t* base, uint32_t addr) { return Readable(base, addr, 1) ? base[addr] : 255u; }

}  // namespace

extern "C" REX_FUNC(sub_825582D0) {
  if (On("audiox")) {
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    const bool marker = lr == 0x828993ACu;
    const float distance = marker ? TryF32(base, ctx.r1.u32 + 96) : NAN;
    const double hold = marker ? ctx.f31.f64 : NAN;
    char dest[64];
    Vec(base, ctx.r4.u32 + 48, dest);
    char callers[64];
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("TPMARK", "%08X\t%.3f\t%.4f\t%s\t%08X\t%u %u %u\t%s", lr, distance, hold, dest, ctx.r5.u32,
                           ctx.r6.u32 & 255u, ctx.r7.u32 & 255u, ctx.r8.u32 & 255u, callers);
  }
  __imp__sub_825582D0(ctx, base);
}

extern "C" REX_FUNC(sub_82706F50) {
  if (!On("audiox")) {
    __imp__sub_82706F50(ctx, base);
    return;
  }
  char dest[64];
  Vec(base, ctx.r4.u32 + 48, dest);
  const uint32_t r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32 & 255u, r8 = ctx.r8.u32 & 255u,
                 r9 = ctx.r9.u32 & 255u;
  const uint32_t busy = Byte(base, ctx.r3.u32 + 40);
  char callers[64];
  CallerChain(ctx, base, callers);
  __imp__sub_82706F50(ctx, base);
  rex::audio_trace::line("TPDEC", "%s\t%08X\t%08X\t%u %u %u\t%u\t%u\t%s", dest, r5, r6, r7, r8, r9, ctx.r3.u32 & 255u,
                         busy, callers);
}

extern "C" REX_FUNC(sub_82864C40) {
  static Limit limit;
  if (!On("audiox") || !Allow(limit, 10)) {
    __imp__sub_82864C40(ctx, base);
    return;
  }
  char dest[64];
  Vec(base, ctx.r3.u32 + 48, dest);
  const uint32_t r4 = ctx.r4.u32 & 255u;
  char callers[64];
  CallerChain(ctx, base, callers);
  __imp__sub_82864C40(ctx, base);
  rex::audio_trace::line("TPSTREAM", "%s\t%u\t%u\t%s", dest, r4, ctx.r3.u32 & 255u, callers);
}

extern "C" REX_FUNC(sub_827A9C60) {
  if (On("audiox")) {
    static std::atomic<uint32_t> last{0xFFFFFFFFu};
    const uint32_t bits = TryU32(base, ctx.r4.u32 + 16, 0xFFFFFFFFu);
    if (last.exchange(bits) != bits) {
      float amount;
      std::memcpy(&amount, &bits, 4);
      rex::audio_trace::line("TPFX", "%08X\t%.4f", ctx.r3.u32, amount);
    }
  }
  __imp__sub_827A9C60(ctx, base);
}

extern "C" REX_FUNC(sub_825DFAF0) {
  if (On("audiox")) {
    char callers[64];
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("FEREQ", "%016llX\t%08X\t%s", static_cast<unsigned long long>(ctx.r3.u64), ctx.r4.u32,
                           callers);
  }
  __imp__sub_825DFAF0(ctx, base);
}

extern "C" REX_FUNC(sub_826DFB30) {
  static Limit limit;
  if (On("audiox") && Allow(limit, 30)) {
    char callers[64];
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("GSTATE", "%08X\t%08X\t%08X\t%08X\t%s", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                           callers);
  }
  __imp__sub_826DFB30(ctx, base);
}

extern "C" REX_FUNC(sub_826DB648) {
  static Limit limit;
  if (On("audiox") && Allow(limit, 30)) {
    char callers[64];
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("GEVENT", "%08X\t%d\t%s", ctx.r3.u32, ctx.r4.s32, callers);
  }
  __imp__sub_826DB648(ctx, base);
}

extern "C" REX_FUNC(sub_82B938D8) {
  if (On("audiox")) {
    static std::mutex mutex;
    static std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> seen;
    const uint32_t id = TryU32(base, ctx.r3.u32, 0xFFFFFFFFu);
    bool log = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const uint64_t second = GetTickCount64() / 1000;
      auto& s = seen[id];
      if (s.first != second) s = {second, 0};
      log = s.second < 2;
      ++s.second;
    }
    if (log) {
      char callers[64];
      CallerChain(ctx, base, callers);
      rex::audio_trace::line("HUBMSG", "%08X\t%d\t%s", id, ctx.r5.s32, callers);
    }
  }
  __imp__sub_82B938D8(ctx, base);
}
