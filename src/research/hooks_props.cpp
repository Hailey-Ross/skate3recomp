// Research hooks (tracing tools; not intended for the upstream project): dynamic props (DMOs) and what wakes them.
// Category: SKATE3_TRACE=props (src/research/trace_common.h).
//
//   PROPHIT  <ms> dmo | event | e76 | other | kind | flag4c | flag137 | kicks | callers   (9 fields; 2026-10-17)
//            The DMO collision-event callback sub_82C54618 (r3 = the DMO's interface at dmo+0x88, r4 = the event).
//            dmo = r3 - 0x88; e76 = [event+76]; other = [e76+32] (the object the DMO touched, as the callback
//            reads it); kind = [[other+0x18]+0x18]; flag4c = byte [[other+0x1C]+0x4C]; flag137 = byte
//            [[other+0x48]+0x137]; kicks = calls of sub_82C530F0 made inside this callback. "-1" where a read is not
//            possible. Only object contacts (e76 != 0) or calls that kicked; at most 20 lines a second.
//   PROPKICK <ms> raw first-pass line of sub_82C530F0 (the DMO velocity kick that wakes the prop through the body
//            velocity setters).
//   PROPWAKE <ms> raw first-pass line of sub_82C53480 (the DMO wake method, vtable 0x82322ED8 slot 23).
//   PROPUPRIGHT / PROPGROUPWAKE: raw first-pass lines of sub_82C54BF0 (upright) and sub_82C58D00 (group wake).
#include "trace_common.h"

using namespace skate3_research;

namespace {
thread_local uint32_t g_kicks = 0;

uint32_t TryByte(uint8_t* base, uint32_t addr) {
  const uint32_t word = TryU32(base, addr & ~3u);
  if (word == 0xFFFFFFFFu && !Readable(base, addr & ~3u, 4)) return 0xFFFFFFFFu;
  return (word >> (24 - 8 * (addr & 3u))) & 0xFFu;
}

uint32_t Follow(uint8_t* base, uint32_t ptr, uint32_t offset) {
  if (ptr == 0 || ptr == 0xFFFFFFFFu) return 0xFFFFFFFFu;
  return TryU32(base, ptr + offset);
}
}  // namespace

extern "C" REX_FUNC(sub_82C530F0) {
  static FirstPassLimit limit;
  uint32_t skipped = 0;
  ++g_kicks;
  const bool log = On("props") && FirstPassAllow(limit, skipped);
  FirstPassEntry e{};
  if (log) e = FirstPassCapture(ctx, base);
  __imp__sub_82C530F0(ctx, base);
  if (log) FirstPassLog("PROPKICK", "", skipped, e, ctx);
}

extern "C" REX_FUNC(sub_82C54618) {
  if (!On("props")) {
    __imp__sub_82C54618(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32, event = ctx.r4.u32;
  char callers[64] = {};
  CallerChain(ctx, base, callers);
  g_kicks = 0;
  __imp__sub_82C54618(ctx, base);
  const uint32_t kicks = g_kicks;
  // Contacts with the static world carry no object (e76 = 0) and arrive every tick for a resting prop: they would
  // eat the rate limit and hide the hits that matter, so only object contacts (and any kick) are logged.
  const uint32_t e76 = TryU32(base, event + 76);
  if ((e76 == 0 || e76 == 0xFFFFFFFFu) && kicks == 0) return;
  static FirstPassLimit limit;
  uint32_t skipped = 0;
  if (!FirstPassAllow(limit, skipped)) return;
  const uint32_t other = Follow(base, e76, 32);
  const uint32_t kind = Follow(base, Follow(base, other, 0x18), 0x18);
  const uint32_t a = Follow(base, other, 0x1C), b = Follow(base, other, 0x48);
  const uint32_t flag4c = (a == 0xFFFFFFFFu || a == 0) ? 0xFFFFFFFFu : TryByte(base, a + 0x4C);
  const uint32_t flag137 = (b == 0xFFFFFFFFu || b == 0) ? 0xFFFFFFFFu : TryByte(base, b + 0x137);
  rex::audio_trace::line("PROPHIT", "%08X\t%08X\t%08X\t%08X\t%d\t%d\t%d\t%u\t%s", self - 0x88, event, e76, other,
                         static_cast<int32_t>(kind), static_cast<int32_t>(flag4c), static_cast<int32_t>(flag137), kicks,
                         callers);
}

FIRST_PASS_HOOK(82C53480, "props", "PROPWAKE", "")
FIRST_PASS_HOOK(82C54BF0, "props", "PROPUPRIGHT", "")
FIRST_PASS_HOOK(82C58D00, "props", "PROPGROUPWAKE", "")
