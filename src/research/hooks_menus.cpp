// Research hooks (tracing tools; not intended for the upstream project): the Trick Guide 3D demo camera.
// Category: SKATE3_TRACE=menus (src/research/trace_common.h). Off = one table lookup per call.
//
// Gate: G = [0x83083C38]; the u32 at G+17616 is 1 while the Trick Guide set is loaded. The per-frame camera hooks
// log only while it is 1. SKATE3_TG_EVERY_MS=<ms> thins TGCAM (0 or unset = every frame, cap 60000 lines).
//
// Per-frame ICE camera apply 828F1C90 (P = ICE camera player): element getter 828EEA20 fills the element values,
// 828F2C38 and 828F2B58 are snapshotted, and the final matrix reaches the camera's vt+20 (82674860, lr 828F1E4C).
//
// Kinds (fields after ms, tab separated):
//   TGCAM   seq | P | cam | f1 arg | P+108 | P+112 | P+116 | P+121 byte | fov deg G+0x30D00 | final M (16) |
//           elements 0..9 (10) | after 828F2C38 (16) | before 828F2B58 (16) | after 828F2B58 (16) | ICE buffer |
//           [P+16] 48 bytes hex | [P+16]+368 48 bytes hex (element words 92..103: the eye/look space modes
//           93..96 and 101..103 that 828F2C38 passes to 828F36B0)                                (17 fields)
//   TGFIT   obj | track index obj+200 | key obj+196 | holder | track ptr | len before | len after | ICE buffer
//                                                                                             (8 fields; 82820E98
//           with the clip-fit lookup 828F4C90 at lr 82821294, on change)
//   TGPROP  track | result | lr | track+8 | [track+12]+12                                    (5 fields; 828EEB20,
//           property 147 only)
//   TGTAN   r3 | r4 | r5 | r6 | r7 | u16 r4+60 | u16 r4+64 | lr                              (8 fields; 828F0990,
//           distinct r4, cap 64)
//   TGE87   r3 | r4 | ret r3 | ret f1 | lr                                                    (5 fields; 828EE8E8
//           called from 828F0990, cap 64)
//   TGLOC   model list | 64-bit location key                                                 (2 fields; 827950A8,
//           on change)
#include "trace_common.h"

using namespace skate3_research;

namespace {

constexpr uint32_t kGlobal = 0x83083C38u;     // G
constexpr uint32_t kSetLoaded = 17616u;       // G+17616 = 1 while the Trick Guide set is loaded
constexpr uint32_t kFovDeg = 0x30D00u;        // G+0x30D00 f32 degrees (camera vt+12 writes it)
constexpr uint32_t kIceBuffer = 0x83083C84u;
constexpr uint32_t kSetTransformLr = 0x828F1E4Cu;
constexpr uint32_t kFitLookupLr = 0x82821294u;

std::atomic<bool> g_open{false};
uint32_t g_player = 0;  // [P+16] of the current frame
float g_elem[10];       // element ids 0..9 as the getter returned them this frame
float g_c38[16], g_b58_before[16], g_b58_after[16];
uint32_t g_seq = 0, g_lines = 0;
uint64_t g_last_ms = 0;

uint64_t EveryMs() {
  static const uint64_t v = [] {
    const char* s = std::getenv("SKATE3_TG_EVERY_MS");
    return s && *s ? std::strtoull(s, nullptr, 10) : 0ull;
  }();
  return v;
}

bool SetLoaded(uint8_t* base) {
  const uint32_t g = TryU32(base, kGlobal, 0);
  return Plausible(g) && TryU32(base, g + kSetLoaded, 0) == 1u;
}

void Mat(uint8_t* base, uint32_t at, float (&out)[16]) {
  for (int i = 0; i < 16; ++i) out[i] = TryF32(base, at + 4u * i);
}

int MatText(const float (&m)[16], char* out, size_t n) {
  int w = 0;
  for (int i = 0; i < 16 && w < int(n); ++i) w += std::snprintf(out + w, n - w, i ? " %.5g" : "%.5g", m[i]);
  return w;
}

uint32_t g_fit_holder = 0;
float g_fit_before = NAN;
uint32_t g_fit_last[3] = {};

uint32_t g_tan_seen[64];
int g_tan_n = 0;
int g_e87_n = 0;

uint64_t g_loc_last = 0;

}  // namespace

