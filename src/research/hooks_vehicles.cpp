// Research hooks (tracing tools; not intended for the upstream project): skitching, the skater-vs-car wipeout term,
// traffic-light phases and the junction query cars make. Categories: `skitch` (SKITCH, SKITCHCALL, SKITCHST,
// SKITCHCAND, VEHBAIL) and `traffic` (TRAFLIGHT2, TRAFPROG, VEHJUNC); see src/research/trace_common.h.
// VEHCONN (junction state / segment / lane changes per car) lives in hooks_traffic.cpp, next to VEHSTATE, because it
// shares the speed planner hook.
//
// Every hook keeps its hot path short: the category check, at most a few guarded reads and a compare; lines are
// written only on a change, on an event, or rate limited. Every read through a pointer goes through Readable / Try*.
//
//   HOOKARMED <ms> hook | lr | r3 | r4 | r5   (5 fields) once per hook, on its first call while its category is on:
//            proves the hook is installed and its function runs, even when its event never happens in a run.
//
// ---- Skitching (category `skitch`) ----
// Car side: the mover interface (vehicle+144, vtable 0x82322240) slot 128 sub_82C34648 writes bit 0x80 of +4403 from
// r4 & 1. The per-frame pass sub_82C34CD0 copies 0x80 into 0x40 and then clears 0x80 through the same slot (lr
// 0x82C34D70); it does the same for +4402 bit 0x02 -> 0x01 through slot 124 sub_82C34630. The only call that sets
// the bits is sub_82C361E8 (slot 292 of vtable 0x82322408): it looks a car up, calls slot 124 with 1 (+4402 bit 0x02,
// which adds +3684 to the speed cap in the integrator sub_82C3FF38) and, only when its r5 equals the id returned by
// [this+14992] (vfunc +4), slot 128 with 1 (lr 0x82C3627C).
//   SKITCH   <ms> event (GRAB | HELD | RELEASE) | latch (124 = +4402 bit 0x02, any skitcher incl. NPC skaters;
//            128 = +4403 bit 0x80, only the id-matched skater) | vehicle | +4402 | +4403 | speed +3412 | accel +3408 |
//            cap +3688 | +3680 | +3684 | car position x y z | car forward x y z | skater p | skater position x y z
//            (p+240, row 3 of p+192) | offset x y z (skater position minus car position, projected on the car
//            matrix rows 0 / 1 / 2) | held ms | asserts | pass gaps | caller this (r31) | caller id (r29) | callers
//            (21 fields). GRAB = first set call for a car that isn't held on that latch; HELD = while held, at most
//            every 250 ms; RELEASE = no set call for 300 ms (checked in the per-frame pass). "pass gaps" counts
//            passes where the car was not re-asserted since the previous pass. Skater p = of the skaters whose state
//            selector stood in or suggested state 104 (Skitching) within the last 300 ms, the one nearest the car
//            (see SKITCHST); 0 when none.
//   SKITCHCALL <ms> this | r4 (lookup key) | r5 (id) | [this+14992] | calls since last line | callers   (6 fields)
//            sub_82C361E8 on entry, on a change of r4 / r5 or at most every 500 ms.
// Skater side: the state selector CalcSuggestedState sub_82D8ADE8 (r3 = selector, r4 = current state, returns the
// suggested state; p = [selector+4]). Skitching (104) needs p+2476 bit 21 (hand) and p+2480 bit 22 (grab candidate
// published by sub_82D740F8); p+2476 bit 3 is the skitching flag the wipeout checks read; p+2592 the skitch value;
// p+2464 the interactable from candidate 3.
//   SKITCHST <ms> selector | p | current | suggested | why (1 bits changed, 2 transition around 104, 4 transition in
//            a vehicle-contact window) | p+2476 (hex) | p+2480 (hex) | p+2592 | p+2464 | selector+48 | position
//            x y z (p+240) | speed p+2656   (12 fields). Bits tracked: 2476 & 0x00200008, 2480 & 0x00700000. Bit
//            changes at most 10 per second per selector; transitions always.
//   SKITCHCAND <ms> S | p | c1 | S+196 | c2 | S+500 | c3 | [S+12768] | p+2480 after (hex) | S bytes 0..47 (hex)
//            (10 fields). The grab-candidate publisher sub_82D740F8 (r3 = candidate search S, r4 = p): c1 = S+288
//            && S+196 (copied to p+1888, bit 22), c2 = S+592 && S+500 (p+2176, bit 21), c3 = S+12780 && S+12772 > 0
//            (bit 20, p+2464 = [S+12768]); read before the call (it clears the flags). On a change of the tuple, at
//            most 10 per second per S.
// Skater vs car: the wipeout vehicle term sub_82D90C98 (r3 = wipeout object W; called by the ground check
// sub_82D8F9E0, lr 0x82D8FC20, and the air check sub_82D90358, lr 0x82D90428). SC = [[[W+8]+6448]+160] (the
// skeleton collision; the contact traversal sub_82BD4A30 keeps the step's largest vehicle-group (8) contact
// |dv . n| at SC+4056 with flag byte SC+4072, skater contacts at SC+4048 / 4052, group 11 at SC+4060). The term
// requests wipeout reason 7 (byte W+27, count W+200) when SC+4056 > limit, limit = settings +156 (skitch contact)
// while p+2476 bit 3, else +144 (vehicle contact); settings = [[[0x830CFDA4]+264]+4].
//   VEHBAIL  <ms> W | p | SC | where (G ground, A air, ? other) | force SC+4056 | limit used | skitching (p+2476 bit 3) |
//            vehicle contact +144 | skitch contact +156 | reason 7 before after | count W+200 | cooldown W+192 |
//            group-8 flag SC+4072 | skater force SC+4048 | group-11 force SC+4060 | p+2476 (hex)   (16 fields)
//            Logged when force > 0 or reason 7 changes; at most 10 per second per W, except a new reason 7.
//
// ---- Traffic lights and junctions (category `traffic`) ----
// The traffic-light manager update sub_826B2C18 ticks exactly four signal controllers (manager vtable +36 with
// index 0..3, loop register r30, lr 0x826B2E64) through sub_82E158D8: each controller C has a phase vector at C+4
// (16-byte records: kind, length, remaining, word) with the current index at C+292, and a second vector at C+148
// with index C+296 (the green phase split by the fraction C+300 when the phases are built, sub_82E156D8). One tick
// subtracts a fixed 1/60 s (0x820849C8) from the current record, so the light runs per call, not per second.
//   TRAFPROG <ms> index | C | n | phases ("kind:length:word,..."; up to 16) | n2 | second phases | C+300   (7 fields)
//            once per controller.
//   TRAFLIGHT2 <ms> index | C | phase | n | kind | length | remaining | phase2 | n2 | kind2 | length2 | C+300 |
//            ticks since the last line | ms since the last line   (14 fields) on every change of C+292 or C+296.
// The junction query sub_82E11E90 (r3 = road network object J, r5 = road segment record, 248 bytes; r6 = output,
// r7 = flag; returns 0..4, which the driving states store as the junction state +4392): the segment data [seg+4]
// holds the signal controller index at +84 (and +88 / +92 / +96); the light is consulted when [[J+4]+580] != 0.
// Callers: FollowingLane (lr 0x82C37C38, vehicle in r17) and sub_82C3B500 (lr 0x82C3B65C, vehicle in r31).
//   VEHJUNC  <ms> vehicle | caller (1 FollowingLane, 2 sub_82C3B500, 0 other) | J | seg | seg data | +84 | +88 | +92 |
//            +96 | lights ([[J+4]+580]) | r4 | r7 | result | output words 0..2 (hex) | light now ("phase kind" of
//            controller +84, "-" when unknown) | position x y z   (16 fields) per vehicle on a change of the result
//            or the segment.
#include "trace_common.h"

