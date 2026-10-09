// Research hooks (tracing tools; not intended for the upstream project): living-world probes — pedestrian/vehicle
// population, mood reactions, security chases, NPC states, NPC stream blobs. Category: SKATE3_TRACE=npc
// (src/research/trace_common.h).
//
// First pass: argument meanings are only partly known, so every hook logs its raw arguments on entry and
// its return value after the call, plus the callers. Analysis of a run pins the meanings down; then the
// hooks get named fields. Each hook is rate limited (kFirstPassPerSecond lines per second, FIRST_PASS_HOOK in trace_common.h); a line's `skipped=N`
// counts calls not logged since the previous line of that kind.
//   <KIND> <ms> skipped=N | r3 r4 r5 r6 r7 r8 | f1 f2 | ret r3 | ret f1 | callers
// Kinds (function):
//   NPCSPAWN  ped ring spawn sub_826B9940 (f1/f2 ring radii, r7 initial-populate flag)
//   NPCSPAWNP ped placed spawn sub_826B90F8
//   NPCCULL   ped cull sub_826BA8B0 (f1 cull radius)
//   VEHSPAWN  vehicle spawn sub_826B9B90;  VEHCULL vehicle cull sub_826BAAB8
//   NPCMOOD   mood evaluator sub_82E41EB0;  NPCMOODR sub_82BFE010 (result pick)
//   NPCSEE    perception sub_82E26F38 (result r3)
//   NPCSPEED  suggested speed per state sub_8269F6A8 (ret f1)
//   NPCSTATE  state factories: InterceptChasee sub_826C75A8, Scatter sub_826C8618, PedestrianColliding
//             sub_826C5D90 (one kind, the function address is the first field)
//   SECCHASE  chase (generic, used by angry peds; Skate 3 has no active guards): escape sub_826AC668, rest sub_826AC898, sub_826AD690, sub_826AD3F8
//   PEDTIMER  per-ped timer set sub_82E40940, named (see the hook); PEDTIMERS = counts of periodic 25/26
//   NPCMOOD   (see above) plus `hit` lines: every positive mood result, unthrottled
//   SECTAKE   takedown: attempt sub_826A49D8, success sub_826A4E50, entry choice sub_82E3C000
//   SECTAZE   DrawTazer sub_826A8258, TazeWantTarget sub_826A8398
//   SECZONE   patrol zone: EntityOfInterestIsInZone sub_826AF310, ReturnToPatrolZone sub_826AEE80
//   AISTREAM  NPC blob stream functions sub_82C9B3A0 / sub_82C9B250 / sub_82C9B4F0
//   PEDAUD    (named, 21 fields; 2026-10-03) the pedestrian audio instance state, read after the PedestrianSFX
//             process sub_824D8078 (r3 = the object; active instances only):
//             obj | S = [obj+32] | id S+64 | list index | list count | flags "+68 +69 +71 +80" (bytes) |
//             feet "+73 +74" | model S+84 | model record key (u64 hex, *(0x830CFDDC) + (model + 14287) * 8) |
//             S+96 | "S+88 S+92 S+120 S+124" | S+132 | S+136 | materials "S+140 S+144" | S+148 | S+152 | S+156 |
//             O+128 (O = [obj+28]) | O+144 | O+48 x y z | listener *(0x830CFDD4)+0 x y z
//             list = the packed 20-byte entries at [G+0x2F070+56], count [G+0x2F070+60], G = *(0x83083C38),
//             matched by entry +12 == S+64 (-1 when not found). Per object at most every 500 ms, and at once
//             when the id, +68 or +136 change. "-1" / "nan" where a read is not possible.
#include "trace_common.h"

using namespace skate3_research;

#define NPC_HOOK(addr, kind, tag) FIRST_PASS_HOOK(addr, "npc", kind, tag)

