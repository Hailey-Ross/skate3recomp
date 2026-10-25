// Research hooks (tracing tools; not intended for the upstream project): the replay recorder's frame rate.
// Category: SKATE3_TRACE=replay (src/research/trace_common.h).
//
// The SimController path (sub_826D78B0) commits one replay frame through sub_82726908 per sim step. Counting the
// commits per second gives the record rate, so the tracker's 5400-frame cap becomes seconds.
//   REPLAYREC <ms> commits in the last second | total commits | DataMan (r3)
// Once a second it also logs the replay camera's look target (b406): the entry of the camera target list
// [0x83083C38]+0x2E680 (stride 0xB0, count +0x4F0) whose flag word (+0xBC) has bit 0x400000, as the subject getter
// sub_826824F8 picks it: its translation (+0x60, game axes) and compose flag (+0xC0), then every entry's translation.
//   CAMTGT <ms> count | flagged index | +0xC0 | flagged x y z | i:flags:x y z ...
// The camera system's per-frame resolve sub_828F2C38 (ticked during the replay): its camera record [r3+0x10] and the
// record's fields 0x5D..0x67 (eye / look space and object, upright, frame variant, smoothing; field n at rec + 4n,
// typed by the descriptor [0x830BBE98 + 4n], +0x18 type 3 / 4 = a float-coded value). Logged when the record or a field
// changes, and once a second.
//   CAMSYS <ms> lr | r3 | rec | w174..w19C (11 hex words) | types (11) | [r3+0x7C] | its vtable
#include "trace_common.h"

#include <atomic>
#include <cstdio>
#include <string>

using namespace skate3_research;

namespace {
std::atomic<uint64_t> g_total{0};
std::atomic<uint64_t> g_window_start{0};
std::atomic<uint64_t> g_window_count{0};
void LogCameraTarget(uint8_t* base) {
  const uint32_t root = TryU32(base, 0x83083C38u, 0);
  if (!Plausible(root)) return;
  const uint32_t list = root + 0x2E680u;
  const uint32_t count = TryU32(base, list + 0x4F0u, 0);
  if (count == 0 || count > 64) return;
  int flagged = -1;
  std::string all;
  for (uint32_t i = 0; i < count && i < 12; ++i) {
    const uint32_t flags = TryU32(base, list + 0xBCu + i * 0xB0u, 0);
    if (flagged < 0 && (flags & 0x400000u)) flagged = static_cast<int>(i);
    char v[64], item[96];
    Vec3Text(base, list + i * 0xB0u + 0x60u, v, sizeof v);
    std::snprintf(item, sizeof item, "%s%u:%08X:%s", i ? "|" : "", i, flags, v);
    all += item;
  }
  char pos[64] = "-";
  uint32_t compose = 0;
  if (flagged >= 0) {
    Vec3Text(base, list + flagged * 0xB0u + 0x60u, pos, sizeof pos);
    compose = TryU32(base, list + flagged * 0xB0u + 0xC0u, 0);
  }
  rex::audio_trace::line("CAMTGT", "%u	%d	%u	%s	%s", count, flagged, compose, pos, all.c_str());
}

}  // namespace

extern "C" REX_FUNC(sub_82726908) {
  if (On("replay")) {
    const uint64_t now = GetTickCount64();
    const uint64_t total = ++g_total;
    uint64_t start = g_window_start.load();
    if (start == 0) g_window_start.compare_exchange_strong(start, now);
    const uint64_t count = ++g_window_count;
    start = g_window_start.load();
    if (now - start >= 1000 && g_window_start.compare_exchange_strong(start, now)) {
      g_window_count = 0;
      rex::audio_trace::line("REPLAYREC", "%llu\t%llu\t%08X", static_cast<unsigned long long>(count),
                             static_cast<unsigned long long>(total), ctx.r3.u32);
      LogCameraTarget(base);
    }
  }
  __imp__sub_82726908(ctx, base);
}

namespace {
std::atomic<uint64_t> g_camsys_last{0};
std::atomic<uint64_t> g_camsys_hash{0};
std::atomic<uint64_t> g_camsys_calls{0};
std::atomic<uint64_t> g_camsys_count_at{0};
}  // namespace

