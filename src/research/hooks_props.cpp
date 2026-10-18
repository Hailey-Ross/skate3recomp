// Research hooks (tracing tools; not intended for the upstream project): dynamic props (DMOs) and what wakes them.
// Category: SKATE3_TRACE=props (src/research/trace_common.h).
//
//   PROPHIT  <ms> dmo | event | e76 | other | kind | actor | actor_vfn14 | kicks | callers   (9 fields; v2 2026-10-17)
//            The DMO collision-event callback sub_82C54618 (r3 = the DMO's interface at dmo+0x88, r4 = the event).
//            dmo = r3 - 0x88; e76 = [event+76] (the touching collidable); other = [e76+32] (its owner);
//            kind = [[e76+0x18]+0x18] (the callback kicks only for kind 3 / 5, or 4 with a flag); actor = the
//            owner's IActor interface (type id sub_82586478 returns "IActor"), found the way sub_82965630 does
//            ((type, interface) pairs at other+4), 0 when the owner is no actor; actor_vfn14 = IActor vtable slot
//            +0x14, which the gate calls (then byte [[ret+0x1C]+0x4C], and [[ret+0x48]+0x137] for kind 4);
//            kicks = calls of sub_82C530F0 made inside this callback. "-1" where a read is not possible. Only object
//            contacts (e76 != 0) or calls that kicked; at most 20 lines a second. (v1 read kind and the two flags
//            through the owner, which the gate does not read.)
//   PROPKICK <ms> raw first-pass line of sub_82C530F0 (the DMO velocity kick that wakes the prop through the body
//            velocity setters).
//   PROPWAKE <ms> raw first-pass line of sub_82C53480 (the DMO wake method, vtable 0x82322ED8 slot 23).
//   PROPUPRIGHT / PROPGROUPWAKE: raw first-pass lines of sub_82C54BF0 (upright) and sub_82C58D00 (group wake).
//   PROPTYPE <ms> fn | owner | owner_vtable | type | callers   (5 fields; 2026-10-17)
//            A physics assembly made with a collision type (sub_82E0E3E8 / sub_82E0E338; r5 = the type written to
//            assembly+0x18 and every part's +0x5C; query meshes get rejection flags 1 << type, and the walker's mask
//            0x6000 skips types 13 and 14). fn = E3E8 or E338; owner = r3. At most 20 lines a second.
//   PROPSETTYPE <ms> slot | object | value_130 | callers   (4 fields; 2026-10-17)
//            The DMO's collision type switches on its +0x90 interface (vtable 0x82322ED8): slot 32 sub_82C56B00 sets
//            type 13; slot 33 sub_82C56BA0 sets 14, or 12 when value_130 = [[object+0x108C]+4]+0x130 >= 5.5, and
//            starts a 30 s timer. object = r3.
#include "trace_common.h"

using namespace skate3_research;

namespace {
thread_local uint32_t g_kicks = 0;

uint32_t Follow(uint8_t* base, uint32_t ptr, uint32_t offset) {
  if (ptr == 0 || ptr == 0xFFFFFFFFu) return 0xFFFFFFFFu;
  return TryU32(base, ptr + offset);
}

// sub_82965630 without calling it: the interface map at `map` is a vector of 8-byte (type, interface) pairs.
uint32_t FindInterface(uint8_t* base, uint32_t map, uint32_t type) {
  const uint32_t begin = Follow(base, map, 0), end = Follow(base, map, 4);
  if (begin == 0xFFFFFFFFu || end == 0xFFFFFFFFu || end < begin || end - begin > 8 * 4096) return 0xFFFFFFFFu;
  for (uint32_t e = begin; e < end; e += 8) {
    if (TryU32(base, e) == type) return TryU32(base, e + 4);
  }
  return 0;
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
  const uint32_t kind = Follow(base, Follow(base, e76, 0x18), 0x18);
  const uint32_t actor = (other == 0 || other == 0xFFFFFFFFu) ? 0xFFFFFFFFu : FindInterface(base, other + 4, 0x82586478u);
  const uint32_t vfn14 = (actor == 0 || actor == 0xFFFFFFFFu) ? 0 : Follow(base, Follow(base, actor, 0), 0x14);
  rex::audio_trace::line("PROPHIT", "%08X\t%08X\t%08X\t%08X\t%d\t%08X\t%08X\t%u\t%s", self - 0x88, event, e76, other,
                         static_cast<int32_t>(kind), actor, vfn14, kicks, callers);
}

FIRST_PASS_HOOK(82C53480, "props", "PROPWAKE", "")
FIRST_PASS_HOOK(82C54BF0, "props", "PROPUPRIGHT", "")
FIRST_PASS_HOOK(82C58D00, "props", "PROPGROUPWAKE", "")

namespace {
void LogType(PPCContext& ctx, uint8_t* base, const char* fn) {
  static FirstPassLimit limit;
  uint32_t skipped = 0;
  if (!FirstPassAllow(limit, skipped)) return;
  char callers[64] = {};
  CallerChain(ctx, base, callers);
  const uint32_t owner = ctx.r3.u32;
  rex::audio_trace::line("PROPTYPE", "%s\t%08X\t%08X\t%d\t%s", fn, owner, Follow(base, owner, 0),
                         static_cast<int32_t>(ctx.r5.u32), callers);
}

void LogSetType(PPCContext& ctx, uint8_t* base, int slot) {
  char callers[64] = {};
  CallerChain(ctx, base, callers);
  const uint32_t object = ctx.r3.u32;
  const uint32_t record = Follow(base, Follow(base, object, 0x108C), 4);
  const float value = (record == 0 || record == 0xFFFFFFFFu) ? -1.0f : TryF32(base, record + 0x130);
  rex::audio_trace::line("PROPSETTYPE", "%d\t%08X\t%.3f\t%s", slot, object, value, callers);
}
}  // namespace

extern "C" REX_FUNC(sub_82E0E3E8) {
  if (On("props")) LogType(ctx, base, "E3E8");
  __imp__sub_82E0E3E8(ctx, base);
}

extern "C" REX_FUNC(sub_82E0E338) {
  if (On("props")) LogType(ctx, base, "E338");
  __imp__sub_82E0E338(ctx, base);
}

extern "C" REX_FUNC(sub_82C56B00) {
  if (On("props")) LogSetType(ctx, base, 32);
  __imp__sub_82C56B00(ctx, base);
}

extern "C" REX_FUNC(sub_82C56BA0) {
  if (On("props")) LogSetType(ctx, base, 33);
  __imp__sub_82C56BA0(ctx, base);
}
