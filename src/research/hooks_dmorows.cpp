// Research hooks (tracing tools; not intended for the upstream project): one dynamic prop's contact rows per producer
// set, and its integrated state per physics step. Category: SKATE3_TRACE=physics, active only with
// SKATE3_DMOROWS_ID="<field hex>[#n]" (the n-th DMO whose DMO+0x1100 field matches, from 1; default the first) or "*#n"
// (the n-th DMO collision body made). DMO+0x1100 is NOT the template id (2026-10-22 run 3: per-object values).
//
// The prop: its collision body cb (made in 82C4E7B8, passed to 82E0E338 / 82E0E3E8, wrapped in hooks_props.cpp, which
// calls DmoRowsAssembly): cb+0x18 the physics assembly, cb+0x20 the DMO (template id at DMO+0x1100), cb+0x24 the world
// (0 while out of the simulation). Assembly: +0x08 parts (stride 0x60, the rigid body at part+0x4C while in the
// simulation), +0x1C part count, +0x20 joint count, +0x18 collision type.
// 82DC2FB8 (pre-step, entry): r3 = the world: [world] = the simulation S, [world+0x18] = the static world body.
// 8277B428 (stream append, worker threads): r3 = output triple (+0 records, +4 count address, +8 capacity, byte +0xD
// pool flag: 1 the job ran the ground reduction, 0 not; valid for the six return addresses in FlagCaller), r4 = source
// records (0x100 bytes; +0xC body A, +0x1C body B), r5 = count. 82AE10C8 (contact compiler, entry): r3 = S, records
// [S+0x10], count [S+0x64]; the compiler rewrites +0xC / +0x1C, so the solved rows are counted here. 82DC30D0 (dynamic
// update): [r3] = S (active list sentinel [S+0x28], next +0x2C, dt [S+0xA0]). Body: +0x00 quaternion, +0x10 position,
// +0x20 linear velocity, +0x30 angular velocity, +0x4C its S, +0x9C energy, +0xAC rest counter.
//   DMOROWSDMO <ms> cb | dmo | template | parts | joints | type | match   (every DMO collision body made)
//   DMOROWSARM <ms> cb | S | static | dt | bodies | pos of body 0 | nact
//   DMOROWS <ms> step | body 0 | G Gs GB | T Ts TB | O OB | RsA RsB | bodies | total | cap | inlist nact | pos | v | w |
//        quat | e9c | rest
//        (rows of any of the prop's bodies: G / T = appended by a ground / terrain job with it as body A, Gs / Ts of
//        those against the static body, GB / TB as body B, O / OB other callers, RsA / RsB at the compiler's entry;
//        the state is body 0's; one line per step while body 0 is in the active list, else 1 Hz)
//   DMOROWSSUM <ms> lines | dropped | steps   (1 Hz, cap 2000 lines a second)
#include "trace_common.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace skate3_research;

namespace {
struct Target {
  bool set = false;
  bool any = false;
  unsigned long long templ = 0;
  int nth = 1;
};

const Target& DmoTarget() {
  static const Target t = [] {
    Target v;
    const char* e = std::getenv("SKATE3_DMOROWS_ID");
    if (e && (*e == '*' || std::sscanf(e, "%llx", &v.templ) == 1)) {
      v.any = *e == '*';
      v.set = true;
      if (const char* h = std::strchr(e, '#')) v.nth = std::atoi(h + 1) > 0 ? std::atoi(h + 1) : 1;
    }
    return v;
  }();
  return t;
}

constexpr uint32_t kBodies = 16;
std::atomic<uint32_t> g_cb{0}, g_sim{0}, g_static{0}, g_seen{0}, g_bodies[kBodies] = {}, g_nbodies{0};
std::atomic<uint32_t> g_g{0}, g_gs{0}, g_gb{0}, g_t{0}, g_ts{0}, g_tb{0}, g_o{0}, g_ob{0};
std::atomic<uint32_t> g_rsa{0}, g_rsb{0};
std::atomic<uint64_t> g_win{0};
std::atomic<uint32_t> g_lines{0}, g_drop{0}, g_steps{0};
uint64_t g_step = 0, g_slow = 0;
bool g_armed_logged = false;

bool Allow() {
  const uint64_t now = GetTickCount64();
  uint64_t w = g_win.load();
  if (now - w >= 1000 && g_win.compare_exchange_strong(w, now)) {
    const uint32_t l = g_lines.exchange(0), d = g_drop.exchange(0), s = g_steps.exchange(0);
    if (l || d) rex::audio_trace::line("DMOROWSSUM", "%u\t%u\t%u", l, d, s);
  }
  if (g_lines.fetch_add(1) >= 2000) {
    g_drop.fetch_add(1);
    return false;
  }
  return true;
}

bool FlagCaller(uint32_t lr) {
  switch (lr) {
    case 0x8277C9A0u: case 0x8277CA44u: case 0x82779E80u: case 0x8277AB34u: case 0x8277B304u: case 0x8277B3ACu:
      return true;
    default:
      return false;
  }
}

bool Mine(uint32_t b) {
  if (!b) return false;
  const uint32_t n = g_nbodies.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < n && i < kBodies; ++i)
    if (g_bodies[i].load(std::memory_order_relaxed) == b) return true;
  return false;
}

