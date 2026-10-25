// Research hooks (tracing tools; not intended for the upstream project): the camera SlowMotionController's
// sim-rate requests and what they do (b584). Category: SKATE3_TRACE=slowmo (src/research/trace_common.h).
//   SMBEGIN  <ms> controller | online | fps_at_scale_one | callers                       (82BB1DD8, after the call)
//   SMUPD    <ms> controller | online | fps1 | s+8 | s+0xC | s+0x10 | s+0x14 | skipped | callers  (82BB1EE0, <=20/s)
//   SRPOST   <ms> msg | timestep | count | fps | callers  (82B938D8, only message id 0x3FBF5E43; in hooks_marker.cpp,
//            which already overrides the message post)
//   SIMRATE  <ms> sim | online | ts | count | 7C0 | 7C1 | 800 | 801 | fps | svc | obj | vt | fn | callers (82857EC8)
//   SIMOBJ   <ms> obj | n | off:old>new,...   ([[0x83083BCC]+0x10] words changed across 82857EC8, first 0x200 bytes)
//   SIMRESET <ms> sim | +0x80 before | +0x7C after | flags     (8285A3C0: the countdown reached 0)
//   SIMTICK  <ms> calls | calls with flag bit 1 | +0x7C | +0x80 | sim   (8285A3C0, once a second)
#include "trace_common.h"

#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <string>

using namespace skate3_research;