// Called from the sub_828F2C38 hook in hooks_menus.cpp (one definition per function).
//   CAMSYSN <ms> calls in the last second | r3 | [r3+0x10] | lr
void CamSysTrace(PPCContext& ctx, uint8_t* base) {
  if (On("replay")) {
    const uint32_t self = ctx.r3.u32;
    const uint32_t rec = TryU32(base, self + 0x10u, 0);
    const uint64_t calls = ++g_camsys_calls;
    const uint64_t t = GetTickCount64();
    uint64_t at = g_camsys_count_at.load();
    if (t - at >= 1000 && g_camsys_count_at.compare_exchange_strong(at, t)) {
      g_camsys_calls = 0;
      rex::audio_trace::line("CAMSYSN", "%llu\t%08X\t%08X\t%08X", static_cast<unsigned long long>(calls), self, rec,
                             static_cast<uint32_t>(ctx.lr));
    }
    if (Plausible(rec)) {
      uint32_t words[11], types[11];
      uint64_t hash = rec;
      for (uint32_t i = 0; i < 11; ++i) {
        words[i] = TryU32(base, rec + 0x174u + i * 4u, 0);
        const uint32_t desc = TryU32(base, 0x830BC00Cu + i * 4u, 0);
        types[i] = Plausible(desc) ? TryU32(base, desc + 0x18u, 0xFFu) : 0xFFu;
        hash = hash * 1099511628211ull ^ words[i];
      }
      const uint64_t now = GetTickCount64();
      const uint64_t last = g_camsys_last.load();
      const bool changed = hash != g_camsys_hash.exchange(hash);
      if ((changed && now - last >= 50) || now - last >= 1000) {
        g_camsys_last = now;
        const uint32_t sub = TryU32(base, self + 0x7Cu, 0);
        const uint32_t vt = Plausible(sub) ? TryU32(base, sub, 0) : 0;
        char w[128], t[64];
        int n = 0, m = 0;
        for (uint32_t i = 0; i < 11; ++i) {
          n += std::snprintf(w + n, sizeof w - n, "%s%08X", i ? " " : "", words[i]);
          m += std::snprintf(t + m, sizeof t - m, "%s%u", i ? " " : "", types[i]);
        }
        rex::audio_trace::line("CAMSYS", "%08X\t%08X\t%08X\t%s\t%s\t%08X\t%08X", static_cast<uint32_t>(ctx.lr), self,
                               rec, w, t, sub, vt);
      }
    }
  }
}

// Who resolves the replay editor camera per frame (USER-ASKS 350). Per function, once a second while called:
//   RCAMN <ms> function | calls in the last second | up to 4 lr:count
// and for the frame resolver sub_82C84CF8(take, &m, space, id), once a second, the frame it returned:
//   RCAMF <ms> lr | space | id | row0 xyz | row1 xyz | row2 xyz | row3 xyz
namespace {
struct CallCounter {
  const char* name;
  std::atomic<uint64_t> at{0};
  std::atomic<uint32_t> lr[4]{};
  std::atomic<uint32_t> n[4]{};
  std::atomic<uint32_t> total{0};
  void Hit(uint32_t ret) {
    ++total;
    for (int i = 0; i < 4; ++i) {
      uint32_t cur = lr[i].load();
      if (cur == ret || (cur == 0 && lr[i].compare_exchange_strong(cur, ret)) || cur == ret) {
        ++n[i];
        break;
      }
    }
    const uint64_t t = GetTickCount64();
    uint64_t last = at.load();
    if (t - last >= 1000 && at.compare_exchange_strong(last, t)) {
      char buf[160];
      int k = 0;
      for (int i = 0; i < 4; ++i)
        if (lr[i].load()) k += std::snprintf(buf + k, sizeof buf - k, "%s%08X:%u", k ? " " : "", lr[i].load(), n[i].exchange(0));
      if (k == 0) buf[0] = 0;
      rex::audio_trace::line("RCAMN", "%s\t%u\t%s", name, total.exchange(0), buf);
      for (auto& l : lr) l = 0;
    }
  }
};
CallCounter g_c84cf8{"82C84CF8"}, g_c82a50{"82C82A50"}, g_c72118{"82672118"};
std::atomic<uint64_t> g_rcamf_at{0};
}  // namespace

