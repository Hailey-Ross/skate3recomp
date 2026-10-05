// Research hooks (tracing tools; not intended for the upstream project): world probes — trigger volumes (stream-in,
// registration per group, enter/exit), teleports and world-painter key changes. Category: SKATE3_TRACE=world
// (src/research/trace_common.h).
//
// Volume item (240 bytes): +64 AABB min, +80 AABB max, +176 link GUID, +184 instance id, +224 name pointer.
//   TRIGLOAD   <ms> set | count                                   (stream-in sub_82C9AFD8, r4+20 = set)
//   TRIGVOL    <ms> item | id | link | name | min x y z | max x y z (one per item of a loaded set)
//   TRIGUNLOAD <ms> set | count                                   (unload sub_82C9B070, r3+20 = set)
//   TRIGMGR    <ms> mgr | challenge group | stairs group | camera group (manager setup sub_82DD7AE0)
//   TRIGADD    <ms> group name | slot | id | name | callers         (AddVolume sub_82DD7668)
//   TRIGREM    <ms> group name | id | name | callers                (RemoveVolume sub_82DD7018)
//   TRIGENTER / TRIGEXIT <ms> group name | entity | id | name
//              (the set diff sub_82557600 called from group Update sub_82DD70E8 at 0x82DD728C; r5 = entered,
//               r6 = exited index vectors; item = group+80+4*index; entity = caller r31, one of up to 9
//               tracked entities: the player and others)
//   TELE       <ms> service | player | destination name | fade | flags r6 r7 r8 r9 r10 | callers
//              (TeleportPlayer sub_82D54C90; *r5 = destination name, hashed by sub_8296EA10)
//   TELEPLACE  <ms> player | callers                               (immediate placement sub_82D54EB0)
//   TELELOC    <ms> r4 | r5 | name(r6) | callers                   (ToWorldFileLocation path sub_82D55280)
//   WPKEY      <ms> layer | old key | new key | position x z       (broadcast layers, sub_82C0EA00 diff of
//              R+2080+16*i, 19 layers; r4 = 2D position (x, z))
//   TRIGQRY    <ms> group | entity | A x y z | B x y z | C x y z | centre x y z | axis x y z
//              (query cylinder builder sub_82DD80B8, called per tracked entity per group update; A/B/C = the points
//               the entity reports through its vtable +12 / +16 / +20, read from the builder's stack frame after
//               the call; centre = output transform +48, axis = output +32. Group "other" = the second caller
//               sub_828A80F8. At most 2 lines per second per group and entity.)
//   TRIGENT    <ms> entity | vtable | fn +4 | fn +8 | fn +12 | fn +16 | fn +20 | words +4..+28
//              (once per new entity vtable, from the same hook)
//   TRIGGRP    <ms> group name | id | item +216 (the word the trigger manager sub_82DD7C58 picks the group with:
//              1 Stairs, 2 Camera, otherwise Challenge)
//   HULLENTER / HULLEXIT <ms> manager | hull entry | hull key | position x y z
//              (challenge dynamic hull manager sub_82D519C0: its set diff sub_82557600 (lr 0x82D51A68) of the hull
//               regions around the query box 82DD8030 builds on the position; key = the hull's challenge record
//               key, *(*(*(entry+16))+24); position = the r4 vector the manager was called with)
//   HULLENT    <ms> manager | entity (sub_82D514C0, the entity whose position drives the hull manager; on change)
//   TELEFP     first pass (raw registers, FIRST_PASS_HOOK) on the other teleport candidates, to find the path
//              Challenge Map > Locations uses (it bypasses TeleportPlayer): tag = function. Flow::Teleport
//              commands sub_82864038/510/6E0/918, Flow::Teleport users sub_82898FC8 / sub_827A0490, Lua
//              bindings ToFELocation sub_82D5C808, ToLocationName sub_82D5CAD0, ToLocationID sub_82D5CBD8,
//              ToSessionMarker sub_82D5CA98, ToWorldFileLocation sub_82D5C8F0, sub_8284A950, and the stream
//              wait sub_82D55FA8.
#include "trace_common.h"