namespace {
constexpr uint32_t kOnlineWord = 0x830B7C28u;  // byte 0x830B7C2B = low byte of this big-endian word
constexpr uint32_t kServices = 0x83083BCCu;
uint32_t Online(uint8_t* base) { return TryU32(base, kOnlineWord, 0xFFFFFFFFu) & 0xFFu; }

void Armed(const char* hook, PPCContext& ctx) {
  // one flag per hook name via a tiny static table
  static std::mutex m;
  static std::string seen;
  std::lock_guard<std::mutex> lock(m);
  const std::string key = std::string(",") + hook + ",";
  if (seen.find(key) != std::string::npos) return;
  seen += key;
  rex::audio_trace::line("HOOKARMED", "%s\t%08X\t%08X\t%08X\t%08X", hook, static_cast<uint32_t>(ctx.lr),
                         ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
}

float FpsAtScaleOne(uint8_t* base, uint32_t controller) {
  const uint32_t settings = TryU32(base, controller + 0x20u, 0);
  return Plausible(settings) ? TryF32(base, settings + 0x80u) : NAN;
}

FirstPassLimit g_upd_limit;
std::atomic<uint64_t> g_tick_window{0}, g_ticks{0}, g_ticks_bit1{0};
std::atomic<uint32_t> g_last_dt_bits{0};
}  // namespace

extern "C" REX_FUNC(sub_82BB1DD8) {  // SlowMotionController Begin
  const bool log = On("slowmo");
  const uint32_t controller = ctx.r3.u32;
  char callers[64] = "-";
  if (log) {
    Armed("82BB1DD8", ctx);
    CallerChain(ctx, base, callers);
  }
  __imp__sub_82BB1DD8(ctx, base);
  if (log) {
    rex::audio_trace::line("SMBEGIN", "%08X\t%u\t%.3f\t%s", controller, Online(base), FpsAtScaleOne(base, controller),
                           callers);
  }
}

extern "C" REX_FUNC(sub_82BB1EE0) {  // SlowMotionController Update
  uint32_t skipped = 0;
  const bool log = On("slowmo") && FirstPassAllow(g_upd_limit, skipped);
  const uint32_t controller = ctx.r3.u32, cx = ctx.r4.u32;
  char callers[64] = "-";
  if (log) {
    Armed("82BB1EE0", ctx);
    CallerChain(ctx, base, callers);
  }
  __imp__sub_82BB1EE0(ctx, base);
  if (log) {
    const uint32_t s = TryU32(base, cx + 8u, 0);
    const bool ok = Plausible(s) && Readable(base, s, 0x18);
    rex::audio_trace::line("SMUPD", "%08X\t%u\t%.3f\t%.4f\t%.4f\t%.4f\t%u\t%u\t%s", controller, Online(base),
                           FpsAtScaleOne(base, controller), ok ? LoadF32(base, s + 8) : NAN,
                           ok ? LoadF32(base, s + 0xC) : NAN, ok ? LoadF32(base, s + 0x10) : NAN,
                           ok ? (LoadU32(base, s + 0x14) >> 24) : 0xFFu, skipped, callers);
  }
}

extern "C" REX_FUNC(sub_82857EC8) {  // Simulation::OnRequestSetSimRate
  if (!On("slowmo")) {
    __imp__sub_82857EC8(ctx, base);
    return;
  }
  Armed("82857EC8", ctx);
  const uint32_t sim = ctx.r3.u32, msg = ctx.r4.u32;
  const float ts = TryF32(base, msg + 0x10u);
  const int32_t count = static_cast<int32_t>(TryU32(base, msg + 0x14u, 0));
  const float dt0 = TryF32(base, sim + 0x7Cu);
  const int32_t cd0 = static_cast<int32_t>(TryU32(base, sim + 0x80u, 0));
  const uint32_t svc = TryU32(base, kServices, 0);
  const uint32_t obj = Plausible(svc) ? TryU32(base, svc + 0x10u, 0) : 0;
  const uint32_t vt = Plausible(obj) ? TryU32(base, obj, 0) : 0;
  const uint32_t fn = Plausible(vt) ? TryU32(base, vt + 0x34u, 0) : 0;
  static uint8_t snap[0x200];  // the handler runs on one thread (the sim's message pump); research code
  const bool have = Plausible(obj) && Readable(base, obj, sizeof snap);
  if (have) std::memcpy(snap, base + obj, sizeof snap);
  char callers[64];
  CallerChain(ctx, base, callers);
  __imp__sub_82857EC8(ctx, base);
  const float dt1 = TryF32(base, sim + 0x7Cu);
  const int32_t cd1 = static_cast<int32_t>(TryU32(base, sim + 0x80u, 0));
  rex::audio_trace::line("SIMRATE", "%08X\t%u\t%.6f\t%d\t%.6f\t%.6f\t%d\t%d\t%d\t%08X\t%08X\t%08X\t%08X\t%s", sim,
                         Online(base), ts, count, dt0, dt1, cd0, cd1, dt1 > 0 ? static_cast<int>(1.0f / dt1 + 0.5f) : -1,
                         svc, obj, vt, fn, callers);
  if (have && Readable(base, obj, sizeof snap)) {
    std::string list;
    int n = 0;
    for (uint32_t off = 0; off < sizeof snap; off += 4) {
      uint32_t a, b;
      std::memcpy(&a, snap + off, 4);
      std::memcpy(&b, base + obj + off, 4);
      if (a == b) continue;
      if (++n > 16) continue;
      char item[48];
      std::snprintf(item, sizeof item, "%s%X:%08X>%08X", list.empty() ? "" : ",", off, __builtin_bswap32(a),
                    __builtin_bswap32(b));
      list += item;
    }
    rex::audio_trace::line("SIMOBJ", "%08X\t%d\t%s", obj, n, list.empty() ? "-" : list.c_str());
  }
}

extern "C" REX_FUNC(sub_8285A3C0) {  // Simulation tick: countdown timeout + 1 Hz tick rate
  if (!On("slowmo")) {
    __imp__sub_8285A3C0(ctx, base);
    return;
  }
  Armed("8285A3C0", ctx);
  const uint32_t sim = ctx.r3.u32, flags = ctx.r4.u32;
  const int32_t cd0 = static_cast<int32_t>(TryU32(base, sim + 0x80u, 0));
  __imp__sub_8285A3C0(ctx, base);
  const int32_t cd1 = static_cast<int32_t>(TryU32(base, sim + 0x80u, 0));
  const uint32_t dt_bits = TryU32(base, sim + 0x7Cu, 0);
  float dt1;
  std::memcpy(&dt1, &dt_bits, 4);
  if (cd0 == 1 && cd1 == 0) rex::audio_trace::line("SIMRESET", "%08X\t%d\t%.6f\t%08X", sim, cd0, dt1, flags);
  ++g_ticks;
  if (flags & 1u) ++g_ticks_bit1;
  const uint64_t now = GetTickCount64();
  uint64_t start = g_tick_window.load();
  if (start == 0) g_tick_window.compare_exchange_strong(start, now);
  start = g_tick_window.load();
  if ((now - start >= 1000 || g_last_dt_bits.exchange(dt_bits) != dt_bits) &&
      g_tick_window.compare_exchange_strong(start, now)) {
    rex::audio_trace::line("SIMTICK", "%llu\t%llu\t%.6f\t%d\t%08X", static_cast<unsigned long long>(g_ticks.exchange(0)),
                           static_cast<unsigned long long>(g_ticks_bit1.exchange(0)), dt1, cd1, sim);
  }
}
