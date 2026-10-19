// Research hooks (tracing tools; not intended for the upstream project): how a pedestrian that stands still (for
// example against a wall) gets moving again. Category: SKATE3_TRACE=pedstuck (src/research/trace_common.h).
//
// The ped's NavPower bot keeps a still timer (sub_82E97118). When it runs out the bot queues an obstacle of its own
// size at its position (sub_82EA17E8, called with lr 0x82E9747C); the nav tick later cuts it into the mesh
// (sub_8292ADE8, called from sub_82EA11A8 with lr 0x82EA126C). The bot's path check (sub_82E95FD0, lr 0x82E975A8
// from the mode-1 update sub_82E974A8) then reports the path invalid and the bot asks for a new path to the same
// destination (sub_82E957F8, lr 0x82E975C8). When the ped moves again the obstacle is released (sub_82E939A0,
// r3 = bot + 0x390; lr 0x82E9735C from the still timer). The lines below show whether that chain fires.
//   STILLOBS <ms> bot (r31) | pos x y z (r5) | r6 x y z | r7 x y z | timer bot+0x390 | mode bot+0x230 | agent bot+0x248
//            | bot+0x258 | *(bot+0x258) | set (r4) | out handle after the call
//   NAVCUT   <ms> world (r3) | pos x y z (r4) | r5 x y z | r6 x y z | obstacle (return r3)
//   PATHOK   <ms> bot | result (r3 & 0xFF) | bot+0x250 | bot+0x254 | bot+0x258 | *(bot+0x258) | word at *(bot+0x258)+0x2C
//            (every invalid result; a valid one only for a bot with a STILLOBS in the last 3 s, at most every 100 ms)
//   PATHREQ  <ms> bot | caller (lr) | dest x y z (r4) | r6 | return r3 | calls skipped since the last line (per bot at
//            most every 250 ms)
//   STILLREL <ms> bot (r3 - 0x390) | caller (lr) | handle bot+0x394 before | after | timer bot+0x390
#include "trace_common.h"

#include <mutex>
#include <unordered_map>

using namespace skate3_research;

namespace {
std::mutex g_mutex;
std::unordered_map<uint32_t, uint64_t> g_still_at;   // bot -> time of its last STILLOBS
std::unordered_map<uint32_t, uint64_t> g_ok_last;    // bot -> last PATHOK line (valid results)
struct ReqLast {
  uint64_t at = 0;
  uint32_t skipped = 0;
};
std::unordered_map<uint32_t, ReqLast> g_req_last;

void Vec(uint8_t* base, uint32_t addr, char (&out)[64]) {
  if (addr == 0) {
    std::snprintf(out, sizeof out, "-");
    return;
  }
  Vec3Text(base, addr, out, sizeof out);
}
}  // namespace

// Still timer ran out: queue the ped's own obstacle.
extern "C" REX_FUNC(sub_82EA17E8) {
  if (!On("pedstuck") || ctx.lr != 0x82E9747Cu) {
    __imp__sub_82EA17E8(ctx, base);
    return;
  }
  const uint32_t bot = ctx.r31.u32, set = ctx.r4.u32, out = ctx.r3.u32;
  char pos[64], r6[64], r7[64];
  Vec(base, ctx.r5.u32, pos);
  Vec(base, ctx.r6.u32, r6);
  Vec(base, ctx.r7.u32, r7);
  const uint32_t handle = TryU32(base, bot + 0x258u);
  const uint32_t handle_word = (handle != 0xFFFFFFFFu && Plausible(handle)) ? TryU32(base, handle) : 0xFFFFFFFFu;
  const float timer = TryF32(base, bot + 0x390u);
  const uint32_t mode = TryU32(base, bot + 0x230u), agent = TryU32(base, bot + 0x248u);
  __imp__sub_82EA17E8(ctx, base);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_still_at[bot] = GetTickCount64();
  }
  rex::audio_trace::line("STILLOBS", "%08X\t%s\t%s\t%s\t%.4f\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X", bot, pos, r6, r7, timer,
                         mode, agent, handle, handle_word, set, TryU32(base, out));
}

