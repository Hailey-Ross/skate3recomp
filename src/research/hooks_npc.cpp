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
