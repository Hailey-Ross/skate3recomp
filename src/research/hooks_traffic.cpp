// Research hooks (tracing tools; not intended for the upstream project): traffic probes — vehicle driving state, horns,
// skids, traffic lights. Category: SKATE3_TRACE=traffic (src/research/trace_common.h).
//
//   VEHSTATE <ms> vehicle | target speed
//            +3408 | manoeuvre +4396 (1 lane change, 2 overtake, 3 pull over) | target lane +4380 | flags
//            +3424 +4403 | alarm +3716 | planner f1 f2
//            (speed planner sub_82C3FA08, r3 = vehicle; per vehicle at most every 250 ms, and on any change of
//            manoeuvre / target lane) | position x y z | forward x y z (world matrix *(vehicle+164)+16)
//   VEHVT    <ms> vehicle | component +144 vtable | slots +36 +56 +88 (once per vtable: position getter search)
//   TRAFHORN / TRAFSKID / TRAFENGINE / TRAFLIGHT / TRAFPHASE / PEDHONKED: first pass (FIRST_PASS_HOOK):
//            horn SFXObj sub_824D6BE8, skids sub_824D7440, engine sub_824D6110, lights sub_826B1540, light phase
//            timer sub_82E156D8 (f1 = phase length), ped IsBeingHonkedAt sub_826ABF98.
#include "trace_common.h"

#include <unordered_set>

using namespace skate3_research;

namespace {
std::mutex g_mutex;
struct VehicleLast {
  uint64_t ms = 0;
  uint32_t manoeuvre = 0xFFFFFFFF, lane = 0xFFFFFFFF;
};
std::unordered_map<uint32_t, VehicleLast> g_last;
std::unordered_set<uint32_t> g_vtables;
}  // namespace

extern "C" REX_FUNC(sub_82C3FA08) {
  const uint32_t vehicle = ctx.r3.u32;
  const double f1 = ctx.f1.f64, f2 = ctx.f2.f64;
  __imp__sub_82C3FA08(ctx, base);
  if (!On("traffic") || !Readable(base, vehicle, 4404)) return;
  const uint32_t manoeuvre = LoadU32(base, vehicle + 4396), lane = LoadU32(base, vehicle + 4380);
  const uint32_t vtable = LoadU32(base, vehicle + 144);
  bool log = false, new_vtable = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    VehicleLast& last = g_last[vehicle];
    const uint64_t now = GetTickCount64();
    if (now - last.ms >= 250 || manoeuvre != last.manoeuvre || lane != last.lane) {
      log = true;
      last.ms = now;
      last.manoeuvre = manoeuvre;
      last.lane = lane;
    }
    new_vtable = g_vtables.insert(vtable).second;
  }
  if (new_vtable) {
    rex::audio_trace::line("VEHVT", "%08X\t%08X\t%08X %08X %08X", vehicle, vtable, TryU32(base, vtable + 36),
                           TryU32(base, vtable + 56), TryU32(base, vtable + 88));
  }
  if (log) {
    // World matrix: *(vehicle + 164) + 16 (the component's slot-9 getter sub_82C45CB0 -> sub_82C48248 copies
    // it); row 2 = forward, row 3 = position.
    const uint32_t matrix = TryU32(base, vehicle + 164, 0) + 16;
    char pos[64], fwd[64];
    Vec3Text(base, matrix + 48, pos, sizeof pos);
    Vec3Text(base, matrix + 32, fwd, sizeof fwd);
    rex::audio_trace::line("VEHSTATE", "%08X\t%.3f\t%u\t%u\t%02X %02X\t%.3f\t%.4f %.4f\t%s\t%s", vehicle,
                           LoadF32(base, vehicle + 3408), manoeuvre, lane, base[vehicle + 3424], base[vehicle + 4403],
                           LoadF32(base, vehicle + 3716), f1, f2, pos, fwd);
  }
}

FIRST_PASS_HOOK(824D6BE8, "traffic", "TRAFHORN", "")
FIRST_PASS_HOOK(824D7440, "traffic", "TRAFSKID", "")
FIRST_PASS_HOOK(824D6110, "traffic", "TRAFENGINE", "")
FIRST_PASS_HOOK(826B1540, "traffic", "TRAFLIGHT", "")
FIRST_PASS_HOOK(82E156D8, "traffic", "TRAFPHASE", "")
FIRST_PASS_HOOK(826ABF98, "traffic", "PEDHONKED", "")