// Per-frame arm: ICE camera apply.
extern "C" REX_FUNC(sub_828F1C90) {
  if (On("menus")) {
    const bool open = SetLoaded(base);
    g_open.store(open, std::memory_order_relaxed);
    if (open) {
      g_player = TryU32(base, ctx.r3.u32 + 16, 0);
      for (float& e : g_elem) e = NAN;
    }
  }
  __imp__sub_828F1C90(ctx, base);
  g_open.store(false, std::memory_order_relaxed);
}

// Element getter: remember the values of ids 0..9 for the armed player.
extern "C" REX_FUNC(sub_828EEA20) {
  const uint32_t player = ctx.r3.u32, id = ctx.r4.u32;
  __imp__sub_828EEA20(ctx, base);
  if (id < 10 && g_open.load(std::memory_order_relaxed) && player == g_player) g_elem[id] = float(ctx.f1.f64);
}

void CamSysTrace(PPCContext& ctx, uint8_t* base);  // hooks_replay.cpp

extern "C" REX_FUNC(sub_828F2C38) {
  const uint32_t m = ctx.r4.u32;
  CamSysTrace(ctx, base);
  __imp__sub_828F2C38(ctx, base);
  if (g_open.load(std::memory_order_relaxed)) Mat(base, m, g_c38);
}

extern "C" REX_FUNC(sub_828F2B58) {
  const uint32_t m = ctx.r4.u32;
  const bool open = g_open.load(std::memory_order_relaxed);
  if (open) Mat(base, m, g_b58_before);
  __imp__sub_828F2B58(ctx, base);
  if (open) Mat(base, m, g_b58_after);
}

// Camera vt+20 (set transform), only from the ICE camera apply.
extern "C" REX_FUNC(sub_82674860) {
  if (static_cast<uint32_t>(ctx.lr) == kSetTransformLr && g_open.load(std::memory_order_relaxed) &&
      On("menus") && g_lines < 60000) {
    const uint64_t now = GetTickCount64();
    if (!EveryMs() || now - g_last_ms >= EveryMs()) {
      g_last_ms = now;
      ++g_lines;
      const uint32_t p = ctx.r29.u32, g = TryU32(base, kGlobal, 0);
      float m[16];
      Mat(base, ctx.r4.u32, m);
      char fm[400], fc[400], fb[400], fa[400], el[200], ph[97], ps[97];
      MatText(m, fm, sizeof fm);
      MatText(g_c38, fc, sizeof fc);
      MatText(g_b58_before, fb, sizeof fb);
      MatText(g_b58_after, fa, sizeof fa);
      int w = 0;
      for (int i = 0; i < 10; ++i) w += std::snprintf(el + w, sizeof el - w, i ? " %.5g" : "%.5g", g_elem[i]);
      HexAt(base, g_player, ph);
      HexAt(base, g_player + 368, ps);
      rex::audio_trace::line("TGCAM", "%u\t%08X\t%08X\t%.5g\t%08X\t%.5g\t%.5g\t%u\t%.5g\t%s\t%s\t%s\t%s\t%s\t%08X\t%s\t%s",
                             g_seq++, p, ctx.r3.u32, ctx.f1.f64, TryU32(base, p + 108, 0), TryF32(base, p + 112),
                             TryF32(base, p + 116), (TryU32(base, p + 120, 0) >> 16) & 0xFFu,
                             TryF32(base, g + kFovDeg), fm, el, fc, fb, fa, TryU32(base, kIceBuffer, 0), ph, ps);
    }
  }
  __imp__sub_82674860(ctx, base);
}

// Clip fit: the lookup's holder, then the track length after the fit.
extern "C" REX_FUNC(sub_828F4C90) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_828F4C90(ctx, base);
  if (lr == kFitLookupLr && On("menus")) {
    g_fit_holder = ctx.r3.u32;
    const uint32_t t = g_fit_holder ? TryU32(base, g_fit_holder, 0) : 0;
    g_fit_before = Plausible(t) ? TryF32(base, t + 12) : NAN;
  }
}