using namespace skate3_research;

namespace {

constexpr uint32_t kPassLr = 0x82C34D70;          // sub_82C34CD0 -> slot 128 (clear)
constexpr uint32_t kSkitchSetLr = 0x82C3627C;     // sub_82C361E8 -> slot 128 (set)
constexpr uint32_t kGroundCheckLr = 0x82D8FC20;   // sub_82D8F9E0 -> sub_82D90C98
constexpr uint32_t kAirCheckLr = 0x82D90428;      // sub_82D90358 -> sub_82D90C98
constexpr uint32_t kLightLoopLr = 0x826B2E64;     // sub_826B2C18 -> sub_82E158D8
constexpr uint32_t kFollowingLaneLr = 0x82C37C38; // sub_82C376E8 -> sub_82E11E90
constexpr uint32_t kJunction2Lr = 0x82C3B65C;     // sub_82C3B500 -> sub_82E11E90
constexpr uint32_t kSkitchState = 104;

void Armed(std::atomic<bool>& flag, const char* hook, uint32_t lr, uint32_t r3, uint32_t r4, uint32_t r5) {
  if (flag.load(std::memory_order_relaxed) || flag.exchange(true)) return;
  rex::audio_trace::line("HOOKARMED", "%s\t%08X\t%08X\t%08X\t%08X", hook, lr, r3, r4, r5);
}

// "x y z" of the matrix row at addr, and the three floats themselves.
bool Row(uint8_t* base, uint32_t addr, float (&v)[3]) {
  if (!Readable(base, addr, 12)) return false;
  for (int i = 0; i < 3; ++i) v[i] = LoadF32(base, addr + 4 * i);
  return true;
}

// ---- Skitch state ----
// Two latches: slot 124 (+4402 bit 0x02 -> 0x01; any skitcher, NPC skaters included) and slot 128 (+4403 bit 0x80 ->
// 0x40; only the skater whose id matches [this+14992]).
struct Held {
  uint64_t start = 0, last_assert = 0, last_line = 0;
  uint32_t asserts = 0, gaps = 0, caller_this = 0, caller_id = 0;
};
struct Latch {
  const char* name;
  uint32_t byte_offset;  // from the interface (vehicle+144)
  uint8_t latched_bit;   // the copy the pass makes
  uint32_t pass_lr, set_lr;
  uint32_t slot;
};
const Latch kLatch124 = {"124", 4258, 0x01, 0x82C34D48, 0x82C3623C, 124};
const Latch kLatch128 = {"128", 4259, 0x40, kPassLr, kSkitchSetLr, 128};
std::mutex g_skitch_mutex;
std::unordered_map<uint32_t, Held> g_held[2];
std::atomic<int> g_held_count[2];
std::unordered_map<uint32_t, uint64_t> g_skitchers;  // p -> last ms its selector stood in / suggested 104
std::atomic<bool> g_armed_pass{false}, g_armed_set{false}, g_armed_pass124{false}, g_armed_set124{false},
    g_armed_call{false}, g_armed_sel{false}, g_armed_cand{false}, g_armed_bail{false}, g_armed_light{false},
    g_armed_junc{false};

void SkitchLine(uint8_t* base, const char* event, const Latch& latch, uint32_t vehicle, const Held& h, uint64_t now,
                const char* callers) {
  if (!Readable(base, vehicle, 4404)) return;
  const uint32_t matrix = TryU32(base, vehicle + 164, 0) + 16;
  float r0[3] = {NAN, NAN, NAN}, r1[3] = {NAN, NAN, NAN}, r2[3] = {NAN, NAN, NAN}, cp[3] = {NAN, NAN, NAN};
  const bool have_car = Row(base, matrix, r0) && Row(base, matrix + 16, r1) && Row(base, matrix + 32, r2) &&
                        Row(base, matrix + 48, cp);
  // The skater: of the selectors seen in Skitching within the last 300 ms, the one nearest to this car.
  uint32_t p = 0;
  float sp[3] = {NAN, NAN, NAN};
  {
    std::lock_guard<std::mutex> lock(g_skitch_mutex);
    float best = INFINITY;
    for (auto it = g_skitchers.begin(); it != g_skitchers.end();) {
      if (now - it->second > 2000) {
        it = g_skitchers.erase(it);
        continue;
      }
      float v[3];
      if (now - it->second <= 300 && Row(base, it->first + 240, v)) {
        const float d = have_car ? (v[0] - cp[0]) * (v[0] - cp[0]) + (v[1] - cp[1]) * (v[1] - cp[1]) +
                                       (v[2] - cp[2]) * (v[2] - cp[2])
                                 : 0.0f;
        if (d < best) {
          best = d;
          p = it->first;
          sp[0] = v[0];
          sp[1] = v[1];
          sp[2] = v[2];
        }
      }
      ++it;
    }
  }
  float off[3] = {NAN, NAN, NAN};
  if (have_car && p != 0) {
    const float d[3] = {sp[0] - cp[0], sp[1] - cp[1], sp[2] - cp[2]};
    off[0] = d[0] * r0[0] + d[1] * r0[1] + d[2] * r0[2];
    off[1] = d[0] * r1[0] + d[1] * r1[1] + d[2] * r1[2];
    off[2] = d[0] * r2[0] + d[1] * r2[1] + d[2] * r2[2];
  }
  rex::audio_trace::line(
      "SKITCH",
      "%s\t%s\t%08X\t%02X\t%02X\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f %.3f %.3f\t%.4f %.4f %.4f\t%08X\t%.3f %.3f %.3f\t%.3f "
      "%.3f %.3f\t%llu\t%u\t%u\t%08X\t%08X\t%s",
      event, latch.name, vehicle, base[vehicle + 4402], base[vehicle + 4403], LoadF32(base, vehicle + 3412),
      LoadF32(base, vehicle + 3408), LoadF32(base, vehicle + 3688), LoadF32(base, vehicle + 3680),
      LoadF32(base, vehicle + 3684), cp[0], cp[1], cp[2], r2[0], r2[1], r2[2], p, sp[0], sp[1], sp[2], off[0], off[1],
      off[2], static_cast<unsigned long long>(now - h.start), h.asserts, h.gaps, h.caller_this, h.caller_id, callers);
}

// ---- Vehicle-contact windows per p (for SKITCHST's why 4) ----
std::mutex g_window_mutex;
std::unordered_map<uint32_t, uint64_t> g_window_until;

bool InWindow(uint32_t p, uint64_t now) {
  std::lock_guard<std::mutex> lock(g_window_mutex);
  auto it = g_window_until.find(p);
  return it != g_window_until.end() && now < it->second;
}

}  // namespace

