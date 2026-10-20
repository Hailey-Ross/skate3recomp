// Research hooks (tracing tools; not intended for the upstream project): the collision-type bit table the world's
// pair jobs filter body pairs with. Category: SKATE3_TRACE=pairtable (src/research/trace_common.h).
//
// The pair jobs sub_8277AB70 and sub_8277DBD0 take their input in r3. The table pointer is input+0x14 (0 = no
// filter); a pair is skipped when bit (width * typeA + typeB) is set, with the width at table+4 and the bit words
// from table+0xC; a body's type is its record +0x0C (the body group). Each distinct (job, table) is logged once, so a
// spawn with peds, NPC skaters and cars around gives the whole table.
//   PAIRTBL <ms> job | input | table | width | words | all bit words as one hex string (word 0 first)
//   PAIRIN  <ms> job | input | input+0x00 .. input+0x1C (hex), once per distinct input
#include "trace_common.h"

#include <mutex>
#include <set>
#include <utility>

using namespace skate3_research;

namespace {
std::mutex g_mutex;
std::set<std::pair<uint32_t, uint32_t>> g_tables;  // (job, table) already logged
std::set<std::pair<uint32_t, uint32_t>> g_inputs;  // (job, input) already logged

void LogJob(uint8_t* base, uint32_t job, uint32_t input) {
  if (input == 0 || !Plausible(input)) return;
  bool new_input = false, new_table = false;
  const uint32_t table = TryU32(base, input + 0x14u);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    new_input = g_inputs.insert({job, input}).second;
    new_table = g_tables.insert({job, table}).second;
  }
  if (new_input) {
    uint32_t w[8];
    for (uint32_t i = 0; i < 8; ++i) w[i] = TryU32(base, input + 4u * i);
    rex::audio_trace::line("PAIRIN", "%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X", job, input, w[0], w[1],
                           w[2], w[3], w[4], w[5], w[6], w[7]);
  }
  if (!new_table) return;
  if (table == 0 || table == 0xFFFFFFFFu || !Plausible(table)) {
    rex::audio_trace::line("PAIRTBL", "%08X\t%08X\t%08X\t-\t0\t-", job, input, table);
    return;
  }
  const uint32_t width = TryU32(base, table + 4u);
  const uint32_t bits = (width <= 64u) ? width * width : 0u;
  const uint32_t words = (bits + 31u) / 32u;
  char hex[1040] = "-";
  for (uint32_t i = 0; i < words && i < 128u; ++i) std::snprintf(hex + 8u * i, sizeof hex - 8u * i, "%08X", TryU32(base, table + 0xCu + 4u * i));
  rex::audio_trace::line("PAIRTBL", "%08X\t%08X\t%08X\t%u\t%u\t%s", job, input, table, width, words, hex);
}
}  // namespace

// Skater x Ped pair job (b342).
extern "C" REX_FUNC(sub_8277AB70) {
  if (On("pairtable")) LogJob(base, 0x8277AB70u, ctx.r3.u32);
  __imp__sub_8277AB70(ctx, base);
}

// The pair job of the same shape at 8277DBD0 (b342).
extern "C" REX_FUNC(sub_8277DBD0) {
  if (On("pairtable")) LogJob(base, 0x8277DBD0u, ctx.r3.u32);
  __imp__sub_8277DBD0(ctx, base);
}