// The nav tick cuts a queued obstacle into the mesh.
extern "C" REX_FUNC(sub_8292ADE8) {
  if (!On("pedstuck") || ctx.lr != 0x82EA126Cu) {
    __imp__sub_8292ADE8(ctx, base);
    return;
  }
  const uint32_t world = ctx.r3.u32;
  char pos[64], r5[64], r6[64];
  Vec(base, ctx.r4.u32, pos);
  Vec(base, ctx.r5.u32, r5);
  Vec(base, ctx.r6.u32, r6);
  __imp__sub_8292ADE8(ctx, base);
  rex::audio_trace::line("NAVCUT", "%08X\t%s\t%s\t%s\t%08X", world, pos, r5, r6, ctx.r3.u32);
}

// The mode-1 path check.
extern "C" REX_FUNC(sub_82E95FD0) {
  if (!On("pedstuck") || ctx.lr != 0x82E975A8u) {
    __imp__sub_82E95FD0(ctx, base);
    return;
  }
  const uint32_t bot = ctx.r3.u32;
  __imp__sub_82E95FD0(ctx, base);
  const uint32_t result = ctx.r3.u32 & 0xFFu;
  if (result != 0) {
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto still = g_still_at.find(bot);
    if (still == g_still_at.end() || now - still->second > 3000) return;
    uint64_t& last = g_ok_last[bot];
    if (now - last < 100) return;
    last = now;
  }
  const uint32_t handle = TryU32(base, bot + 0x258u);
  const bool live = handle != 0xFFFFFFFFu && Plausible(handle);
  const uint32_t object = live ? TryU32(base, handle) : 0xFFFFFFFFu;
  const uint32_t flags = (object != 0xFFFFFFFFu && Plausible(object)) ? TryU32(base, object + 0x2Cu) : 0xFFFFFFFFu;
  rex::audio_trace::line("PATHOK", "%08X\t%u\t%08X\t%08X\t%08X\t%08X\t%08X", bot, result, TryU32(base, bot + 0x250u),
                         TryU32(base, bot + 0x254u), handle, object, flags);
}

// A path request (any caller; the mode-1 re-request is lr 0x82E975C8).
extern "C" REX_FUNC(sub_82E957F8) {
  if (!On("pedstuck")) {
    __imp__sub_82E957F8(ctx, base);
    return;
  }
  const uint32_t bot = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr), r6 = ctx.r6.u32;
  char dest[64];
  Vec(base, ctx.r4.u32, dest);
  __imp__sub_82E957F8(ctx, base);
  uint32_t skipped = 0;
  {
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);
    ReqLast& last = g_req_last[bot];
    if (now - last.at < 250) {
      ++last.skipped;
      return;
    }
    skipped = last.skipped;
    last.at = now;
    last.skipped = 0;
  }
  rex::audio_trace::line("PATHREQ", "%08X\t%08X\t%s\t%08X\t%08X\t%u", bot, lr, dest, r6, ctx.r3.u32, skipped);
}

// The ped's obstacle is released (r3 = bot + 0x390).
extern "C" REX_FUNC(sub_82E939A0) {
  if (!On("pedstuck")) {
    __imp__sub_82E939A0(ctx, base);
    return;
  }
  const uint32_t holder = ctx.r3.u32, bot = holder - 0x390u, lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t before = TryU32(base, bot + 0x394u);
  __imp__sub_82E939A0(ctx, base);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_still_at.erase(bot);
  }
  rex::audio_trace::line("STILLREL", "%08X\t%08X\t%08X\t%08X\t%.4f", bot, lr, before, TryU32(base, bot + 0x394u),
                         TryF32(base, bot + 0x390u));
}
