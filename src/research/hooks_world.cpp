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

}  // namespace

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
  const uint32_t entered = ctx.r5.u32;
  const uint32_t exited = ctx.r6.u32;
  __imp__sub_82557600(ctx, base);
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