// Mover interface slots 124 / 128: set or clear the skitch bits. Returns after calling `imp`.
namespace {
void LatchHook(PPCContext& ctx, uint8_t* base, const Latch& latch, std::atomic<bool>& armed_pass,
               std::atomic<bool>& armed_set, void (*imp)(PPCContext&, uint8_t*)) {
  const uint32_t iface = ctx.r3.u32, value = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  const int k = latch.slot == 124 ? 0 : 1;
  if (!On("skitch")) {
    imp(ctx, base);
    return;
  }
  if (lr == latch.pass_lr) {
    Armed(armed_pass, latch.slot == 124 ? "skitch_slot124_pass" : "skitch_slot128_pass", lr, iface, value, 0);
    if (g_held_count[k].load(std::memory_order_relaxed) == 0) {
      imp(ctx, base);
      return;
    }
    const uint8_t before = Readable(base, iface + latch.byte_offset, 1) ? base[iface + latch.byte_offset] : 0;
    imp(ctx, base);
    const uint32_t vehicle = iface - 144;
    const uint64_t now = GetTickCount64();
    Held copy;
    bool release = false;
    {
      std::lock_guard<std::mutex> lock(g_skitch_mutex);
      auto it = g_held[k].find(vehicle);
      if (it == g_held[k].end()) return;
      if ((before & latch.latched_bit) == 0) ++it->second.gaps;
      if (now - it->second.last_assert >= 300) {
        release = true;
        copy = it->second;
        g_held[k].erase(it);
        g_held_count[k].fetch_sub(1);
      }
    }
    if (release) SkitchLine(base, "RELEASE", latch, vehicle, copy, now, "-");
    return;
  }
  // Any other caller: the skitch setter sub_82C361E8 (re-asserts every update) or an unknown writer.
  Armed(armed_set, latch.slot == 124 ? "skitch_slot124_set" : "skitch_slot128_set", lr, iface, value, ctx.r29.u32);
  const uint32_t caller_this = lr == latch.set_lr ? ctx.r31.u32 : 0, caller_id = lr == latch.set_lr ? ctx.r29.u32 : 0;
  char callers[64] = "-";
  bool grab = false;
  {
    std::lock_guard<std::mutex> lock(g_skitch_mutex);
    grab = (value & 1) != 0 && g_held[k].find(iface - 144) == g_held[k].end();
  }
  if (grab) CallerChain(ctx, base, callers);
  imp(ctx, base);
  if ((value & 1) == 0) return;
  const uint32_t vehicle = iface - 144;
  const uint64_t now = GetTickCount64();
  Held copy;
  bool due = false;
  {
    std::lock_guard<std::mutex> lock(g_skitch_mutex);
    auto it = g_held[k].find(vehicle);
    if (it == g_held[k].end()) {
      Held h;
      h.start = now;
      h.last_line = now;
      it = g_held[k].emplace(vehicle, h).first;
      g_held_count[k].fetch_add(1);
      grab = true;
      due = true;
    } else if (now - it->second.last_line >= 250) {
      it->second.last_line = now;
      due = true;
    }
    it->second.last_assert = now;
    ++it->second.asserts;
    it->second.caller_this = caller_this;
    it->second.caller_id = caller_id;
    copy = it->second;
  }
  if (due) SkitchLine(base, grab ? "GRAB" : "HELD", latch, vehicle, copy, now, grab ? callers : "-");
}
}  // namespace

