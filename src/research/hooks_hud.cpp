// Research hooks (tracing tools; not intended for the upstream project): the HUD minimap's draw inputs. Category:
// SKATE3_TRACE=render (src/research/trace_common.h).
//
//   MMDRAW <ms> lr | component | player x z (f1 f2) | r4 | r6 | r7 | matrix r5: 16 floats | callers
//          (minimap component draw sub_82665388, 1 / s)
//   MMITEM <ms> lr | r3 | r4 | item x z (+0x10 +0x14) | angle +0x20 | scale +0x24 | visible +0x28 | fade +0x30 | rgba +0x40
//          (icon draw sub_8268CB40, <= 10 / s; item = r4)
//   MMQUAD <ms> lr | r3 | r4 | r5 | r6 | r4: 16 floats | r5: 16 floats
//          (quad submit sub_824FA550 when called from sub_8268CB40 or sub_82665848, <= 10 / s)
#include "trace_common.h"

#include <cmath>
#include <cstdio>
#include <mutex>

using namespace skate3_research;

namespace {

struct Limit {
  std::mutex mutex;
  uint64_t tick = 0;
  uint32_t lines = 0;
};

bool Allow(Limit& limit, uint32_t per_second) {
  std::lock_guard<std::mutex> lock(limit.mutex);
  const uint64_t second = GetTickCount64() / 1000;
  if (second != limit.tick) {
    limit.tick = second;
    limit.lines = 0;
  }
  if (limit.lines >= per_second) return false;
  ++limit.lines;
  return true;
}

// `count` floats at addr, or "nan" when not readable.
void Floats(uint8_t* base, uint32_t addr, int count, char* out, size_t size) {
  size_t used = 0;
  out[0] = 0;
  if (!Readable(base, addr, static_cast<uint32_t>(count * 4))) {
    std::snprintf(out, size, "nan");
    return;
  }
  for (int i = 0; i < count && used < size; ++i) {
    used += std::snprintf(out + used, size - used, i ? " %.4f" : "%.4f", LoadF32(base, addr + i * 4));
  }
}

Limit g_draw, g_item, g_quad;

}  // namespace

extern "C" REX_FUNC(sub_82665388) {
  if (On("render") && Allow(g_draw, 1)) {
    char matrix[512];
    Floats(base, ctx.r5.u32, 16, matrix, sizeof(matrix));
    char callers[64];
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("MMDRAW", "%08X\t%08X\t%.3f %.3f\t%08X\t%08X\t%08X\t%s\t%s", static_cast<uint32_t>(ctx.lr),
                           ctx.r3.u32, ctx.f1.f64, ctx.f2.f64, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, matrix, callers);
  }
  __imp__sub_82665388(ctx, base);
}

extern "C" REX_FUNC(sub_8268CB40) {
  if (On("render") && Allow(g_item, 10)) {
    const uint32_t item = ctx.r4.u32;
    char pos[64], rgba[96];
    Floats(base, item + 0x10, 2, pos, sizeof(pos));
    Floats(base, item + 0x40, 4, rgba, sizeof(rgba));
    rex::audio_trace::line("MMITEM", "%08X\t%08X\t%08X\t%s\t%.4f\t%.4f\t%u\t%u\t%s", static_cast<uint32_t>(ctx.lr), ctx.r3.u32,
                           item, pos, TryF32(base, item + 0x20), TryF32(base, item + 0x24),
                           Readable(base, item + 0x28, 1) ? base[item + 0x28] : 255u,
                           Readable(base, item + 0x30, 4) ? LoadU32(base, item + 0x30) : 0xFFFFFFFFu, rgba);
  }
  __imp__sub_8268CB40(ctx, base);
}

extern "C" REX_FUNC(sub_824FA550) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const bool hud = (lr >= 0x8268CB40u && lr < 0x8268D400u) || (lr >= 0x82665848u && lr < 0x82665C58u);
  if (hud && On("render") && Allow(g_quad, 10)) {
    char a[512], b[512];
    Floats(base, ctx.r4.u32, 16, a, sizeof(a));
    Floats(base, ctx.r5.u32, 16, b, sizeof(b));
    rex::audio_trace::line("MMQUAD", "%08X\t%08X\t%08X\t%08X\t%08X\t%s\t%s", lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, a, b);
  }
  __imp__sub_824FA550(ctx, base);
}
