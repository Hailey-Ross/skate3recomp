// Research hooks (tracing tools; not intended for the upstream project): where FE 62's MarqueeViewer puts the
// camera and the previewed character (b585). Category: SKATE3_TRACE=marquee.
//   MQCTRL  <ms> ctrl | lr | +0 xyz | +10 xyz | +20 | +24 | +28 | +2C | +30 | +38 | +34   (82546648, after, <=4/s)
//   MQCAM   <ms> ctrl | lr | ctrl+10 xyz | right | at | pos                               (82546500, after, <=4/s)
//   MQSET   <ms> cam | lr | pos | at | flag | callers               (82530C68, any caller, entry, <=4/s)
//   MQENTRY <ms> viewer | index | name | +60 | settings | settings+20 | entry+130 | entry+140 | callers (827DBC08)
//   MQACTOR <ms> actor | lr | +30 xyz | +40 | callers                                   (82546978, after)
//   MQPOSE  <ms> actor | +30 xyz | +40 | +B0 xyz | +80 xyz                             (82546EA0, after, <=4/s)
#include "trace_common.h"

#include <cstdio>
#include <mutex>
#include <string>

using namespace skate3_research;

namespace {
struct Limit {
  std::mutex mutex;
  uint64_t tick = 0;
  uint32_t lines = 0;
};
bool Allow(Limit& limit, uint32_t per_second) {  // as hooks_hud.cpp
  std::lock_guard<std::mutex> lock(limit.mutex);
  const uint64_t second = GetTickCount64() / 1000;
  if (second != limit.tick) {
    limit.tick = second;
    limit.lines = 0;
  }
  if (limit.lines >= per_second) return false;
  ++limit.lines;
  return true;
}
void Armed(const char* hook, PPCContext& ctx) {  // as hooks_slowmo.cpp (b584)
  static std::mutex m;
  static std::string seen;
  std::lock_guard<std::mutex> lock(m);
  const std::string key = std::string(",") + hook + ",";
  if (seen.find(key) != std::string::npos) return;
  seen += key;
  rex::audio_trace::line("HOOKARMED", "%s\t%08X\t%08X\t%08X\t%08X", hook, static_cast<uint32_t>(ctx.lr), ctx.r3.u32,
                         ctx.r4.u32, ctx.r5.u32);
}
struct V3 {
  char s[48];
};
V3 Vec(uint8_t* base, uint32_t a) {
  V3 v;
  std::snprintf(v.s, sizeof v.s, "%.3f %.3f %.3f", TryF32(base, a), TryF32(base, a + 4), TryF32(base, a + 8));
  return v;
}
uint32_t Byte(uint8_t* base, uint32_t a) { return Readable(base, a, 1) ? base[a] : 0xFFu; }
Limit g_ctrl, g_cam, g_set, g_pose;
}  // namespace

extern "C" REX_FUNC(sub_82546648) {  // MarqueeViewer_CameraCtrl update: +0 -> +10, build, push to the game camera
  const uint32_t ctrl = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  const bool log = On("marquee") && Allow(g_ctrl, 4);
  if (log) Armed("82546648", ctx);
  __imp__sub_82546648(ctx, base);
  if (log) {
    rex::audio_trace::line("MQCTRL", "%08X\t%08X\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%u", ctrl, lr,
                           Vec(base, ctrl).s, Vec(base, ctrl + 0x10).s, TryF32(base, ctrl + 0x20),
                           TryF32(base, ctrl + 0x24), TryF32(base, ctrl + 0x28), TryF32(base, ctrl + 0x2C),
                           TryF32(base, ctrl + 0x30), TryF32(base, ctrl + 0x38), Byte(base, ctrl + 0x34));
  }
}

extern "C" REX_FUNC(sub_82546500) {  // build camera matrix: r3 = out matrix, r4 = ctrl
  const uint32_t m = ctx.r3.u32, ctrl = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  const bool log = On("marquee") && Allow(g_cam, 4);
  if (log) Armed("82546500", ctx);
  __imp__sub_82546500(ctx, base);
  if (log) {
    rex::audio_trace::line("MQCAM", "%08X\t%08X\t%s\t%s\t%s\t%s", ctrl, lr, Vec(base, ctrl + 0x10).s, Vec(base, m).s,
                           Vec(base, m + 0x20).s, Vec(base, m + 0x30).s);
  }
}

extern "C" REX_FUNC(sub_82530C68) {  // game camera set matrix (r3 = camera, r4 = matrix)
  if (On("marquee")) {
    Armed("82530C68", ctx);  // any caller: boot-time proof
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    if (Allow(g_set, 4)) {  // every caller: the viewer's camera does not come through the callers b585 expected
      char callers[64];
      CallerChain(ctx, base, callers);
      rex::audio_trace::line("MQSET", "%08X\t%08X\t%s\t%s\t%u\t%s", ctx.r3.u32, lr, Vec(base, ctx.r4.u32 + 0x30).s,
                             Vec(base, ctx.r4.u32 + 0x20).s, Byte(base, ctx.r3.u32 + 0xF53u), callers);
    }
  }
  __imp__sub_82530C68(ctx, base);
}

extern "C" REX_FUNC(sub_827DBC08) {  // place preview character: r3 viewer, r4 name, r5 index
  const bool log = On("marquee");
  const uint32_t viewer = ctx.r3.u32, name = ctx.r4.u32, index = ctx.r5.u32;
  char callers[64] = "-";
  if (log) {
    Armed("827DBC08", ctx);
    CallerChain(ctx, base, callers);
  }
  const uint32_t pending = TryU32(base, viewer + 0x60u, 0);  // before: the call may consume it
  __imp__sub_827DBC08(ctx, base);
  if (log) {
    const uint32_t settings = TryU32(base, viewer + 0x3Cu, 0);
    const uint32_t first = Plausible(settings) ? TryU32(base, settings + 0x3Cu, 0) : 0;
    const uint32_t entry = Plausible(first) ? first + index * 0x150u : 0;
    rex::audio_trace::line("MQENTRY", "%08X\t%d\t%08X\t%08X\t%08X\t%s\t%s\t%.4f\t%s", viewer,
                           static_cast<int32_t>(index), name, pending, settings,
                           Plausible(settings) ? Vec(base, settings + 0x20).s : "nan nan nan",
                           entry ? Vec(base, entry + 0x130).s : "nan nan nan",
                           entry ? TryF32(base, entry + 0x140) : NAN, callers);
  }
}

extern "C" REX_FUNC(sub_82546978) {  // marquee actor ctor
  const bool log = On("marquee");
  const uint32_t actor = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  char callers[64] = "-";
  if (log) {
    Armed("82546978", ctx);
    CallerChain(ctx, base, callers);
  }
  __imp__sub_82546978(ctx, base);
  if (log) {
    rex::audio_trace::line("MQACTOR", "%08X\t%08X\t%s\t%.4f\t%s", actor, lr, Vec(base, actor + 0x30).s,
                           TryF32(base, actor + 0x40), callers);
  }
}

extern "C" REX_FUNC(sub_82546EA0) {  // marquee actor per-frame matrix
  const uint32_t actor = ctx.r3.u32;
  const bool log = On("marquee") && Allow(g_pose, 4);
  if (log) Armed("82546EA0", ctx);
  __imp__sub_82546EA0(ctx, base);
  if (log) {
    rex::audio_trace::line("MQPOSE", "%08X\t%s\t%.4f\t%s\t%s", actor, Vec(base, actor + 0x30).s,
                           TryF32(base, actor + 0x40), Vec(base, actor + 0xB0).s, Vec(base, actor + 0x80).s);
  }
}