extern "C" REX_FUNC(sub_82C34630) { LatchHook(ctx, base, kLatch124, g_armed_pass124, g_armed_set124, __imp__sub_82C34630); }
extern "C" REX_FUNC(sub_82C34648) { LatchHook(ctx, base, kLatch128, g_armed_pass, g_armed_set, __imp__sub_82C34648); }

// The skitch setter (slot 292 of vtable 0x82322408).
namespace {
std::mutex g_call_mutex;
uint64_t g_call_last_ms = 0;
uint32_t g_call_last_r4 = 0, g_call_last_r5 = 0, g_call_count = 0;
}  // namespace

extern "C" REX_FUNC(sub_82C361E8) {
  const bool on = On("skitch");
  const uint32_t self = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  bool log = false;
  uint32_t count = 0;
  char callers[64] = "-";
  if (on) {
    Armed(g_armed_call, "skitch_setter", static_cast<uint32_t>(ctx.lr), self, r4, r5);
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_call_mutex);
    ++g_call_count;
    if (r4 != g_call_last_r4 || r5 != g_call_last_r5 || now - g_call_last_ms >= 500) {
      log = true;
      count = g_call_count;
      g_call_count = 0;
      g_call_last_ms = now;
      g_call_last_r4 = r4;
      g_call_last_r5 = r5;
    }
  }
  if (log) CallerChain(ctx, base, callers);
  const uint32_t handle = log ? TryU32(base, self + 14992, 0) : 0;
  __imp__sub_82C361E8(ctx, base);
  if (log) rex::audio_trace::line("SKITCHCALL", "%08X\t%08X\t%08X\t%08X\t%u\t%s", self, r4, r5, handle, count, callers);
}

