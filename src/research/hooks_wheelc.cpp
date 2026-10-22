// Research hooks (tracing tools; not intended for the upstream project): board wheel contacts against world
// triangles. Category: SKATE3_TRACE=physics, active only with SKATE3_WHLC="x z radius [maxr]" (an area around the
// wheel centre; maxr > 0 keeps spheres of at most that radius).
//
// 8277B720 (called from 8277BC58, lr 0x8277BF90): r3 = moving volume (+0x00 centre, +0x80 radius, +0x84 / +0x88 ids,
// +0x90 type), r4 = world triangle (v0 +0x00, normal +0x10, v1 +0x20, v2 +0x30, ids +0x84 / +0x88, type +0x90,
// flags +0x94, edge cosines +0x98 / +0x9C / +0xA0), r6 = output (+0x10 count, +0x20 normal, pair i A +0x30 + 0x20 i,
// B +0x40 + 0x20 i), the caller's r30 = the rigid body. The fixup 82AD3130 (lr 0x8277B9CC) and its region classify
// 82AD2A50 (lr 0x82AD325C) fill a per-thread record while the main hook's call runs.
//   WHLC <ms> body | A id84 id88 type r | centre | tri id84 id88 | v0 | v1 | v2 | n | flags | cos0 cos1 cos2 |
//        bytes b0 b1 b2 | region (-1 none, 0 face, 1 e0, 2 e1, 3 v1, 4 e2, 5 v0, 6 v2) | fix | n_pre | n_out | count |
//        sep0 | A0 | B0 | ret
//   WHLCARM <ms> x z radius maxr | hooked function | first body
//   WHLCSUM <ms> lines | dropped | candidates | accepted   (1 Hz, cap 2000 lines a second)
//   FUSEPL <ms> job | job+4 +8 +12 (floats) | builder | r5 r6 r7 r8 | plane count | per plane: [r8+16i] / [r7+16i]
//        / [r5+16i] / [r6+16i] (x y z w)   (the fuser's ground plane pass 82874248, any physics run; once per fuse)
// Bytes are rebuilt from the decoded cosine (exact match over 28 codes) and the flag bits; ?? = no match.
#include "trace_common.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace skate3_research;

namespace {
struct Area {
  bool set = false;
  float x = 0, z = 0, r = 0, maxr = 0;
};

const Area& WhlcArea() {
  static const Area a = [] {
    Area v;
    const char* s = std::getenv("SKATE3_WHLC");
    const int n = s ? std::sscanf(s, "%f %f %f %f", &v.x, &v.z, &v.r, &v.maxr) : 0;
    v.set = n >= 3;
    return v;
  }();
  return a;
}

struct FixRec {
  bool armed = false, ran = false;
  int region = -1;
  uint32_t fix = 0;
  float pre[3] = {};
};
thread_local FixRec t_fix;
std::atomic<uint64_t> g_win{0};
std::atomic<uint32_t> g_lines{0}, g_drop{0}, g_cand{0}, g_acc{0};
std::atomic<bool> g_armed{false};

bool Allow() {
  const uint64_t now = GetTickCount64();
  uint64_t w = g_win.load();
  if (now - w >= 1000 && g_win.compare_exchange_strong(w, now)) {
    const uint32_t l = g_lines.exchange(0), d = g_drop.exchange(0), c = g_cand.exchange(0), a = g_acc.exchange(0);
    if (l || d) rex::audio_trace::line("WHLCSUM", "%u\t%u\t%u\t%u", l, d, c, a);
  }
  if (g_lines.fetch_add(1) >= 2000) {
    g_drop.fetch_add(1);
    return false;
  }
  return true;
}

const char* EdgeByte(float c, uint32_t flags, int i, char* out) {
  int code = -1;
  for (int e = 0; e < 28 && code < 0; ++e)
    if (1.0f - 9.8696044f / static_cast<float>(8u << e) == c) code = e;
  if (code < 0) {
    std::snprintf(out, 4, "??");
    return out;
  }
  const uint32_t b = static_cast<uint32_t>(code) | (((flags >> (5 + i)) & 1u) << 5) | (((flags >> (9 + i)) & 1u) << 6);
  std::snprintf(out, 4, "%02X", b);
  return out;
}
}  // namespace

extern "C" REX_FUNC(sub_82874248) {
  if (On("physics")) {
    const uint32_t job = ctx.r3.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32, r8 = ctx.r8.u32;
    const uint32_t count = ctx.r9.u32;
    std::string planes;
    auto vec4 = [&](uint32_t at) {
      char t[96];
      std::snprintf(t, sizeof t, "%.6g %.6g %.6g %.6g", TryF32(base, at), TryF32(base, at + 4), TryF32(base, at + 8),
                    TryF32(base, at + 12));
      return std::string(t);
    };
    for (uint32_t i = 0; i < count && i < 8; ++i) {
      planes += (i ? "|" : "") + vec4(r8 + 16 * i) + " / " + vec4(r7 + 16 * i) + " / " + vec4(r5 + 16 * i) + " / " +
                vec4(r6 + 16 * i);
    }
    rex::audio_trace::line("FUSEPL", "%08X\t%.7g %.7g %.7g\t%08X\t%08X %08X %08X %08X\t%u\t%s", job,
                           TryF32(base, job + 4), TryF32(base, job + 8), TryF32(base, job + 12), ctx.r4.u32, r5, r6, r7,
                           r8, count, planes.c_str());
  }
  __imp__sub_82874248(ctx, base);
}

