// Research hooks (tracing tools; not intended for the upstream project): which character record (and so which
// natural stance) each skater actor uses. Category: SKATE3_TRACE=cas (src/research/trace_common.h).
//
// Background: the character settings getter sub_82590B50 takes a record index (r3) into the table at
// [0x83067060 + 8] (168 bytes per record). Byte +120 of the record == 0 means goofy (written to *r4 as 1),
// the word at +144 maps 1/2/3 -> 0/1/2 (written to *r6); it returns a skate style name. The skater actor
// constructor sub_82590DC0 receives the same index in r5 (stored at actor +1768), asks the table for the
// record's 64-bit id, turns that into a name (sub_824581C0) and builds the character with it (sub_82B973C8).
//
// Kinds (fields after ms; all rare, logged per actor or on change):
//   CASACTOR <ms> actor | r4 | index | r6 | r7 | r8 | character (+1804 after the ctor) | callers      (8 fields)
//   CASID    <ms> actor | index | record id (u64 hex) | name word | name                              (5 fields)
//   CASCHAR  <ms> actor | r5 | r6 | name pointer | name                                               (5 fields)
//   CASSET   <ms> index | lr | +120 | +121 | +124 | +144 | goofy out | style out | style name | callers (10 fields)
//            once per (index, lr) and again when any value changes for that pair
//   CASREC   <ms> index | record address | 168 bytes hex                                             (3 fields)
//            once per index
// "-1" / "-" where a read is not possible.
#include "trace_common.h"

#include <map>
#include <set>
#include <tuple>

using namespace skate3_research;

namespace {

constexpr uint32_t kCasTable = 0x83067060;
constexpr uint32_t kRecordSize = 168;
constexpr uint32_t kMaxIndex = 4096;

std::mutex g_cas_mutex;
std::set<uint32_t> g_rec_logged;
std::map<std::pair<uint32_t, uint32_t>, std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> g_set_seen;
uint32_t g_ctor_actor = 0;  // actor being constructed (the ctor runs on one thread at a time in practice)
uint32_t g_ctor_index = 0xFFFFFFFFu;

uint32_t RecordAddr(uint8_t* base, uint32_t index) {
  if (index >= kMaxIndex) return 0;
  const uint32_t table = TryU32(base, kCasTable + 8, 0);
  if (!Plausible(table)) return 0;
  const uint32_t rec = table + index * kRecordSize;
  return Readable(base, rec, kRecordSize) ? rec : 0;
}

void LogRecordOnce(uint8_t* base, uint32_t index) {
  {
    std::lock_guard<std::mutex> lock(g_cas_mutex);
    if (!g_rec_logged.insert(index).second) return;
  }
  const uint32_t rec = RecordAddr(base, index);
  char hex[kRecordSize * 2 + 1];
  if (rec) {
    for (uint32_t i = 0; i < kRecordSize; ++i) std::snprintf(hex + 2 * i, 3, "%02X", base[rec + i]);
  } else {
    std::snprintf(hex, sizeof hex, "-");
  }
  rex::audio_trace::line("CASREC", "%u\t%08X\t%s", index, rec, hex);
}

}  // namespace

// Skater actor constructor (r3 actor, r5 record index).
extern "C" REX_FUNC(sub_82590DC0) {
  const bool on = On("cas");
  const uint32_t actor = ctx.r3.u32, r4 = ctx.r4.u32, index = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32,
                 r8 = ctx.r8.u32;
  char callers[64] = "-";
  if (on) {
    CallerChain(ctx, base, callers);
    std::lock_guard<std::mutex> lock(g_cas_mutex);
    g_ctor_actor = actor;
    g_ctor_index = index;
  }
  __imp__sub_82590DC0(ctx, base);
  if (!on) return;
  rex::audio_trace::line("CASACTOR", "%08X\t%08X\t%u\t%08X\t%08X\t%08X\t%08X\t%s", actor, r4, index, r6, r7, r8,
                         TryU32(base, actor + 1804), callers);
  LogRecordOnce(base, index);
}

// Name lookup from the record id, only the call inside the actor constructor (lr 0x82591158).
// r3 = output {count?, pointer}, r4 = record id (u64).
extern "C" REX_FUNC(sub_824581C0) {
  if (static_cast<uint32_t>(ctx.lr) != 0x82591158u || !On("cas")) {
    __imp__sub_824581C0(ctx, base);
    return;
  }
  const uint32_t out = ctx.r3.u32;
  const uint64_t id = ctx.r4.u64;
  __imp__sub_824581C0(ctx, base);
  uint32_t actor, index;
  {
    std::lock_guard<std::mutex> lock(g_cas_mutex);
    actor = g_ctor_actor;
    index = g_ctor_index;
  }
  const uint32_t present = TryU32(base, out, 0xFFFFFFFFu);
  const uint32_t holder = TryU32(base, out + 4, 0);
  const uint32_t name_ptr = (present != 0 && present != 0xFFFFFFFFu && Plausible(holder)) ? TryU32(base, holder, 0) : 0;
  char name[33];
  NameAt(base, name_ptr, name);
  rex::audio_trace::line("CASID", "%08X\t%u\t%016llX\t%08X\t%s", actor, index, static_cast<unsigned long long>(id),
                         name_ptr, name);
}

// Character build from the actor constructor only (lr 0x825911E8): r4 actor, r7 name.
extern "C" REX_FUNC(sub_82B973C8) {
  if (static_cast<uint32_t>(ctx.lr) == 0x825911E8u && On("cas")) {
    char name[33];
    NameAt(base, ctx.r7.u32, name);
    rex::audio_trace::line("CASCHAR", "%08X\t%08X\t%08X\t%08X\t%s", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                           name);
  }
  __imp__sub_82B973C8(ctx, base);
}

// Character settings getter: r3 index, r4 -> goofy flag out, r5 -> settings out, r6 -> style out.
extern "C" REX_FUNC(sub_82590B50) {
  if (!On("cas")) {
    __imp__sub_82590B50(ctx, base);
    return;
  }
  const uint32_t index = ctx.r3.u32, out_goofy = ctx.r4.u32, out_style = ctx.r6.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  char callers[64];
  CallerChain(ctx, base, callers);
  __imp__sub_82590B50(ctx, base);
  const uint32_t ret = ctx.r3.u32;
  const uint32_t rec = RecordAddr(base, index);
  const uint32_t b120 = rec ? base[rec + 120] : 0xFFFFFFFFu;
  const uint32_t b121 = rec ? base[rec + 121] : 0xFFFFFFFFu;
  const uint32_t w124 = rec ? LoadU32(base, rec + 124) : 0xFFFFFFFFu;
  const uint32_t w144 = rec ? LoadU32(base, rec + 144) : 0xFFFFFFFFu;
  {
    std::lock_guard<std::mutex> lock(g_cas_mutex);
    auto key = std::make_pair(index, lr);
    auto value = std::make_tuple(b120, b121, w124, w144);
    auto it = g_set_seen.find(key);
    if (it != g_set_seen.end() && it->second == value) return;
    g_set_seen[key] = value;
  }
  char style[33];
  NameAt(base, ret, style);
  rex::audio_trace::line("CASSET", "%u\t%08X\t%d\t%d\t%08X\t%08X\t%d\t%d\t%s\t%s", index, lr, static_cast<int>(b120),
                         static_cast<int>(b121), w124, w144, static_cast<int>(TryU32(base, out_goofy)),
                         static_cast<int>(TryU32(base, out_style)), style, callers);
  LogRecordOnce(base, index);
}
