// Research hooks (tracing tools; not intended for the upstream project): the replay recorder's frame rate.
// Category: SKATE3_TRACE=replay (src/research/trace_common.h).
//
// The SimController path (sub_826D78B0) commits one replay frame through sub_82726908 per sim step. Counting the
// commits per second gives the record rate, so the tracker's 5400-frame cap becomes seconds.
//   REPLAYREC <ms> commits in the last second | total commits | DataMan (r3)
#include "trace_common.h"

#include <atomic>

using namespace skate3_research;

namespace {
std::atomic<uint64_t> g_total{0};
std::atomic<uint64_t> g_window_start{0};
std::atomic<uint64_t> g_window_count{0};
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
    }
  }
  __imp__sub_82726908(ctx, base);
}
