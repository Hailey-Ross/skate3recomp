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