// State selector CalcSuggestedState.
namespace {
struct SelLast {
  bool seen = false;
  uint32_t cur = 0, result = 0, bits = 0, value = 0;
  uint64_t second = 0;
  uint32_t lines = 0;
};
std::mutex g_sel_mutex;
std::unordered_map<uint32_t, SelLast> g_sel;
}  // namespace

extern "C" REX_FUNC(sub_82D8ADE8) {
  const uint32_t sel = ctx.r3.u32, cur = ctx.r4.u32;
  __imp__sub_82D8ADE8(ctx, base);
  if (!On("skitch")) return;
  const uint32_t result = ctx.r3.u32;
  Armed(g_armed_sel, "skitch_selector", static_cast<uint32_t>(ctx.lr), sel, cur, result);
  const uint32_t p = TryU32(base, sel + 4, 0);
  if (!Readable(base, p, 2672)) return;
  const uint32_t f2476 = LoadU32(base, p + 2476), f2480 = LoadU32(base, p + 2480), value = LoadU32(base, p + 2592);
  const uint32_t bits = (f2476 & 0x00200008u) | (f2480 & 0x00700000u);
  const bool skitchy = cur == kSkitchState || result == kSkitchState;
  const uint64_t now = GetTickCount64();
  if (skitchy) {
    std::lock_guard<std::mutex> lock(g_skitch_mutex);
    g_skitchers[p] = now;
  }
  uint32_t why = 0;
  {
    std::lock_guard<std::mutex> lock(g_sel_mutex);
    SelLast& last = g_sel[sel];
    const bool transition = result != cur && (result != last.result || cur != last.cur);
    if (last.seen && (bits != last.bits || value != last.value)) why |= 1;
    if (transition && skitchy) why |= 2;
    const uint64_t second = now / 1000;
    if (second != last.second) {
      last.second = second;
      last.lines = 0;
    }
    if (why == 1 && last.lines >= 10) why = 0;
    last.seen = true;
    last.cur = cur;
    last.result = result;
    last.bits = bits;
    last.value = value;
    if (why != 0) ++last.lines;
    if (transition && !skitchy) why |= 8;  // provisional: kept only inside a vehicle-contact window
  }
  if (why & 8) {
    why &= ~8u;
    if (InWindow(p, now)) why |= 4;
  }
  if (why == 0) return;
  char pos[64];
  Vec3Text(base, p + 240, pos, sizeof pos);
  rex::audio_trace::line("SKITCHST", "%08X\t%08X\t%u\t%u\t%u\t%08X\t%08X\t%u\t%08X\t%d\t%s\t%.3f", sel, p, cur, result,
                         why, f2476, f2480, value, LoadU32(base, p + 2464),
                         static_cast<int>(TryU32(base, sel + 48, 0xFFFFFFFFu)), pos, LoadF32(base, p + 2656));
}

