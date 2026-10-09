// Research hooks (tracing tools; not intended for the upstream project): front-end menu key events, to measure
// whether a held direction repeats in the menus and at what rate.
// Category: SKATE3_TRACE=feinput (src/research/trace_common.h).
//
// The menu pad poll 825DFDC8 walks the Apt key table (0x83027BF0, 20-byte records) per controller and asks the
// input-map program interpreter 8296C350 for each key's value; a non-zero value posts the key to the menu
// (input vtbl+8). The first version filtered on lr 825DFF04 (bl + 4) and saw none; the recomp passes the bl address 825DFF00 in an in-game
// pause menu run (menu moved, so the path differs), so this version also reports which callers return non-zero.
//
// Kinds (fields after ms):
//   FEKEY  <ms> r3 (map state) | r4 | r5 (program) | value | poll count            (5 fields; caller 825DFF00 only: the recomp reports the bl address in lr)
//   FELR   <ms> lr | non-zero results in the last second | last value | last r5 | poll count   (5 fields; once
//          per second per caller that returned non-zero, while the category is on)
//   HOOKARMED <ms> kind 0 0 0 0 (first call; 5-field layout like the other hooks)
#include "trace_common.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

using namespace skate3_research;

namespace {

std::atomic<bool> g_armed_key{false}, g_armed_poll{false}, g_armed_vm{false};
std::atomic<uint32_t> g_polls{0};

struct LrStat {
  uint32_t count = 0;
  double last = 0.0;
  uint32_t r5 = 0;
  uint64_t since = 0;
};
std::mutex g_lr_mutex;
std::unordered_map<uint32_t, LrStat> g_lr;

void Armed(std::atomic<bool>& flag, const char* kind) {
  bool expected = false;
  if (flag.compare_exchange_strong(expected, true)) rex::audio_trace::line("HOOKARMED", "%s\t0\t0\t0\t0", kind);
}

}  // namespace

// Menu pad poll (r3 = front-end input object): counts polls so FEKEY lines can be grouped per poll.
extern "C" REX_FUNC(sub_825DFDC8) {
  g_polls.fetch_add(1, std::memory_order_relaxed);
  if (On("feinput")) Armed(g_armed_poll, "FEPOLL");
  __imp__sub_825DFDC8(ctx, base);
}

// Input-map program interpreter (r3 = state, r4, r5 = program); returns the action value in f1.
extern "C" REX_FUNC(sub_8296C350) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  __imp__sub_8296C350(ctx, base);
  if (ctx.f1.f64 == 0.0 || !On("feinput")) return;
  Armed(g_armed_vm, "FEVM");
  const double value = ctx.f1.f64;
  const uint32_t polls = g_polls.load(std::memory_order_relaxed);
  if (lr == 0x825DFF00u) {  // the recomp reports the bl address, not bl + 4
    Armed(g_armed_key, "FEKEY");
    rex::audio_trace::line("FEKEY", "%08X\t%08X\t%08X\t%.4f\t%u", r3, r4, r5, value, polls);
  }
  const uint64_t now = GetTickCount64();
  std::lock_guard<std::mutex> lock(g_lr_mutex);
  LrStat& s = g_lr[lr];
  if (s.since == 0) s.since = now;
  ++s.count;
  s.last = value;
  s.r5 = r5;
  if (now - s.since >= 1000) {
    rex::audio_trace::line("FELR", "%08X\t%u\t%.4f\t%08X\t%u", lr, s.count, s.last, s.r5, polls);
    s = LrStat{};
    s.since = now;
  }
}