void V3(uint8_t* base, uint32_t at, char* out, size_t size) {
  std::snprintf(out, size, "%.4f %.4f %.4f", TryF32(base, at), TryF32(base, at + 4), TryF32(base, at + 8));
}

unsigned long long TryU64(uint8_t* base, uint32_t at) {
  return Readable(base, at, 8) ? (static_cast<unsigned long long>(LoadU32(base, at)) << 32) | LoadU32(base, at + 4) : 0;
}
}  // namespace

namespace skate3_research {
// From hooks_props.cpp after 82E0E338 / 82E0E3E8 ran: cb = the collision body (the caller's r3).
void DmoRowsAssembly(uint8_t* base, uint32_t cb) {
  const Target& target = DmoTarget();
  if (!target.set || !On("physics") || !Plausible(cb)) return;
  const uint32_t assembly = TryU32(base, cb + 0x18, 0), dmo = TryU32(base, cb + 0x20, 0);
  const unsigned long long templ = Plausible(dmo) ? TryU64(base, dmo + 0x1100) : 0;
  const bool match = (target.any || templ == target.templ) && static_cast<int>(g_seen.fetch_add(1) + 1) == target.nth;
  if (match) g_cb = cb;
  rex::audio_trace::line("DMOROWSDMO", "%08X\t%08X\t%016llX\t%u\t%u\t%u\t%d", cb, dmo, templ,
                         Plausible(assembly) ? TryU32(base, assembly + 0x1C, 0) : 0,
                         Plausible(assembly) ? TryU32(base, assembly + 0x20, 0) : 0,
                         Plausible(assembly) ? TryU32(base, assembly + 0x18, 0) : 0, match ? 1 : 0);
}
}  // namespace skate3_research

extern "C" REX_FUNC(sub_82DC2FB8) {
  const uint32_t sim = g_sim.load();
  if (sim && DmoTarget().set && TryU32(base, ctx.r3.u32, 0) == sim) g_static = TryU32(base, ctx.r3.u32 + 0x18, 0);
  __imp__sub_82DC2FB8(ctx, base);
}

extern "C" REX_FUNC(sub_8277B428) {
  if (g_nbodies.load(std::memory_order_relaxed) && DmoTarget().set) {
    const uint32_t out = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32 < 64 ? ctx.r5.u32 : 64;
    const bool flagged = FlagCaller(static_cast<uint32_t>(ctx.lr));
    const bool ground = flagged && Readable(base, out + 0xD, 1) && base[out + 0xD] != 0;
    const uint32_t stat = g_static.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t rec = src + 0x100 * i;
      const uint32_t a = TryU32(base, rec + 0xC, 0), b = TryU32(base, rec + 0x1C, 0);
      if (Mine(a)) {
        if (!flagged) {
          g_o.fetch_add(1);
        } else if (ground) {
          g_g.fetch_add(1);
          if (stat && b == stat) g_gs.fetch_add(1);
        } else {
          g_t.fetch_add(1);
          if (stat && b == stat) g_ts.fetch_add(1);
        }
      } else if (Mine(b)) {
        (!flagged ? g_ob : ground ? g_gb : g_tb).fetch_add(1);
      }
    }
  }
  __imp__sub_8277B428(ctx, base);
}

