// Research hooks (tracing tools; not intended for the upstream project): keep the game's audio "set parameter" command
// queue from overflowing while research hooks are active, and log its state.
//
// The queue Q (global pointer 0x8307762C; also [obj+8] of every sound object) has a 256,000-byte buffer at
// Q+48 (capacity at Q+200, never read by the game), a byte write offset at Q+204, a high-water mark at Q+208
// and a drain counter at Q+256. ~700 inlined producers append 16-byte commands WITHOUT a bounds check; the
// audio render thread drains it once per 256-sample block (sub_82B48530). Retail never overflows, but our
// hooks on the render thread can delay drains long enough to overflow it (crashes in sub_824C9058,
// sub_82491180; plain recomp without tracing doesn't crash on the same Mega Park ramp run).
//
// 1) Grow the buffer (only when tracing is enabled): after the queue is created (sub_82B47E68, once at
//    startup, before any producer), allocate kGrownBytes of guest system heap and point Q+48 / Q+200 at it.
//    Same commands, same order, same drain points as retail whenever retail wouldn't overflow; the original
//    256 KB block is leaked (only freed on the failed-init path).
//      QGROW <ms> Q | old buffer | old capacity | new buffer | new capacity
// 2) Watchdog (category `audio`), from the game-side audio tick hook in hooks_watch.cpp: every 100 ms
//      QSTAT <ms> write offset (bytes) | high-water mark | drain counter
//    A frozen drain counter = drains stopped (host audio stall); a growing backlog with a moving counter =
//    the render thread is too slow (our hooks).
#include "trace_common.h"

#include <rex/system/kernel_state.h>

using namespace skate3_research;

namespace {
constexpr uint32_t kQueueGlobal = 0x8307762Cu;
constexpr uint32_t kGrownBytes = 4u << 20;  // 4 MB: 16x retail's 256,000 bytes

void StoreU32(uint8_t* base, uint32_t addr, uint32_t value) {
  const uint32_t raw = __builtin_bswap32(value);
  std::memcpy(base + addr, &raw, 4);
}
}  // namespace

extern "C" REX_FUNC(sub_82B47E68) {
  __imp__sub_82B47E68(ctx, base);
  if (!rex::audio_trace::enabled()) return;  // plain play: leave the game untouched
  const uint32_t q = TryU32(base, kQueueGlobal, 0);
  if (!Readable(base, q, 260)) return;
  const uint32_t old_buffer = LoadU32(base, q + 48), old_capacity = LoadU32(base, q + 200);
  if (old_capacity >= kGrownBytes) return;
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) return;
  const uint32_t buffer = ks->memory()->SystemHeapAlloc(kGrownBytes, 0x80);
  if (!buffer || !Readable(base, buffer, kGrownBytes)) return;
  StoreU32(base, q + 48, buffer);
  StoreU32(base, q + 200, kGrownBytes);
  rex::audio_trace::line("QGROW", "%08X\t%08X\t%u\t%08X\t%u", q, old_buffer, old_capacity, buffer, kGrownBytes);
}

// Called from the audio tick hook (hooks_watch.cpp) once per game frame.
void QueueWatchdog(uint8_t* base) {
  static uint64_t last_ms = 0;
  const uint64_t now = GetTickCount64();
  if (now - last_ms < 100) return;
  last_ms = now;
  const uint32_t q = TryU32(base, kQueueGlobal, 0);
  if (!Readable(base, q, 260)) return;
  rex::audio_trace::line("QSTAT", "%u\t%u\t%u", LoadU32(base, q + 204), LoadU32(base, q + 208), LoadU32(base, q + 256));
}
