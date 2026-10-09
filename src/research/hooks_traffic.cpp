// Research hooks (tracing tools; not intended for the upstream project): traffic probes — vehicle driving state, horns,
// skids, traffic lights. Category: SKATE3_TRACE=traffic (src/research/trace_common.h).
//
//   VEHSTATE <ms> vehicle | target speed
//            +3408 | manoeuvre +4396 (1 lane change, 2 overtake, 3 pull over) | target lane +4380 | flags
//            +3424 +4403 | alarm +3716 | planner f1 f2
//            (speed planner sub_82C3FA08, r3 = vehicle; per vehicle at most every 250 ms, and on any change of
//            manoeuvre / target lane) | position x y z | forward x y z (world matrix *(vehicle+164)+16)
//   VEHCONN  <ms> vehicle | why (1 junction state, 2 segment index, 4 lane, 8 segment id, 16 first sight) |
//            junction state +4392 old new | segment index +4388 old new | lane +4376 old new | segment id +4144
//            (u64 hex) | +4136 (u64 hex) | target lane +4380 | manoeuvre +4396 | distance +3640 | speed +3412 |
//            accel +3408 | flags +4402 +4403 | position x y z | forward x y z   (17 fields; 2026-10-05)
//            From the same planner hook as VEHSTATE, logged only when one of the four values changes. The
//            junction state is the result of the junction query sub_82E11E90 that the driving states store; the
//            segment index is the next segment FollowingLane (sub_82C376E8) picks at a junction (scored or random),
//            i.e. the turn choice. Mover slots: +68 distance, +72 segment id, +76 segment index, +84 lane.
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
//   VEHHIT / VEHALARMSTOP / VEHPARK: the car alarm trigger (2026-10-04), see the end of the file.
#include "trace_common.h"

#include <unordered_set>

using namespace skate3_research;