extern "C" REX_FUNC(sub_82AE10C8) {
  if (g_nbodies.load(std::memory_order_relaxed) && DmoTarget().set && ctx.r3.u32 == g_sim.load()) {
    const uint32_t sim = ctx.r3.u32, recs = TryU32(base, sim + 0x10, 0), total = TryU32(base, sim + 0x64, 0),
                   cap = TryU32(base, sim + 0x7C, 0);
    const uint32_t scan = total < cap ? total : cap;
    uint32_t a = 0, b = 0;
    for (uint32_t i = 0; Plausible(recs) && i < scan && i < 4096; ++i) {
      if (Mine(TryU32(base, recs + 0x100 * i + 0xC, 0))) ++a;
      if (Mine(TryU32(base, recs + 0x100 * i + 0x1C, 0))) ++b;
    }
    g_rsa = a;
    g_rsb = b;
  }
  __imp__sub_82AE10C8(ctx, base);
}

extern "C" REX_FUNC(sub_82DC30D0) {
  const uint32_t arg = ctx.r3.u32;
  __imp__sub_82DC30D0(ctx, base);
  const uint32_t cb = g_cb.load();
  if (!cb || !DmoTarget().set || !On("physics")) return;
  const uint32_t sim = TryU32(base, arg, 0);
  if (!Plausible(sim) || !Plausible(TryU32(base, cb + 0x24, 0))) return;
  // The prop's bodies, read every step (a re-add allocates new ones); only bodies of this simulation count.
  const uint32_t assembly = TryU32(base, cb + 0x18, 0), parts = TryU32(base, assembly + 0x08, 0);
  const uint32_t count = TryU32(base, assembly + 0x1C, 0);
  uint32_t n = 0;
  for (uint32_t i = 0; Plausible(parts) && i < count && n < kBodies; ++i) {
    const uint32_t body = TryU32(base, parts + 0x60 * i + 0x4C, 0);
    if (Plausible(body) && TryU32(base, body + 0x4C, 0) == sim) g_bodies[n++] = body;
  }
  if (!n) return;
  g_nbodies = n;
  g_sim = sim;
  const uint32_t body = g_bodies[0].load();
  const uint32_t sentinel = TryU32(base, sim + 0x28, 0);
  uint32_t nact = 0;
  bool inlist = false;
  for (uint32_t b = TryU32(base, sentinel + 0x2C, 0); Plausible(sentinel) && Plausible(b) && b != sentinel && nact < 4096;
       b = TryU32(base, b + 0x2C, 0)) {
    ++nact;
    if (b == body) inlist = true;
  }
  char p[64], v[64], w[64];
  V3(base, body + 0x10, p, sizeof p);
  if (!g_armed_logged) {
    g_armed_logged = true;
    rex::audio_trace::line("DMOROWSARM", "%08X\t%08X\t%08X\t%.6g\t%u\t%s\t%u", cb, sim, g_static.load(),
                           TryF32(base, sim + 0xA0), n, p, nact);
  }
  ++g_step;
  g_steps.fetch_add(1);
  const uint32_t g = g_g.exchange(0), gs = g_gs.exchange(0), gb = g_gb.exchange(0), t = g_t.exchange(0),
                 ts = g_ts.exchange(0), tb = g_tb.exchange(0), o = g_o.exchange(0), ob = g_ob.exchange(0),
                 rsa = g_rsa.exchange(0), rsb = g_rsb.exchange(0);
  const uint64_t now = GetTickCount64();
  if (!inlist && now - g_slow < 1000) return;
  if (!inlist) g_slow = now;
  if (!Allow()) return;
  V3(base, body + 0x20, v, sizeof v);
  V3(base, body + 0x30, w, sizeof w);
  rex::audio_trace::line(
      "DMOROWS",
      "%llu\t%08X\t%u %u %u\t%u %u %u\t%u %u\t%u %u\t%u\t%u\t%u\t%d %u\t%s\t%s\t%s\t%.4f %.4f %.4f %.4f\t%.6g\t%u",
      static_cast<unsigned long long>(g_step), body, g, gs, gb, t, ts, tb, o, ob, rsa, rsb, n,
      TryU32(base, sim + 0x64, 0), TryU32(base, sim + 0x7C, 0), inlist ? 1 : 0, nact, p, v, w, TryF32(base, body),
      TryF32(base, body + 4), TryF32(base, body + 8), TryF32(base, body + 12), TryF32(base, body + 0x9C),
      TryU32(base, body + 0xAC, 0));
}
