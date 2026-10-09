// Research hooks (tracing tools; not intended for the upstream project): the front end's font table lookup by
// APT font name. Shows which table row each requested name resolves to at run time (a match, the "debug" row
// fallback or row 0) and lists the live table rows.
// Category: SKATE3_TRACE=fonts (src/research/trace_common.h). Off = one table lookup per call.
//
// Lookup 82809208 (r3 = font table object, +24 row count; r4 = requested name). Rows of 36 bytes at 0x830680D0:
// +0 file name pointer, +4 APT name pointer, +32 loaded font (0 until first use). It compares the name against
// each row's +4 (case-insensitive), on a miss looks for the row whose +0 is "debug", else uses row 0, and returns
// that row's +32 (loading it first when 0).
//
// Kinds (fields after ms):
//   FONTLOOK <ms> lr | callers | table | name | result | index | how | row +0 | row +4    (9 fields; once per
//            distinct name, caller and result; how = HIT (name matched +4), DEBUG (row +0 is "debug"),
//            ROW0, or NONE (result is no row's +32))
//   FONTROW  <ms> count | index | row address | +0 | +4 | +32                            (6 fields; every row,
//            at the first lookup and again whenever the row count changes)
//   HOOKARMED <ms> hook                                                                  (first call)
#include "trace_common.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

using namespace skate3_research;

namespace {

constexpr const char* kCat = "fonts";
constexpr uint32_t kRows = 0x830680D0u;
constexpr uint32_t kRowSize = 36;
constexpr uint32_t kMaxRows = 32;

std::mutex g_mutex;  // lookups are rare outside loading; one lock keeps the state simple
bool g_armed = false;
int32_t g_dumped_count = -1;
std::unordered_set<std::string> g_seen;

void Str(uint8_t* base, uint32_t ptr_addr, char (&out)[33]) {
  const uint32_t p = Plausible(ptr_addr) ? TryU32(base, ptr_addr, 0) : 0;
  NameAt(base, p, out);
}

void DumpRows(uint8_t* base, int32_t count) {
  const int32_t n = count < 0 ? 0 : (count > (int32_t)kMaxRows ? (int32_t)kMaxRows : count);
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t row = kRows + (uint32_t)i * kRowSize;
    char file[33], apt[33];
    Str(base, row, file);
    Str(base, row + 4, apt);
    rex::audio_trace::line("FONTROW", "%d\t%d\t%08X\t%s\t%s\t%08X", count, i, row, file, apt,
                           TryU32(base, row + 32, 0xFFFFFFFFu));
  }
}

}  // namespace

extern "C" REX_FUNC(sub_82809208) {
  if (!On(kCat)) {
    __imp__sub_82809208(ctx, base);
    return;
  }
  const uint32_t table = ctx.r3.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  char name[33];
  NameAt(base, ctx.r4.u32, name);
  char callers[64];
  CallerChain(ctx, base, callers);

  __imp__sub_82809208(ctx, base);
  const uint32_t result = ctx.r3.u32;

  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_armed) {
    g_armed = true;
    rex::audio_trace::line("HOOKARMED", "82809208");
  }
  const int32_t count = (int32_t)TryU32(base, table + 24, 0);
  if (count != g_dumped_count) {
    g_dumped_count = count;
    DumpRows(base, count);
  }

  char key[96];
  std::snprintf(key, sizeof key, "%s|%08X|%08X", name, lr, result);
  if (!g_seen.insert(key).second) return;

  int32_t index = -1;
  const int32_t n = count < 0 ? 0 : (count > (int32_t)kMaxRows ? (int32_t)kMaxRows : count);
  for (int32_t i = 0; i < n; ++i) {
    if (TryU32(base, kRows + (uint32_t)i * kRowSize + 32, 0) == result && result != 0) {
      index = i;
      break;
    }
  }
  char file[33] = "-", apt[33] = "-";
  const char* how = "NONE";
  if (index >= 0) {
    const uint32_t row = kRows + (uint32_t)index * kRowSize;
    Str(base, row, file);
    Str(base, row + 4, apt);
    if (_stricmp(apt, name) == 0) {
      how = "HIT";
    } else if (_stricmp(file, "debug") == 0) {
      how = "DEBUG";
    } else if (index == 0) {
      how = "ROW0";
    }
  }
  rex::audio_trace::line("FONTLOOK", "%08X\t%s\t%08X\t%s\t%08X\t%d\t%s\t%s\t%s", lr, callers, table, name, result,
                         index, how, file, apt);
}