namespace {
std::mutex g_mutex;
struct VehicleLast {
  uint64_t ms = 0;
  uint32_t manoeuvre = 0xFFFFFFFF, lane = 0xFFFFFFFF;
  // VEHCONN: junction state +4392, segment index +4388, current lane +4376, segment id +4144.
  bool seen = false;
  uint32_t junction = 0xFFFFFFFF, segment = 0xFFFFFFFF, cur_lane = 0xFFFFFFFF;
  uint64_t segment_id = ~0ull;
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
  const uint32_t junction = LoadU32(base, vehicle + 4392), segment = LoadU32(base, vehicle + 4388);
  const uint32_t cur_lane = LoadU32(base, vehicle + 4376);
  const uint64_t segment_id = LoadU64(base, vehicle + 4144);
  bool log = false, new_vtable = false;
  uint32_t conn_why = 0, old_junction = 0, old_segment = 0, old_lane = 0;
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
    if (!last.seen) conn_why |= 16;
    if (junction != last.junction) conn_why |= 1;
    if (segment != last.segment) conn_why |= 2;
    if (cur_lane != last.cur_lane) conn_why |= 4;
    if (segment_id != last.segment_id) conn_why |= 8;
    old_junction = last.junction;
    old_segment = last.segment;
    old_lane = last.cur_lane;
    last.seen = true;
    last.junction = junction;
    last.segment = segment;
    last.cur_lane = cur_lane;
    last.segment_id = segment_id;
    new_vtable = g_vtables.insert(vtable).second;
  }
  if (conn_why != 0) {
    const uint32_t matrix = TryU32(base, vehicle + 164, 0) + 16;
    char pos[64], fwd[64];
    Vec3Text(base, matrix + 48, pos, sizeof pos);
    Vec3Text(base, matrix + 32, fwd, sizeof fwd);
    rex::audio_trace::line("VEHCONN", "%08X\t%u\t%d %d\t%d %d\t%d %d\t%016llX\t%016llX\t%d\t%u\t%.3f\t%.3f\t%.3f\t%02X %02X\t%s\t%s",
                           vehicle, conn_why, static_cast<int>(old_junction), static_cast<int>(junction),
                           static_cast<int>(old_segment), static_cast<int>(segment), static_cast<int>(old_lane),
                           static_cast<int>(cur_lane), static_cast<unsigned long long>(segment_id),
                           static_cast<unsigned long long>(LoadU64(base, vehicle + 4136)), static_cast<int>(lane),
                           manoeuvre, LoadF32(base, vehicle + 3640), LoadF32(base, vehicle + 3412),
                           LoadF32(base, vehicle + 3408), base[vehicle + 4402], base[vehicle + 4403], pos, fwd);
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

// Car alarm trigger (2026-10-04, category `traffic`). The vehicle's collision callback sub_82C3C150 (slot +32 of the
// interface at vehicle+136, vtable 0x82322218): while the StayingParked flag (+3424 bit 0x80) is set and the length
// of the message's vector at msg+48 exceeds the vehicle_characteristics threshold (default 0.1), it sets the alarm
// flag (+3424 bit 0x10) and zeroes the alarm timer +3716 and the parked timer +3712. Logged per call, at most every
// 100 ms per vehicle unless the flags change:
//   VEHHIT   <ms> vehicle | flags +3424 before after | |v48| | v48 x y z | p32 x y z | msg+64 | msg+76 | other +32 |
//            alarm +3716 after | parked +3712 after | msg bytes 0..47 (hex) | caller chain ("-" for an
//            alarm-flag change inside the 100 ms gate). The hot path reads only the flag byte and the gate.
// StopAlarming's action sub_82C3B4E8 (clears bit 0x10) and StayingParked's begin / end (sub_82C39120 sets bit 0x80,
// sub_82C391F0 clears it), r4+4 = the vehicle:
//   VEHALARMSTOP <ms> vehicle | alarm +3716 | flags +3424 after
//   VEHPARK      <ms> vehicle | 1 enter / 0 exit | flags +3424 after | parked +3712 | alarm +3716
namespace {
std::mutex g_hit_mutex;
std::unordered_map<uint32_t, uint64_t> g_hit_last;
}  // namespace

extern "C" REX_FUNC(sub_82C3C150) {
  const uint32_t self = ctx.r3.u32, msg = ctx.r4.u32;
  const uint32_t vehicle = self - 136;
  // Cars touch the road every physics step, so this runs very often: the hot path only reads the
  // alarm flag and the per-car time gate. The guarded reads, the caller chain and the line itself
  // happen only for logged calls (an alarm-flag change, or at most one line per car per 100 ms).
  if (!On("traffic") || !Plausible(self) || !Plausible(vehicle + 3424)) {
    __imp__sub_82C3C150(ctx, base);
    return;
  }
  const uint64_t now = GetTickCount64();
  bool due;
  {
    std::lock_guard<std::mutex> lock(g_hit_mutex);
    due = now - g_hit_last[vehicle] >= 100;
  }
  const uint8_t before = base[vehicle + 3424];
  char callers[64] = "-";
  if (due) CallerChain(ctx, base, callers);
  __imp__sub_82C3C150(ctx, base);
  const uint8_t after = base[vehicle + 3424];
  if (after == before && !due) return;
  if (!Readable(base, vehicle, 4404) || !Readable(base, msg, 80)) return;
  const float alarm = LoadF32(base, vehicle + 3716), parked = LoadF32(base, vehicle + 3712);
  {
    std::lock_guard<std::mutex> lock(g_hit_mutex);
    g_hit_last[vehicle] = now;
  }
  const float x = LoadF32(base, msg + 48), y = LoadF32(base, msg + 52), z = LoadF32(base, msg + 56);
  char point[64], raw[97];
  Vec3Text(base, msg + 32, point, sizeof point);
  HexAt(base, msg, raw);
  const uint32_t other = LoadU32(base, msg + 76);
  rex::audio_trace::line("VEHHIT", "%08X\t%02X %02X\t%.4f\t%.4f %.4f %.4f\t%s\t%08X\t%08X\t%08X\t%.4f\t%.4f\t%s\t%s", vehicle,
                         before, after, std::sqrt(x * x + y * y + z * z), x, y, z, point, LoadU32(base, msg + 64), other,
                         Plausible(other) ? TryU32(base, other + 32, 0xFFFFFFFFu) : 0xFFFFFFFFu, alarm, parked, raw,
                         callers);
}

extern "C" REX_FUNC(sub_82C3B4E8) {
  const uint32_t vehicle = Readable(base, ctx.r4.u32, 8) ? LoadU32(base, ctx.r4.u32 + 4) : 0;
  __imp__sub_82C3B4E8(ctx, base);
  if (!On("traffic") || !Readable(base, vehicle, 4404)) return;
  rex::audio_trace::line("VEHALARMSTOP", "%08X\t%.4f\t%02X", vehicle, LoadF32(base, vehicle + 3716), base[vehicle + 3424]);
}

static void LogPark(uint8_t* base, uint32_t vehicle, int enter) {
  if (!On("traffic") || !Readable(base, vehicle, 4404)) return;
  rex::audio_trace::line("VEHPARK", "%08X\t%d\t%02X\t%.4f\t%.4f", vehicle, enter, base[vehicle + 3424],
                         LoadF32(base, vehicle + 3712), LoadF32(base, vehicle + 3716));
}

extern "C" REX_FUNC(sub_82C39120) {
  const uint32_t vehicle = Readable(base, ctx.r4.u32, 8) ? LoadU32(base, ctx.r4.u32 + 4) : 0;
  __imp__sub_82C39120(ctx, base);
  LogPark(base, vehicle, 1);
}

extern "C" REX_FUNC(sub_82C391F0) {
  const uint32_t vehicle = Readable(base, ctx.r4.u32, 8) ? LoadU32(base, ctx.r4.u32 + 4) : 0;
  __imp__sub_82C391F0(ctx, base);
  LogPark(base, vehicle, 0);
}