// Grab-candidate publisher.
namespace {
struct CandLast {
  bool seen = false;
  uint32_t c = 0, o1 = 0, o2 = 0, o3 = 0;
  uint64_t second = 0;
  uint32_t lines = 0;
};
std::mutex g_cand_mutex;
std::unordered_map<uint32_t, CandLast> g_cand;
}  // namespace

extern "C" REX_FUNC(sub_82D740F8) {
  const uint32_t s = ctx.r3.u32, p = ctx.r4.u32;
  if (!On("skitch") || !Readable(base, s, 600)) {
    __imp__sub_82D740F8(ctx, base);
    return;
  }
  Armed(g_armed_cand, "skitch_candidates", static_cast<uint32_t>(ctx.lr), s, p, 0);
  const uint32_t o1 = LoadU32(base, s + 196), o2 = LoadU32(base, s + 500);
  const uint32_t c1 = (base[s + 288] != 0 && o1 != 0) ? 1 : 0, c2 = (base[s + 592] != 0 && o2 != 0) ? 1 : 0;
  uint32_t c3 = 0, o3 = 0;
  if (Readable(base, s + 12768, 16)) {
    c3 = (base[s + 12780] != 0 && static_cast<int32_t>(LoadU32(base, s + 12772)) > 0) ? 1 : 0;
    o3 = LoadU32(base, s + 12768);
  }
  char raw[97];
  HexAt(base, s, raw);
  __imp__sub_82D740F8(ctx, base);
  const uint32_t c = c1 | (c2 << 1) | (c3 << 2);
  {
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_cand_mutex);
    CandLast& last = g_cand[s];
    const bool changed = !last.seen || c != last.c || (c1 && o1 != last.o1) || (c2 && o2 != last.o2) ||
                         (c3 && o3 != last.o3);
    if (!changed) return;
    const uint64_t second = now / 1000;
    if (second != last.second) {
      last.second = second;
      last.lines = 0;
    }
    if (last.lines >= 10) return;
    ++last.lines;
    last.seen = true;
    last.c = c;
    last.o1 = o1;
    last.o2 = o2;
    last.o3 = o3;
  }
  rex::audio_trace::line("SKITCHCAND", "%08X\t%08X\t%u\t%08X\t%u\t%08X\t%u\t%08X\t%08X\t%s", s, p, c1, o1, c2, o2, c3, o3,
                         TryU32(base, p + 2480, 0xFFFFFFFFu), raw);
}

// Wipeout vehicle term.
namespace {
std::mutex g_bail_mutex;
std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> g_bail_rate;  // W -> (second, lines)
}  // namespace