FIRST_PASS_HOOK(82864038, "world", "TELEFP", "flow_82864038\t")
FIRST_PASS_HOOK(82864510, "world", "TELEFP", "flow_82864510\t")
FIRST_PASS_HOOK(828646E0, "world", "TELEFP", "flow_828646E0\t")
FIRST_PASS_HOOK(82864918, "world", "TELEFP", "flow_82864918\t")
FIRST_PASS_HOOK(82898FC8, "world", "TELEFP", "flowuser_82898FC8\t")
FIRST_PASS_HOOK(827A0490, "world", "TELEFP", "flowuser_827A0490\t")
FIRST_PASS_HOOK(82D5C808, "world", "TELEFP", "lua_ToFELocation\t")
FIRST_PASS_HOOK(82D5CAD0, "world", "TELEFP", "lua_ToLocationName\t")
FIRST_PASS_HOOK(82D5CBD8, "world", "TELEFP", "lua_ToLocationID\t")
FIRST_PASS_HOOK(82D5CA98, "world", "TELEFP", "lua_ToSessionMarker\t")
FIRST_PASS_HOOK(82D5C8F0, "world", "TELEFP", "lua_ToWorldFileLocation\t")
FIRST_PASS_HOOK(8284A950, "world", "TELEFP", "8284A950\t")
FIRST_PASS_HOOK(82D55FA8, "world", "TELEFP", "streamwait_82D55FA8\t")

using namespace skate3_research;

namespace {

std::mutex g_mutex;
std::unordered_map<uint32_t, const char*> g_group_names;

const char* GroupName(uint32_t group) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_group_names.find(group);
  return it == g_group_names.end() ? "?" : it->second;
}

// Volume name up to 95 characters, cut before the editor path ("_0x…").
void VolumeName(uint8_t* base, uint32_t item, char (&out)[96]) {
  const uint32_t at = TryU32(base, item + 224, 0);
  int n = 0;
  for (; n < 95 && Readable(base, at + n, 1); ++n) {
    const uint8_t c = base[at + n];
    if (c == 0) break;
    if (c == '_' && Readable(base, at + n + 2, 1) && base[at + n + 1] == '0' && base[at + n + 2] == 'x') break;
    out[n] = (c >= 32 && c < 127 && c != '\t') ? static_cast<char>(c) : '?';
  }
  if (n == 0) out[n++] = '-';
  out[n] = 0;
}

unsigned long long Id(uint8_t* base, uint32_t item) {
  return static_cast<unsigned long long>(TryU64(base, item + 184));
}

void LogSet(uint8_t* base, const char* kind, uint32_t set, bool items) {
  const uint32_t count = TryU32(base, set + 4, 0);
  const uint32_t first = TryU32(base, set + 12, 0);
  rex::audio_trace::line(kind, "%08X\t%u", set, count);
  if (!items || count > 4096) return;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t item = first + i * 240;
    if (!Readable(base, item, 240)) break;
    char name[96], lo[64], hi[64];
    VolumeName(base, item, name);
    Vec3Text(base, item + 64, lo, sizeof lo);
    Vec3Text(base, item + 80, hi, sizeof hi);
    rex::audio_trace::line("TRIGVOL", "%08X\t%016llX\t%016llX\t%s\t%s\t%s", item, Id(base, item),
                           static_cast<unsigned long long>(TryU64(base, item + 176)), name, lo, hi);
  }
}

void LogIndices(PPCContext& ctx, uint8_t* base, const char* kind, uint32_t group, uint32_t vec) {
  const uint32_t begin = TryU32(base, vec, 0);
  const uint32_t end = TryU32(base, vec + 4, 0);
  if (!begin || end <= begin || end - begin > 4 * 256) return;
  for (uint32_t at = begin; at < end; at += 4) {
    const uint32_t index = TryU32(base, at, 0xFFFFFFFFu);
    if (index >= 256) continue;
    const uint32_t item = TryU32(base, group + 80 + 4 * index, 0);
    char name[96];
    VolumeName(base, item, name);
    rex::audio_trace::line(kind, "%s\t%08X\t%016llX\t%s", GroupName(group), ctx.r31.u32, Id(base, item), name);
  }
}

// Rate limit per (tag, entity): true at most once per `period_ms`.
bool QueryAllow(uint32_t tag, uint32_t entity, uint64_t period_ms) {
  struct Slot {
    uint32_t tag, entity;
    uint64_t last;
  };
  static Slot slots[64] = {};
  static uint32_t next = 0;
  const uint64_t now = GetTickCount64();
  std::lock_guard<std::mutex> lock(g_mutex);
  for (Slot& s : slots) {
    if (s.tag == tag && s.entity == entity && s.last != 0) {
      if (now - s.last < period_ms) return false;
      s.last = now;
      return true;
    }
  }
  slots[next++ % 64] = {tag, entity, now};
  return true;
}

bool NewVtable(uint32_t vtable) {
  static uint32_t seen[64] = {};
  static uint32_t count = 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    if (seen[i] == vtable) return false;
  }
  if (count < 64) seen[count++] = vtable;
  return true;
}

thread_local float t_hull_pos[3] = {NAN, NAN, NAN};

}  // namespace