extern "C" REX_FUNC(sub_82AD2A50) {
  const bool mine = t_fix.armed && ctx.lr == 0x82AD325Cu;
  __imp__sub_82AD2A50(ctx, base);
  if (mine) t_fix.region = static_cast<int>(ctx.r3.u32);
}

extern "C" REX_FUNC(sub_82AD3130) {
  const bool mine = t_fix.armed && ctx.lr == 0x8277B9CCu;
  const uint32_t n = ctx.r4.u32;
  if (mine) {
    t_fix.ran = true;
    for (int i = 0; i < 3; ++i) t_fix.pre[i] = TryF32(base, n + 4 * i);
  }
  __imp__sub_82AD3130(ctx, base);
  if (mine) t_fix.fix = ctx.r3.u32 != 0;
}

extern "C" REX_FUNC(sub_8277B720) {
  const Area& area = WhlcArea();
  if (!area.set || ctx.lr != 0x8277BF90u || !On("physics")) {
    __imp__sub_8277B720(ctx, base);
    return;
  }
  const uint32_t a = ctx.r3.u32, tri = ctx.r4.u32, out = ctx.r6.u32, body = ctx.r30.u32;
  if (!Plausible(a) || !Readable(base, a, 0x94)) {
    __imp__sub_8277B720(ctx, base);
    return;
  }
  const float dx = LoadF32(base, a) - area.x, dz = LoadF32(base, a + 8) - area.z;
  const bool inside = dx * dx + dz * dz <= area.r * area.r &&
                    (area.maxr <= 0 || (LoadU32(base, a + 0x90) == 1 && LoadF32(base, a + 0x80) <= area.maxr));
  if (!inside) {
    __imp__sub_8277B720(ctx, base);
    return;
  }
  t_fix = FixRec{};
  t_fix.armed = true;
  __imp__sub_8277B720(ctx, base);
  const uint32_t ret = ctx.r3.u32;
  t_fix.armed = false;
  if (!t_fix.ran) return;
  g_cand.fetch_add(1);
  if (ret) g_acc.fetch_add(1);
  bool first = false;
  if (g_armed.compare_exchange_strong(first, true))
    rex::audio_trace::line("WHLCARM", "%.3f\t%.3f\t%.3f\t%.3f\t%08X\t%08X", area.x, area.z, area.r, area.maxr,
                           0x8277B720u, body);
  if (!Allow() || !Plausible(tri) || !Plausible(out) || !Readable(base, tri, 0xA4) || !Readable(base, out, 0x50)) return;
  char c[64], v0[64], v1[64], v2[64], n[64], no[64], pa[64], pb[64], b0[4], b1[4], b2[4];
  Vec3Text(base, a, c, sizeof c);
  Vec3Text(base, tri, v0, sizeof v0);
  Vec3Text(base, tri + 0x20, v1, sizeof v1);
  Vec3Text(base, tri + 0x30, v2, sizeof v2);
  Vec3Text(base, tri + 0x10, n, sizeof n);
  Vec3Text(base, out + 0x20, no, sizeof no);
  Vec3Text(base, out + 0x30, pa, sizeof pa);
  Vec3Text(base, out + 0x40, pb, sizeof pb);
  const uint32_t flags = LoadU32(base, tri + 0x94), count = LoadU32(base, out + 0x10);
  const float k0 = LoadF32(base, tri + 0x98), k1 = LoadF32(base, tri + 0x9C), k2 = LoadF32(base, tri + 0xA0);
  float sep = NAN;
  if (count) {
    sep = 0;
    for (int i = 0; i < 3; ++i)
      sep += (LoadF32(base, out + 0x30 + 4 * i) - LoadF32(base, out + 0x40 + 4 * i)) * LoadF32(base, out + 0x20 + 4 * i);
  }
  rex::audio_trace::line(
      "WHLC",
      "%08X\t%08X\t%08X\t%u\t%.4f\t%s\t%08X\t%08X\t%08X\t%s\t%s\t%s\t%s\t%08X\t%.7g %.7g %.7g\t%s %s %s\t%d\t%u\t"
      "%.4f %.4f %.4f\t%s\t%u\t%.5f\t%s\t%s\t%u",
      body, LoadU32(base, a + 0x84), LoadU32(base, a + 0x88), LoadU32(base, a + 0x90), LoadF32(base, a + 0x80), c, tri,
      LoadU32(base, tri + 0x84), LoadU32(base, tri + 0x88), v0, v1, v2, n, flags, k0, k1, k2, EdgeByte(k0, flags, 0, b0),
      EdgeByte(k1, flags, 1, b1), EdgeByte(k2, flags, 2, b2), t_fix.region, t_fix.fix, t_fix.pre[0], t_fix.pre[1],
      t_fix.pre[2], no, count, sep, pa, pb, ret);
}