NPC_HOOK(826B9940, "NPCSPAWN", "")
NPC_HOOK(826B90F8, "NPCSPAWNP", "")
NPC_HOOK(826BA8B0, "NPCCULL", "")
NPC_HOOK(826B9B90, "VEHSPAWN", "")
NPC_HOOK(826BAAB8, "VEHCULL", "")
// Mood evaluator: first-pass line (rate limited) for every call, but a positive result (r3 = 1) is always
// logged, so escalation steps are never lost to the limit.
extern "C" REX_FUNC(sub_82E41EB0) {
  static FirstPassLimit limit;
  const bool on = On("npc");
  const FirstPassEntry e = on ? FirstPassCapture(ctx, base) : FirstPassEntry{};
  const uint32_t r9 = ctx.r9.u32, r10 = ctx.r10.u32;
  __imp__sub_82E41EB0(ctx, base);
  if (!on) return;
  uint32_t skipped = 0;
  if (ctx.r3.u32 == 1) {
    FirstPassLog("NPCMOOD", "hit\t", 0, e, ctx);
    // Outputs (all pointers into the caller's frame, sub_82E41C7C call site): r4 = vector {begin, end, cap}
    // of results; r5, r6, r8, r9, r10 = result words; r7 = 4 words preset to -1. Logged raw to decode the
    // reaction (want enum, event, target) by correlation with screenshots.
    //   MOODOUT <ms> brain | r5 r6 r8 r9 r10 words | r7 4 words | results count | first 4 result words
    const uint32_t list = e.r[1];
    const uint32_t begin = TryU32(base, list, 0), end = TryU32(base, list + 4, 0);
    const uint32_t count = (begin && end >= begin && end - begin < 4096) ? (end - begin) / 4 : 0;
    const uint32_t r7 = e.r[4];
    rex::audio_trace::line(
        "MOODOUT", "%08X\t%08X %08X %08X %08X %08X\t%08X %08X %08X %08X\t%u\t%08X %08X %08X %08X", e.r[0],
        TryU32(base, e.r[2]), TryU32(base, e.r[3]), TryU32(base, e.r[5]), TryU32(base, r9), TryU32(base, r10),
        TryU32(base, r7), TryU32(base, r7 + 4), TryU32(base, r7 + 8), TryU32(base, r7 + 12), count,
        TryU32(base, begin), TryU32(base, begin + 4), TryU32(base, begin + 8), TryU32(base, begin + 12));
  } else if (FirstPassAllow(limit, skipped)) {
    FirstPassLog("NPCMOOD", "", skipped, e, ctx);
  }
}
NPC_HOOK(82BFE010, "NPCMOODR", "")

// Perception check (r3 = perception object = ped + 0x920, r4 -> target position vec4, f1 = range; ret r3 =
// seen). Logged per perception object at most every 250 ms.
//   PEDSEE <ms> perception | target x y z | range | seen
namespace {
std::mutex g_pos_mutex;
std::unordered_map<uint32_t, uint64_t> g_see_last, g_xyz_last;

// True at most every `ms` per key.
bool Every(std::unordered_map<uint32_t, uint64_t>& last, uint32_t key, uint64_t ms) {
  const uint64_t now = GetTickCount64();
  std::lock_guard<std::mutex> lock(g_pos_mutex);
  uint64_t& at = last[key];
  if (now - at < ms) return false;
  at = now;
  return true;
}
}  // namespace
extern "C" REX_FUNC(sub_82E26F38) {
  const bool on = On("npc");
  const uint32_t perception = ctx.r3.u32, target = ctx.r4.u32;
  const double range = ctx.f1.f64;
  __imp__sub_82E26F38(ctx, base);
  // Rate check first: this runs thousands of times a second, so the guarded read (VirtualQuery) only
  // happens for lines that are logged. The target vec lives in the caller's frame, unchanged by the call.
  if (on && Every(g_see_last, perception, 250)) {
    char t[64];
    Vec3Text(base, target, t, sizeof t);
    rex::audio_trace::line("PEDSEE", "%08X\t%s\t%.1f\t%u", perception, t, range, ctx.r3.u32 & 0xFF);
  }
}