// Query cylinder builder: r3 entity, r4 shape, r5 output transform. Its frame is 224 bytes; the entity's three
// points land at frame +128 (vtable +12), +144 (+16) and +112 (+20) and stay there after the call returns.
extern "C" REX_FUNC(sub_82DD80B8) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t entity = ctx.r3.u32;
  const uint32_t out = ctx.r5.u32;
  const uint32_t frame = ctx.r1.u32 - 224;
  __imp__sub_82DD80B8(ctx, base);
  if (lr != 0x82DD7200 && lr != 0x828A81E8) return;
  if (!On("world")) return;
  // r29 is the caller's group (non-volatile, restored by the builder) for the group update caller.
  const uint32_t tag = lr == 0x82DD7200 ? ctx.r29.u32 : 1u;
  if (!QueryAllow(tag, entity, 500)) return;
  char a[64], b[64], c[64], centre[64], axis[64];
  Vec3Text(base, frame + 128, a, sizeof a);
  Vec3Text(base, frame + 144, b, sizeof b);
  Vec3Text(base, frame + 112, c, sizeof c);
  Vec3Text(base, out + 48, centre, sizeof centre);
  Vec3Text(base, out + 32, axis, sizeof axis);
  rex::audio_trace::line("TRIGQRY", "%s\t%08X\t%s\t%s\t%s\t%s\t%s", lr == 0x82DD7200 ? GroupName(tag) : "other", entity,
                         a, b, c, centre, axis);
  const uint32_t vtable = TryU32(base, entity, 0);
  if (vtable && vtable != 0xFFFFFFFFu && NewVtable(vtable)) {
    rex::audio_trace::line("TRIGENT", "%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X %08X %08X %08X %08X %08X %08X",
                           entity, vtable, TryU32(base, vtable + 4), TryU32(base, vtable + 8), TryU32(base, vtable + 12),
                           TryU32(base, vtable + 16), TryU32(base, vtable + 20), TryU32(base, entity + 4),
                           TryU32(base, entity + 8), TryU32(base, entity + 12), TryU32(base, entity + 16),
                           TryU32(base, entity + 20), TryU32(base, entity + 24), TryU32(base, entity + 28));
  }
}

// Dynamic hull manager proximity update: r3 manager, r4 position vector.
extern "C" REX_FUNC(sub_82D519C0) {
  if (On("world")) {
    const uint32_t pos = ctx.r4.u32;
    t_hull_pos[0] = TryF32(base, pos);
    t_hull_pos[1] = TryF32(base, pos + 4);
    t_hull_pos[2] = TryF32(base, pos + 8);
  }
  __imp__sub_82D519C0(ctx, base);
}

// Dynamic hull manager update: r3 manager, r4 the entity whose position it uses.
extern "C" REX_FUNC(sub_82D514C0) {
  static uint32_t last = 0;
  const uint32_t entity = ctx.r4.u32;
  if (entity != last && On("world")) {
    last = entity;
    rex::audio_trace::line("HULLENT", "%08X\t%08X", ctx.r3.u32, entity);
  }
  __imp__sub_82D514C0(ctx, base);
}

extern "C" REX_FUNC(sub_82C9AFD8) {
  const uint32_t set = TryU32(base, ctx.r4.u32 + 20, 0);
  __imp__sub_82C9AFD8(ctx, base);
  if (On("world") && set) LogSet(base, "TRIGLOAD", set, true);
}

extern "C" REX_FUNC(sub_82C9B070) {
  if (On("world")) {
    const uint32_t set = TryU32(base, ctx.r3.u32 + 20, 0);
    if (set) LogSet(base, "TRIGUNLOAD", set, false);
  }
  __imp__sub_82C9B070(ctx, base);
}

extern "C" REX_FUNC(sub_82DD7AE0) {
  const uint32_t mgr = ctx.r3.u32;
  __imp__sub_82DD7AE0(ctx, base);
  const uint32_t groups[3] = {TryU32(base, mgr + 8376, 0), TryU32(base, mgr + 8380, 0), TryU32(base, mgr + 8384, 0)};
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    static const char* const kNames[3] = {"Challenge", "Stairs", "Camera"};
    for (int i = 0; i < 3; ++i) {
      if (groups[i]) g_group_names[groups[i]] = kNames[i];
    }
  }
  if (On("world")) {
    rex::audio_trace::line("TRIGMGR", "%08X\t%08X\t%08X\t%08X", mgr, groups[0], groups[1], groups[2]);
  }
}

extern "C" REX_FUNC(sub_82DD7668) {
  const uint32_t group = ctx.r3.u32;
  const uint32_t item = ctx.r4.u32;
  char callers[64];
  CallerChain(ctx, base, callers);
  __imp__sub_82DD7668(ctx, base);
  if (On("world")) {
    char name[96];
    VolumeName(base, item, name);
    rex::audio_trace::line("TRIGADD", "%s\t%u\t%016llX\t%s\t%s", GroupName(group), ctx.r3.u32, Id(base, item), name,
                           callers);
    rex::audio_trace::line("TRIGGRP", "%s\t%016llX\t%u", GroupName(group), Id(base, item), TryU32(base, item + 216));
  }
}