extern "C" REX_FUNC(sub_82820E98) {
  const uint32_t obj = ctx.r3.u32;
  g_fit_holder = 0;
  __imp__sub_82820E98(ctx, base);
  if (!g_fit_holder || !On("menus")) return;
  const uint32_t track = TryU32(base, g_fit_holder, 0), index = TryU32(base, obj + 200, 0);
  const float after = Plausible(track) ? TryF32(base, track + 12) : NAN;
  uint32_t after_bits;
  std::memcpy(&after_bits, &after, 4);
  if (track == g_fit_last[0] && index == g_fit_last[1] && after_bits == g_fit_last[2]) return;
  g_fit_last[0] = track;
  g_fit_last[1] = index;
  g_fit_last[2] = after_bits;
  rex::audio_trace::line("TGFIT", "%08X\t%d\t%08X\t%08X\t%08X\t%.5g\t%.5g\t%08X", obj, int(index),
                         TryU32(base, obj + 196, 0), g_fit_holder, track, g_fit_before, after,
                         TryU32(base, kIceBuffer, 0));
}

// Property 147 lookups.
extern "C" REX_FUNC(sub_828EEB20) {
  const uint32_t track = ctx.r3.u32, prop = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_828EEB20(ctx, base);
  if (prop == 147u && On("menus")) {
    const uint32_t g12 = TryU32(base, track + 12, 0);
    rex::audio_trace::line("TGPROP", "%08X\t%08X\t%08X\t%.5g\t%.5g", track, ctx.r3.u32, lr, TryF32(base, track + 8),
                           Plausible(g12) ? TryF32(base, g12 + 12) : NAN);
  }
}

// Tangent / spline switch per element (called when a track is bound or started: 828F0BB0 from 828F0FE8 / 828F04A8,
// outside the per-frame apply, so gated on the set flag, not g_open).
extern "C" REX_FUNC(sub_828F0990) {
  if (g_tan_n < 64 && On("menus") && SetLoaded(base)) {
    const uint32_t r4 = ctx.r4.u32;
    bool seen = false;
    for (int i = 0; i < g_tan_n; ++i) seen |= g_tan_seen[i] == r4;
    if (!seen) {
      g_tan_seen[g_tan_n++] = r4;
      const uint32_t w60 = TryU32(base, r4 + 60, 0xFFFFFFFFu), w64 = TryU32(base, r4 + 64, 0xFFFFFFFFu);
      rex::audio_trace::line("TGTAN", "%08X\t%08X\t%08X\t%08X\t%08X\t%04X\t%04X\t%08X", ctx.r3.u32, r4, ctx.r5.u32,
                             ctx.r6.u32, ctx.r7.u32, w60 >> 16, w64 >> 16, static_cast<uint32_t>(ctx.lr));
    }
  }
  __imp__sub_828F0990(ctx, base);
}

extern "C" REX_FUNC(sub_828EE8E8) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr), r3 = ctx.r3.u32, r4 = ctx.r4.u32;
  __imp__sub_828EE8E8(ctx, base);
  if (lr >= 0x828F0990u && lr < 0x828F0BB0u && g_e87_n < 64 && On("menus") &&
      SetLoaded(base)) {
    ++g_e87_n;
    rex::audio_trace::line("TGE87", "%08X\t%u\t%08X\t%.5g\t%08X", r3, r4, ctx.r3.u32, ctx.f1.f64, lr);
  }
}

// Render location selector.
extern "C" REX_FUNC(sub_827950A8) {
  const uint32_t list = ctx.r3.u32;
  __imp__sub_827950A8(ctx, base);
  if (ctx.r3.u64 != g_loc_last && On("menus")) {
    g_loc_last = ctx.r3.u64;
    rex::audio_trace::line("TGLOC", "%08X\t%016llX", list, static_cast<unsigned long long>(g_loc_last));
  }
}
