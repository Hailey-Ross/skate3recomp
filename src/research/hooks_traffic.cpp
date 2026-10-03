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
//   VEHAUD   <ms> obj | R | id | key | patch | rpm | pos x y z | fwd x y z | f144 | f148 | f152 | horn | skid |
//            rel176 | listener x y z   (15 fields; 2026-10-03)
//            TrafficEngine update sub_824D6478 (r3 = the engine object), read after it runs. R = [obj+28] = the
//            vehicle audio record that sub_824B2A28 fills from the 32-byte vehicle-audio entry: id = [[R+40]+64]
//            (the entry id it matched), key = R+168 (u64, hex), patch = obj+48 (after the random override), rpm =
//            obj+52, pos R+48, fwd R+112, R+144 / R+148 / R+152 floats, horn = R+156, skid = R+160, rel176 = R+176
//            (slewed relative speed), listener = *(0x830CFDD4)+0. Per object at most every 250 ms, and at once
//            when horn, skid, key or patch change. "-1" / "nan" where a read is not possible.
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

namespace {
struct VehAudLast {
  uint64_t ms = 0, key = 0;
  int32_t horn = -99, skid = -99, patch = -99;
};
std::mutex g_vehaud_mutex;
std::unordered_map<uint32_t, VehAudLast> g_vehaud_last;
}  // namespace

extern "C" REX_FUNC(sub_824D6478) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_824D6478(ctx, base);
  if (!On("traffic") || !Readable(base, obj, 56)) return;
  const uint32_t rec = LoadU32(base, obj + 28);
  if (!Readable(base, rec, 184)) return;
  const int32_t horn = static_cast<int32_t>(LoadU32(base, rec + 156));
  const int32_t skid = static_cast<int32_t>(LoadU32(base, rec + 160));
  const int32_t patch = static_cast<int32_t>(LoadU32(base, obj + 48));
  const uint64_t key = LoadU64(base, rec + 168);
  {
    std::lock_guard<std::mutex> lock(g_vehaud_mutex);
    VehAudLast& last = g_vehaud_last[obj];
    const uint64_t now = GetTickCount64();
    if (now - last.ms < 250 && horn == last.horn && skid == last.skid && patch == last.patch && key == last.key) return;
    last.ms = now;
    last.horn = horn;
    last.skid = skid;
    last.patch = patch;
    last.key = key;
  }
  const uint32_t link = TryU32(base, rec + 40, 0);
  const uint32_t id = TryU32(base, link + 64, 0xFFFFFFFFu);
  char pos[64], fwd[64], cam[64];
  Vec3Text(base, rec + 48, pos, sizeof pos);
  Vec3Text(base, rec + 112, fwd, sizeof fwd);
  Vec3Text(base, TryU32(base, 0x830CFDD4u, 0), cam, sizeof cam);
  rex::audio_trace::line("VEHAUD", "%08X\t%08X\t%08X\t%016llX\t%d\t%.1f\t%s\t%s\t%.4f\t%.4f\t%.4f\t%d\t%d\t%.4f\t%s", obj, rec, id,
                         static_cast<unsigned long long>(key), patch, LoadF32(base, obj + 52), pos, fwd,
                         LoadF32(base, rec + 144), LoadF32(base, rec + 148), LoadF32(base, rec + 152), horn, skid,
                         LoadF32(base, rec + 176), cam);
}

FIRST_PASS_HOOK(824D6BE8, "traffic", "TRAFHORN", "")
FIRST_PASS_HOOK(824D7440, "traffic", "TRAFSKID", "")
FIRST_PASS_HOOK(824D6110, "traffic", "TRAFENGINE", "")
FIRST_PASS_HOOK(826B1540, "traffic", "TRAFLIGHT", "")
FIRST_PASS_HOOK(82E156D8, "traffic", "TRAFPHASE", "")
FIRST_PASS_HOOK(826ABF98, "traffic", "PEDHONKED", "")
