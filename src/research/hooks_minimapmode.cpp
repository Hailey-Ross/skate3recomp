// Research hooks (tracing tools; not intended for the upstream project): who sets the HUD minimap mode (b585).
// Category: SKATE3_TRACE=minimap.
//   MMMODE  <ms> owner | mode | GameMode | online | [0x830670CC] | managers | callers   (82552650, after; every call)
//   MMENTER <ms> mgr | mode | show_all | group A | group B | lr                          (8254E2E8, after; every call)
//   MMEXIT  <ms> mgr | lr                                                               (8254E370, entry)
#include "trace_common.h"

#include <mutex>
#include <string>

using namespace skate3_research;

namespace {
void Armed(const char* hook, PPCContext& ctx) {  // same helper as hooks_slowmo.cpp (b584)
  static std::mutex m;
  static std::string seen;
  std::lock_guard<std::mutex> lock(m);
  const std::string key = std::string(",") + hook + ",";
  if (seen.find(key) != std::string::npos) return;
  seen += key;
  rex::audio_trace::line("HOOKARMED", "%s\t%08X\t%08X\t%08X\t%08X", hook, static_cast<uint32_t>(ctx.lr), ctx.r3.u32,
                         ctx.r4.u32, ctx.r5.u32);
}
uint32_t Byte(uint8_t* base, uint32_t a) { return Readable(base, a, 1) ? base[a] : 0xFFu; }
}  // namespace

extern "C" REX_FUNC(sub_82552650) {  // minimap / challenge managers enter with the mode from 82552750
  const bool log = On("minimap");
  const uint32_t owner = ctx.r3.u32;
  char callers[64] = "-";
  if (log) {
    Armed("82552650", ctx);
    CallerChain(ctx, base, callers);
  }
  __imp__sub_82552650(ctx, base);
  if (log) {
    const uint32_t sys = TryU32(base, owner + 0x148u, 0);
    const uint32_t b = Plausible(sys) ? TryU32(base, sys + 0x1257Cu, 0) : 0;
    const uint32_t e = Plausible(sys) ? TryU32(base, sys + 0x12580u, 0) : 0;
    rex::audio_trace::line("MMMODE", "%08X\t%d\t%d\t%u\t%d\t%d\t%s", owner,
                           static_cast<int32_t>(TryU32(base, owner + 0x12Cu, 0xFFFFFFFFu)),
                           static_cast<int32_t>(TryU32(base, 0x830B7AE8u, 0xFFFFFFFFu)), Byte(base, 0x830B7C2Bu),
                           static_cast<int32_t>(TryU32(base, 0x830670CCu, 0xFFFFFFFFu)),
                           (e >= b && Plausible(b)) ? static_cast<int>((e - b) / 4) : -1, callers);
  }
}

extern "C" REX_FUNC(sub_8254E2E8) {  // MiniMapManager vt+0x0C enter(mode)
  const bool log = On("minimap");
  const uint32_t mgr = ctx.r3.u32, mode = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  if (log) Armed("8254E2E8", ctx);
  __imp__sub_8254E2E8(ctx, base);
  if (log) {
    rex::audio_trace::line("MMENTER", "%08X\t%u\t%u\t%08X\t%08X\t%08X", mgr, mode, Byte(base, mgr + 0x5FCu),
                           TryU32(base, mgr + 0x618u, 0), TryU32(base, mgr + 0x61Cu, 0), lr);
  }
}

extern "C" REX_FUNC(sub_8254E370) {  // MiniMapManager vt+0x10 exit
  if (On("minimap")) {
    Armed("8254E370", ctx);
    rex::audio_trace::line("MMEXIT", "%08X\t%08X", ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_8254E370(ctx, base);
}