// Ped position getter (vtable slot 9 of the component at owner + 188, used by the perception check):
// r3 -> output vec4, r4 = component; owner = r4 - 188 = ped - 0xB0 (ped = owner + 0xB0, slot = (ped - ped
// base) / 0x1780). Logged per owner at most every 250 ms: positions over time give chase speeds.
//   PEDXYZ <ms> ped | x y z
extern "C" REX_FUNC(sub_82E3D0D8) {
  const uint32_t out = ctx.r3.u32, ped = ctx.r4.u32 - 188 + 0xB0;
  __imp__sub_82E3D0D8(ctx, base);
  if (On("npc") && Every(g_xyz_last, ped, 250)) {
    char p[64];
    Vec3Text(base, out, p, sizeof p);
    rex::audio_trace::line("PEDXYZ", "%08X\t%s", ped, p);
  }
}
namespace {
struct PedAudLast {
  uint64_t ms = 0;
  uint32_t id = 0xFFFFFFFFu, speech = 0xFFFFFFFFu;
  int on = -1;
};
std::mutex g_pedaud_mutex;
std::unordered_map<uint32_t, PedAudLast> g_pedaud_last;
}  // namespace
extern "C" REX_FUNC(sub_824D8078) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_824D8078(ctx, base);
  if (!On("npc") || !Readable(base, obj, 36)) return;
  const uint32_t owner16 = LoadU32(base, obj + 16);
  if (!Readable(base, owner16, 53) || base[owner16 + 52] == 0) return;
  const uint32_t s = LoadU32(base, obj + 32);
  if (!Readable(base, s, 160)) return;
  const uint32_t id = LoadU32(base, s + 64), speech = LoadU32(base, s + 136);
  const int on = base[s + 68];
  {
    std::lock_guard<std::mutex> lock(g_pedaud_mutex);
    PedAudLast& last = g_pedaud_last[obj];
    const uint64_t now = GetTickCount64();
    if (now - last.ms < 500 && id == last.id && speech == last.speech && on == last.on) return;
    last.ms = now;
    last.id = id;
    last.speech = speech;
    last.on = on;
  }
  // The living-world ped audio list: the 20-byte entries the manager sub_824F2890 copies from.
  const uint32_t g = TryU32(base, 0x83083C38u, 0);
  const uint32_t list = g ? g + 0x2F070u : 0;
  const uint32_t entries = TryU32(base, list + 56, 0);
  const uint32_t count = TryU32(base, list + 60, 0);
  int index = -1;
  if (entries && count <= 256 && Readable(base, entries, count * 20 + 4)) {
    for (uint32_t i = 0; i < count; ++i) {
      if (LoadU32(base, entries + i * 20 + 12) == id) {
        index = static_cast<int>(i);
        break;
      }
    }
  }
  const uint32_t model = LoadU32(base, s + 84);
  const uint32_t table = TryU32(base, 0x830CFDDCu, 0);
  const uint64_t model_key = (table && model < 4096) ? TryU64(base, table + (model + 14287u) * 8u) : ~0ull;
  const uint32_t o = LoadU32(base, obj + 28);
  const bool o_ok = Readable(base, o, 148);
  char pos[64], cam[64];
  Vec3Text(base, o + 48, pos, sizeof pos);
  Vec3Text(base, TryU32(base, 0x830CFDD4u, 0), cam, sizeof cam);
  rex::audio_trace::line(
      "PEDAUD", "%08X\t%08X\t%08X\t%d\t%u\t%u %u %u %u\t%u %u\t%u\t%016llX\t%u\t%u %u %u %u\t%u\t%u\t%u %u\t%.4f\t%.4f\t%.4f\t%.4f\t%d\t%s\t%s",
      obj, s, id, index, count, base[s + 68], base[s + 69], base[s + 71], base[s + 80], base[s + 73], base[s + 74],
      model, static_cast<unsigned long long>(model_key), LoadU32(base, s + 96), LoadU32(base, s + 88),
      LoadU32(base, s + 92), LoadU32(base, s + 120), LoadU32(base, s + 124), LoadU32(base, s + 132), speech,
      LoadU32(base, s + 140), LoadU32(base, s + 144), LoadF32(base, s + 148), LoadF32(base, s + 152),
      LoadF32(base, s + 156), o_ok ? LoadF32(base, o + 128) : NAN,
      o_ok ? static_cast<int>(LoadU32(base, o + 144)) : -1, pos, cam);
}
NPC_HOOK(8269F6A8, "NPCSPEED", "")
NPC_HOOK(826C75A8, "NPCSTATE", "InterceptChasee\t")
NPC_HOOK(826C8618, "NPCSTATE", "Scatter\t")
NPC_HOOK(826C5D90, "NPCSTATE", "PedestrianColliding\t")
NPC_HOOK(826AC668, "SECCHASE", "escape\t")
NPC_HOOK(826AC898, "SECCHASE", "rest\t")
NPC_HOOK(826AD690, "SECCHASE", "826AD690\t")
NPC_HOOK(826AD3F8, "SECCHASE", "826AD3F8\t")
// Per-ped timer set (r3 = brain, r4 = index into the name list at 0x82064BCC, f1 = length). Named and never
// rate limited, except the two periodic timers every ped re-arms (25 PresenceCheckTimer 0.5 s, 26
// TrespassCheck 0.1 s), which are only counted (PEDTIMERS summary every 10 s).
//   PEDTIMER  <ms> brain | index | name | length | callers
//   PEDTIMERS <ms> presence sets | trespass sets   (since the previous summary)
namespace {
const char* const kTimerNames[] = {
    "NextWarnTimer", "InvestigateTimer", "ChaseSkaterOutViewTimer", "ChaseExhaustionTimer", "RestTimer",
    "OutOfViewTimer", "LookAtTimer", "AlertTimer", "TimedRangeRandom", "SpectatorBoredomTimer",
    "SpectatorCheerTimer", "AmbientBehaviourTimer", "CellPhoneTimer", "GatherTimer", "WanderWaitTimer",
    "PatrolZoneMaintainInterestTimer", "AttemptTakeDownTimeout", "TakeDownSuccessTimeout", "ReturnToPatrolZone",
    "ProRecTimer", "TargetConverse", "StartChase", "ForceWarnTimer", "WaitAfterCheer", "SitTimer",
    "PresenceCheckTimer", "TrespassCheck", "PatrolTimer", "IdleTimer", "TimeUntilICanBePrimaryChaserTimer",
    "ConversationSpeakingTimer", "HandPropActionTimer", "HandPropUsageTimer", "HandPropUsageTimer2",
    "ThrowHandPropTimer", "ThrowHandPropReactionTimer", "InterestTimer", "TazerIntoTime", "TazerWaitTime",
    "TazerPlayerDropTime", "TazerCycTime", "WanderRoadCheck", "TargetUnreachableTimer", "WanderTargetTime",
    "CollisionVolumeUnlock", "PluginTimeout", "ScatterTime", "UnspawnTime"};
std::mutex g_timer_mutex;
uint64_t g_timer_summary_ms = 0;
uint32_t g_presence_sets = 0, g_trespass_sets = 0;
}  // namespace
extern "C" REX_FUNC(sub_82E40940) {
  if (On("npc")) {
    const uint32_t index = ctx.r4.u32;
    if (index == 25 || index == 26) {
      std::lock_guard<std::mutex> lock(g_timer_mutex);
      (index == 25 ? g_presence_sets : g_trespass_sets)++;
      const uint64_t now = GetTickCount64();
      if (now - g_timer_summary_ms >= 10000) {
        rex::audio_trace::line("PEDTIMERS", "%u\t%u", g_presence_sets, g_trespass_sets);
        g_presence_sets = g_trespass_sets = 0;
        g_timer_summary_ms = now;
      }
    } else {
      char callers[64];
      CallerChain(ctx, base, callers);
      const char* name = index < sizeof kTimerNames / sizeof kTimerNames[0] ? kTimerNames[index] : "?";
      rex::audio_trace::line("PEDTIMER", "%08X\t%u\t%s\t%.4f\t%s", ctx.r3.u32, index, name, ctx.f1.f64, callers);
    }
  }
  __imp__sub_82E40940(ctx, base);
}
NPC_HOOK(826A49D8, "SECTAKE", "attempt\t")
NPC_HOOK(826A4E50, "SECTAKE", "success\t")
NPC_HOOK(82E3C000, "SECTAKE", "entry\t")
NPC_HOOK(826A8258, "SECTAZE", "draw\t")
NPC_HOOK(826A8398, "SECTAZE", "want\t")
NPC_HOOK(826AF310, "SECZONE", "in_zone\t")
NPC_HOOK(826AEE80, "SECZONE", "return\t")
NPC_HOOK(82C9B3A0, "AISTREAM", "82C9B3A0\t")
NPC_HOOK(82C9B250, "AISTREAM", "82C9B250\t")
NPC_HOOK(82C9B4F0, "AISTREAM", "82C9B4F0\t")
// PEDCOLL <ms> | ped | vtable | vtable+212 | brain | brain+3196 before | after | ped+2496 | ped+2500 | callers
// The pedestrian collision reaction setup sub_8269D8A8 (PedestrianColliding): ped+2496 / +2500 are the reaction
// kind and direction it leaves behind; vtable+212 is the virtual that decides them. One line per call (rare).
extern "C" REX_FUNC(sub_8269D8A8) {
  const bool on = On("npc");
  const uint32_t ped = ctx.r3.u32;
  uint32_t vt = 0, slot = 0, brain = 0, before = 0xFFFFFFFFu;
  char callers[64] = "-";
  if (on && Readable(base, ped, 5900)) {
    vt = LoadU32(base, ped);
    slot = Readable(base, vt + 212, 4) ? LoadU32(base, vt + 212) : 0;
    brain = LoadU32(base, ped + 5896);
    if (Readable(base, brain, 3200)) before = base[brain + 3196];
    CallerChain(ctx, base, callers);
  }
  __imp__sub_8269D8A8(ctx, base);
  if (!on || !Readable(base, ped, 5900)) return;
  const uint32_t after = Readable(base, brain, 3200) ? base[brain + 3196] : 0xFFFFFFFFu;
  rex::audio_trace::line("PEDCOLL", "%08X\t%08X\t%08X\t%08X\t%02X\t%02X\t%d\t%d\t%s", ped, vt, slot, brain,
                         before, after, static_cast<int>(LoadU32(base, ped + 2496)),
                         static_cast<int>(LoadU32(base, ped + 2500)), callers);
}