extern "C" REX_FUNC(sub_82D90C98) {
  const uint32_t w = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  if (!On("skitch") || !Readable(base, w, 212)) {
    __imp__sub_82D90C98(ctx, base);
    return;
  }
  Armed(g_armed_bail, "vehicle_wipeout_term", lr, w, 0, 0);
  const uint8_t before = base[w + 27];
  __imp__sub_82D90C98(ctx, base);
  const uint8_t after = base[w + 27];
  const uint32_t a = TryU32(base, w + 8, 0);
  const uint32_t b = a ? TryU32(base, a + 6448, 0) : 0;
  const uint32_t sc = b ? TryU32(base, b + 160, 0) : 0;
  if (!Readable(base, sc + 4048, 28)) return;
  const float force = LoadF32(base, sc + 4056);
  const bool new_request = after != 0 && before == 0;
  if (!(force > 0.0f) && after == before) return;
  const uint64_t now = GetTickCount64();
  {
    std::lock_guard<std::mutex> lock(g_bail_mutex);
    auto& rate = g_bail_rate[w];
    const uint64_t second = now / 1000;
    if (rate.first != second) {
      rate.first = second;
      rate.second = 0;
    }
    if (!new_request && rate.second >= 10) return;
    ++rate.second;
  }
  const uint32_t p = TryU32(base, w + 16, 0);
  const uint32_t f2476 = TryU32(base, p + 2476, 0);
  if (force > 0.0f && Plausible(p)) {
    std::lock_guard<std::mutex> lock(g_window_mutex);
    g_window_until[p] = now + 3000;
  }
  const uint32_t settings = TryU32(base, TryU32(base, TryU32(base, 0x830CFDA4u, 0) + 264, 0) + 4, 0);
  const float vehicle_contact = TryF32(base, settings + 144), skitch_contact = TryF32(base, settings + 156);
  const bool skitching = (f2476 & 8) != 0;
  const char* where = lr == kGroundCheckLr ? "G" : lr == kAirCheckLr ? "A" : "?";
  rex::audio_trace::line("VEHBAIL", "%08X\t%08X\t%08X\t%s\t%.4f\t%.4f\t%d\t%.4f\t%.4f\t%u %u\t%u\t%.4f\t%u\t%.4f\t%.4f\t%08X", w,
                         p, sc, where, force, skitching ? skitch_contact : vehicle_contact, skitching ? 1 : 0,
                         vehicle_contact, skitch_contact, before, after, LoadU32(base, w + 200), LoadF32(base, w + 192),
                         base[sc + 4072], LoadF32(base, sc + 4048), LoadF32(base, sc + 4060), f2476);
}

// ---- Traffic lights ----
namespace {
struct Ctl {
  bool seen = false;
  int32_t phase = -2, phase2 = -2;
  uint32_t ticks = 0;
  uint64_t ms = 0;
};
std::mutex g_light_mutex;
std::unordered_map<uint32_t, Ctl> g_ctl;
std::atomic<uint32_t> g_ctl_by_index[4];

void PhaseList(uint8_t* base, uint32_t begin, uint32_t n, char* out, size_t size) {
  size_t used = 0;
  out[0] = 0;
  if (n == 0) {
    std::snprintf(out, size, "-");
    return;
  }
  for (uint32_t i = 0; i < n && i < 16 && used + 40 < size; ++i) {
    const uint32_t rec = begin + 16 * i;
    if (!Readable(base, rec, 16)) break;
    used += std::snprintf(out + used, size - used, "%s%u:%.3f:%u", i ? "," : "", LoadU32(base, rec),
                          LoadF32(base, rec + 4), LoadU32(base, rec + 12));
  }
}

bool PhaseRecord(uint8_t* base, uint32_t begin, uint32_t n, int32_t index, uint32_t& kind, float& length,
                 float& remaining) {
  if (index < 0 || static_cast<uint32_t>(index) >= n) return false;
  const uint32_t rec = begin + 16 * static_cast<uint32_t>(index);
  if (!Readable(base, rec, 16)) return false;
  kind = LoadU32(base, rec);
  length = LoadF32(base, rec + 4);
  remaining = LoadF32(base, rec + 8);
  return true;
}
}  // namespace

extern "C" REX_FUNC(sub_82E158D8) {
  const uint32_t c = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t index = lr == kLightLoopLr ? ctx.r30.u32 : 0xFFu;
  __imp__sub_82E158D8(ctx, base);
  if (!On("traffic") || !Readable(base, c, 304)) return;
  Armed(g_armed_light, "traffic_light_tick", lr, c, index, 0);
  if (index < 4) g_ctl_by_index[index].store(c, std::memory_order_relaxed);
  const int32_t phase = static_cast<int32_t>(LoadU32(base, c + 292)), phase2 = static_cast<int32_t>(LoadU32(base, c + 296));
  const uint64_t now = GetTickCount64();
  bool first = false;
  uint32_t ticks = 0;
  uint64_t since = 0;
  {
    std::lock_guard<std::mutex> lock(g_light_mutex);
    Ctl& ctl = g_ctl[c];
    ++ctl.ticks;
    if (ctl.seen && phase == ctl.phase && phase2 == ctl.phase2) return;
    first = !ctl.seen;
    ticks = ctl.ticks;
    since = ctl.seen ? now - ctl.ms : 0;
    ctl.seen = true;
    ctl.phase = phase;
    ctl.phase2 = phase2;
    ctl.ticks = 0;
    ctl.ms = now;
  }
  const uint32_t b1 = LoadU32(base, c + 4), e1 = LoadU32(base, c + 8);
  const uint32_t b2 = LoadU32(base, c + 148), e2 = LoadU32(base, c + 152);
  const uint32_t n1 = e1 >= b1 ? (e1 - b1) / 16 : 0, n2 = e2 >= b2 ? (e2 - b2) / 16 : 0;
  const float split = LoadF32(base, c + 300);
  if (first) {
    char l1[720], l2[720];
    PhaseList(base, b1, n1, l1, sizeof l1);
    PhaseList(base, b2, n2, l2, sizeof l2);
    rex::audio_trace::line("TRAFPROG", "%u\t%08X\t%u\t%s\t%u\t%s\t%.4f", index, c, n1, l1, n2, l2, split);
  }
  uint32_t k1 = 0xFFFFFFFFu, k2 = 0xFFFFFFFFu;
  float len1 = NAN, rem1 = NAN, len2 = NAN, rem2 = NAN;
  PhaseRecord(base, b1, n1, phase, k1, len1, rem1);
  PhaseRecord(base, b2, n2, phase2, k2, len2, rem2);
  rex::audio_trace::line("TRAFLIGHT2", "%u\t%08X\t%d\t%u\t%d\t%.3f\t%.3f\t%d\t%u\t%d\t%.3f\t%.4f\t%u\t%llu", index, c, phase,
                         n1, static_cast<int>(k1), len1, rem1, phase2, n2, static_cast<int>(k2), len2, split, ticks,
                         static_cast<unsigned long long>(since));
}