extern "C" REX_FUNC(sub_82DD7018) {
  if (On("world")) {
    char name[96], callers[64];
    VolumeName(base, ctx.r4.u32, name);
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("TRIGREM", "%s\t%016llX\t%s\t%s", GroupName(ctx.r3.u32), Id(base, ctx.r4.u32), name,
                           callers);
  }
  __imp__sub_82DD7018(ctx, base);
}

extern "C" REX_FUNC(sub_82557600) {
  const bool trigger = ctx.lr == 0x82DD7290;
  const bool hull = ctx.lr == 0x82D51A68;
  const uint32_t entered = ctx.r5.u32;
  const uint32_t exited = ctx.r6.u32;
  __imp__sub_82557600(ctx, base);
  if (hull && On("world")) {
    // r26 = the hull manager (caller's non-volatile register); entry = *(*(manager+12) + 4*(index+16)).
    const uint32_t mgr = ctx.r26.u32;
    const uint32_t entries = TryU32(base, mgr + 12, 0);
    const uint32_t vecs[2] = {entered, exited};
    for (int k = 0; k < 2; ++k) {
      const uint32_t begin = TryU32(base, vecs[k], 0);
      const uint32_t end = TryU32(base, vecs[k] + 4, 0);
      if (!entries || !begin || end <= begin || end - begin > 4 * 512) continue;
      for (uint32_t at = begin; at < end; at += 4) {
        const uint32_t index = TryU32(base, at, 0xFFFFFFFFu);
        if (index >= 4096) continue;
        const uint32_t entry = TryU32(base, entries + 4 * (index + 16), 0);
        const uint32_t record = TryU32(base, TryU32(base, entry + 16, 0), 0);
        const unsigned long long key = record ? static_cast<unsigned long long>(TryU64(base, record + 24, 0)) : 0ull;
        rex::audio_trace::line(k == 0 ? "HULLENTER" : "HULLEXIT", "%08X\t%08X\t%016llX\t%.3f %.3f %.3f", mgr, entry, key,
                               t_hull_pos[0], t_hull_pos[1], t_hull_pos[2]);
      }
    }
  }
  if (trigger && On("world")) {
    // r29 (group) and r31 (entity) are the caller's non-volatile registers, unchanged across the call.
    LogIndices(ctx, base, "TRIGENTER", ctx.r29.u32, entered);
    LogIndices(ctx, base, "TRIGEXIT", ctx.r29.u32, exited);
  }
}

extern "C" REX_FUNC(sub_82D54C90) {
  if (On("world")) {
    char name[33], callers[64];
    NameAt(base, TryU32(base, ctx.r5.u32, 0), name);
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("TELE", "%08X\t%08X\t%s\t%.3f\t%08X %08X %08X %08X %08X\t%s", ctx.r3.u32, ctx.r4.u32, name,
                           ctx.f1.f64, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, callers);
  }
  __imp__sub_82D54C90(ctx, base);
}

extern "C" REX_FUNC(sub_82D54EB0) {
  if (On("world")) {
    char callers[64];
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("TELEPLACE", "%08X\t%s", ctx.r4.u32, callers);
  }
  __imp__sub_82D54EB0(ctx, base);
}

extern "C" REX_FUNC(sub_82D55280) {
  if (On("world")) {
    char name[33], callers[64];
    NameAt(base, ctx.r6.u32, name);
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("TELELOC", "%08X\t%016llX\t%s\t%s", ctx.r4.u32, static_cast<unsigned long long>(ctx.r5.u64),
                           name, callers);
  }
  __imp__sub_82D55280(ctx, base);
}

extern "C" REX_FUNC(sub_82C0EA00) {
  constexpr int kLayers = 19;
  const uint32_t r = ctx.r3.u32;
  const uint32_t pos = ctx.r4.u32;
  const bool on = On("world") && Readable(base, r + 2080, 16 * kLayers);
  uint64_t before[kLayers] = {};
  if (on) {
    for (int i = 0; i < kLayers; ++i) before[i] = LoadU64(base, r + 2080 + 16 * i);
  }
  __imp__sub_82C0EA00(ctx, base);
  if (!on) return;
  char p[64];
  std::snprintf(p, sizeof p, "%.2f %.2f", TryF32(base, pos), TryF32(base, pos + 4));
  for (int i = 0; i < kLayers; ++i) {
    const uint64_t after = LoadU64(base, r + 2080 + 16 * i);
    if (after != before[i]) {
      rex::audio_trace::line("WPKEY", "%d\t%016llX\t%016llX\t%s", i, static_cast<unsigned long long>(before[i]),
                             static_cast<unsigned long long>(after), p);
    }
  }
}