extern "C" REX_FUNC(sub_82C84CF8) {
  const bool on = On("replay");
  const uint32_t m = ctx.r4.u32, space = ctx.r5.u32, id = ctx.r6.u32, ret = static_cast<uint32_t>(ctx.lr);
  if (on) g_c84cf8.Hit(ret);
  __imp__sub_82C84CF8(ctx, base);
  if (on) {
    const uint64_t t = GetTickCount64();
    uint64_t last = g_rcamf_at.load();
    if (t - last >= 1000 && g_rcamf_at.compare_exchange_strong(last, t) && Plausible(m)) {
      char r[4][48];
      for (uint32_t i = 0; i < 4; ++i)
        std::snprintf(r[i], sizeof r[i], "%.4f %.4f %.4f", TryF32(base, m + i * 16u), TryF32(base, m + i * 16u + 4u),
                      TryF32(base, m + i * 16u + 8u));
      rex::audio_trace::line("RCAMF", "%08X\t%u\t%u\t%s\t%s\t%s\t%s", ret, space, id, r[0], r[1], r[2], r[3]);
    }
  }
}

extern "C" REX_FUNC(sub_82C82A50) {
  if (On("replay")) g_c82a50.Hit(static_cast<uint32_t>(ctx.lr));
  __imp__sub_82C82A50(ctx, base);
}

// Also the raw camera matrix it copies out (r3), once a second, next to RCAMF:
//   RCAMRAW <ms> lr | row0 xyz | row1 xyz | row2 xyz | row3 xyz
std::atomic<uint64_t> g_rcamraw_at{0};

extern "C" REX_FUNC(sub_82672118) {
  const bool on = On("replay");
  const uint32_t out = ctx.r3.u32, ret = static_cast<uint32_t>(ctx.lr);
  if (on) g_c72118.Hit(ret);
  __imp__sub_82672118(ctx, base);
  if (on) {
    const uint64_t t = GetTickCount64();
    uint64_t last = g_rcamraw_at.load();
    if (t - last >= 1000 && g_rcamraw_at.compare_exchange_strong(last, t) && Plausible(out)) {
      char r[4][48];
      for (uint32_t i = 0; i < 4; ++i)
        std::snprintf(r[i], sizeof r[i], "%.4f %.4f %.4f", TryF32(base, out + i * 16u), TryF32(base, out + i * 16u + 4u),
                      TryF32(base, out + i * 16u + 8u));
      rex::audio_trace::line("RCAMRAW", "%08X\t%s\t%s\t%s\t%s", ret, r[0], r[1], r[2], r[3]);
    }
  }
}

// The replay editor camera's per-frame resolve sub_82C8C470 (from 82C8B9F8, the 82C8A960 tick; b611): raw arguments
// and the 3 floats at r3 after the call. Logged when the caller's argument set changes, and once a second.
//   RCAMP <ms> lr | r3 | r4 | r5 | r6 | r7 | r8 | r9 | r10 | f1 | [r3] xyz after
CallCounter g_c8c470{"82C8C470"};
std::atomic<uint64_t> g_rcamp_at{0};
std::atomic<uint64_t> g_rcamp_hash{0};

extern "C" REX_FUNC(sub_82C8C470) {
  const bool on = On("replay");
  const uint32_t a[8] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  const uint32_t ret = static_cast<uint32_t>(ctx.lr);
  const double f1 = ctx.f1.f64;
  if (on) g_c8c470.Hit(ret);
  __imp__sub_82C8C470(ctx, base);
  if (on) {
    uint64_t hash = ret;
    for (int i = 2; i < 8; ++i) hash = hash * 1099511628211ull ^ a[i];
    const uint64_t t = GetTickCount64();
    const uint64_t last = g_rcamp_at.load();
    const bool changed = hash != g_rcamp_hash.exchange(hash);
    if ((changed && t - last >= 50) || t - last >= 1000) {
      g_rcamp_at = t;
      char out[64] = "-";
      if (Plausible(a[0]))
        std::snprintf(out, sizeof out, "%.4f %.4f %.4f", TryF32(base, a[0]), TryF32(base, a[0] + 4u), TryF32(base, a[0] + 8u));
      rex::audio_trace::line("RCAMP", "%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%.4f\t%s", ret, a[0], a[1], a[2],
                             a[3], a[4], a[5], a[6], a[7], f1, out);
    }
  }
}