// ---- Junction query ----
namespace {
struct JuncLast {
  bool seen = false;
  uint32_t result = 0, seg = 0;
};
std::mutex g_junc_mutex;
std::unordered_map<uint32_t, JuncLast> g_junc;
}  // namespace

extern "C" REX_FUNC(sub_82E11E90) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t j = ctx.r3.u32, r4 = ctx.r4.u32, seg = ctx.r5.u32, out = ctx.r6.u32, flag = ctx.r7.u32;
  const uint32_t vehicle = lr == kFollowingLaneLr ? ctx.r17.u32 : lr == kJunction2Lr ? ctx.r31.u32 : 0;
  const int caller = lr == kFollowingLaneLr ? 1 : lr == kJunction2Lr ? 2 : 0;
  __imp__sub_82E11E90(ctx, base);
  if (!On("traffic")) return;
  const uint32_t result = ctx.r3.u32;
  Armed(g_armed_junc, "junction_query", lr, j, seg, result);
  {
    std::lock_guard<std::mutex> lock(g_junc_mutex);
    JuncLast& last = g_junc[vehicle ? vehicle : seg];
    if (last.seen && last.result == result && last.seg == seg) return;
    last.seen = true;
    last.result = result;
    last.seg = seg;
  }
  const uint32_t data = TryU32(base, seg + 4, 0);
  const uint32_t group = TryU32(base, data + 84);
  const uint32_t lights = TryU32(base, TryU32(base, j + 4, 0) + 580);
  char light[32] = "-";
  if (group < 4) {
    const uint32_t c = g_ctl_by_index[group].load(std::memory_order_relaxed);
    if (c && Readable(base, c, 304)) {
      const uint32_t b1 = LoadU32(base, c + 4), e1 = LoadU32(base, c + 8);
      const uint32_t n1 = e1 >= b1 ? (e1 - b1) / 16 : 0;
      const int32_t phase = static_cast<int32_t>(LoadU32(base, c + 292));
      uint32_t kind = 0xFFFFFFFFu;
      float len = NAN, rem = NAN;
      if (PhaseRecord(base, b1, n1, phase, kind, len, rem)) std::snprintf(light, sizeof light, "%d %d", phase, static_cast<int>(kind));
    }
  }
  char pos[64] = "nan nan nan";
  if (vehicle) Vec3Text(base, TryU32(base, vehicle + 164, 0) + 16 + 48, pos, sizeof pos);
  rex::audio_trace::line("VEHJUNC", "%08X\t%d\t%08X\t%08X\t%08X\t%d\t%d\t%d\t%d\t%d\t%08X\t%u\t%u\t%08X %08X %08X\t%s\t%s", vehicle,
                         caller, j, seg, data, static_cast<int>(group), static_cast<int>(TryU32(base, data + 88)),
                         static_cast<int>(TryU32(base, data + 92)), static_cast<int>(TryU32(base, data + 96)),
                         static_cast<int>(lights), r4, flag, result, TryU32(base, out), TryU32(base, out + 4),
                         TryU32(base, out + 8), light, pos);
}
