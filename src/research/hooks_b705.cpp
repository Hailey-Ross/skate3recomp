// Research hooks (tracing tools; not intended for the upstream project): ped path-controller runtime values, the NPC
// skater bail outcome and the NPC skater line filter. Categories (armed only when named in SKATE3_TRACE, never by an
// unset list): pedmove, npcbail, npcline.
//
// pedmove
//   PEDSET   <ms> ctrl= agent= set= s00= s04= s08= s0C= s10= s14= s18= s1C= s2C= sA8= a144= a14C= c98= cA1= cE8= n=
//            (19) the integrate step sub_82EA0808: settings = *(*(ctrl+4)+4); on first sight of a ctrl and when the
//            settings pointer, agent+0x144, agent+0x14C or ctrl+0x98 changes (first 64 ctrls).
//   PEDMOVE  <ms> ctrl= n= dt= vin= a= vout= vmax= pos= su=   (10, with the ms field counted by trace.py as 9 after
//            the kind) per ctrl at most 5 Hz, first 12 ctrls; vmax = s00 * a144 * s04.
//   PEDAVLINK <ms> ctrl= agent= where= off= av=   (5) once per ctrl, 1 s after its first PEDSET: the first word of
//            ctrl+0..0x300 / agent+0..0x200 that holds an avoider seen by AVMASK.
//   PEDTURN  <ms> ctrl= n= f1= r5= r6= brk= v= out=   (8) path steering sub_82E9DF70 from the controller tick
//            (lr 0x82E9FFBC), per ctrl at most 10 Hz, first 12 ctrls.
//   AVMASK   <ms> av= m34= m38= w28= pair= other= p32= p80=   (8) the avoider pair step sub_82E97F30, per avoider on a
//            change of its (+0x34, +0x38) pair, first 64 avoiders.
// npcbail
//   NPCLATCH  <ms> ev= pc= sk= kind= cnt= dly= cd= f1D= t750=   (9) the bail latch sub_8246EF78: ev=seen once per pc,
//            rise / fall on the bail byte pc+0x39C, cd when the post-bail countdown pc+0x378 reaches 0.
//   NPCBAIL   <ms> ev= pc= sk= kind= cnt= dly= f1D= t750= cd= b25C= v240=   (11) the bail wait sub_8246EA90 (lr
//            0x8246F060): start, fade, t750, end, hb every 30 ticks; place (sub_8246E558, lr 0x8246ECE8) and reset
//            (sub_82468C10, lr 0x8246ED04 / 0x8246ED48) with the same fields.
//   NPCREMOVE <ms> mgr= sk= lr= pc= kind= f1D= cnt= t750= baild=   (9) every call of the removal sub_8245DCE0.
// npcline
//   NPCLINE   <ms> lr= filter= vt= target= line= f200= res= calls=   (8) the line test sub_82458860: every call with a
//            filter, else one in 64.
#include "trace_common.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

using namespace skate3_research;

namespace {

// Armed only when the category is named in SKATE3_TRACE (exact, comma separated); an unset list does not arm it.
bool Named(const char* category) {
  const char* list = std::getenv("SKATE3_TRACE");
  if (!rex::audio_trace::enabled() || list == nullptr) return false;
  return (std::string(",") + list + ",").find(std::string(",") + category + ",") != std::string::npos;
}
bool Ptr(uint32_t p) { return p != 0xFFFFFFFFu && Plausible(p); }
uint32_t U8(uint8_t* base, uint32_t a) { return Readable(base, a, 1) ? base[a] : 0xFFFFFFFFu; }
void V3(uint8_t* base, uint32_t a, char (&out)[64]) {
  std::snprintf(out, sizeof out, "%.3f,%.3f,%.3f", TryF32(base, a), TryF32(base, a + 4), TryF32(base, a + 8));
}

std::mutex g_mutex;

// pedmove state
struct PedSeen {
  uint32_t set = 0, agent = 0;
  float a144 = 0, a14C = 0, c98 = 0;
  uint64_t first = 0, last_move = 0, last_turn = 0;
  uint32_t n = 0, turn_n = 0;
  bool linked = false;
  bool detailed = false;
};
std::unordered_map<uint32_t, PedSeen> g_peds;
uint32_t g_detailed = 0;
std::unordered_set<uint32_t> g_avoiders;
struct AvSeen {
  uint32_t m34 = 0, m38 = 0;
};
std::unordered_map<uint32_t, AvSeen> g_av;

// npcbail state
struct BailSeen {
  uint32_t bail = 0, cd = 0, f1D = 0;
  float t750 = 0;
  bool bailed = false, cd_pending = false;
};
std::unordered_map<uint32_t, BailSeen> g_bail;

uint32_t Kind(uint8_t* base, uint32_t sk) {
  const uint32_t rec = Ptr(sk) ? TryU32(base, sk + 0x724u) : 0xFFFFFFFFu;
  return Ptr(rec) ? TryU32(base, rec + 0x9Cu) : 0xFFFFFFFFu;
}

void LinkScan(uint8_t* base, uint32_t ctrl, uint32_t agent) {
  // Caller holds g_mutex.
  struct Where {
    const char* name;
    uint32_t from, size;
  };
  const Where where[] = {{"ctrl", ctrl, 0x300u}, {"agent", agent, 0x200u}};
  for (const auto& w : where) {
    if (!Ptr(w.from)) continue;
    for (uint32_t off = 0; off < w.size; off += 4) {
      const uint32_t v = TryU32(base, w.from + off);
      if (g_avoiders.count(v)) {
        rex::audio_trace::line("PEDAVLINK", "ctrl=%08X\tagent=%08X\twhere=%s\toff=%X\tav=%08X", ctrl, agent, w.name, off, v);
        return;
      }
    }
  }
  rex::audio_trace::line("PEDAVLINK", "ctrl=%08X\tagent=%08X\twhere=none\toff=-\tav=-", ctrl, agent);
}
}  // namespace

// A1: the ped path controller's integrate step (velocity, acceleration, speed cap).
extern "C" REX_FUNC(sub_82EA0808) {
  static const bool on = Named("pedmove");
  if (!on) {
    __imp__sub_82EA0808(ctx, base);
    return;
  }
  const uint32_t ctrl = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  const uint32_t agent = Ptr(ctrl) ? TryU32(base, ctrl + 4u) : 0xFFFFFFFFu;
  const uint32_t set = Ptr(agent) ? TryU32(base, agent + 4u) : 0xFFFFFFFFu;
  if (!Ptr(agent) || !Ptr(set)) {
    __imp__sub_82EA0808(ctx, base);
    return;
  }
  char vin[64], acc[64];
  V3(base, ctrl + 0x14u, vin);
  V3(base, ctrl + 0x20u, acc);
  const uint32_t su = U8(base, ctrl + 0x105u);
  __imp__sub_82EA0808(ctx, base);
  const uint64_t now = GetTickCount64();
  const float a144 = TryF32(base, agent + 0x144u), a14C = TryF32(base, agent + 0x14Cu), c98 = TryF32(base, ctrl + 0x98u);
  bool log_set = false, log_move = false, link = false;
  uint32_t n = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_peds.find(ctrl);
    if (it == g_peds.end()) {
      if (g_peds.size() >= 64) return;
      PedSeen s;
      s.set = set;
      s.agent = agent;
      s.a144 = a144;
      s.a14C = a14C;
      s.c98 = c98;
      s.first = now;
      s.detailed = g_detailed < 12;
      if (s.detailed) ++g_detailed;
      it = g_peds.emplace(ctrl, s).first;
      log_set = true;
    }
    PedSeen& s = it->second;
    n = ++s.n;
    if (!log_set && (s.set != set || s.a144 != a144 || s.a14C != a14C || std::fabs(s.c98 - c98) > 1e-3f)) {
      s.set = set;
      s.a144 = a144;
      s.a14C = a14C;
      s.c98 = c98;
      log_set = true;
    }
    if (s.detailed && now - s.last_move >= 200) {
      s.last_move = now;
      log_move = true;
    }
    if (!s.linked && now - s.first >= 1000 && !g_avoiders.empty()) {
      s.linked = true;
      link = true;
      LinkScan(base, ctrl, agent);
    }
  }
  (void)link;
  const float s00 = TryF32(base, set), s04 = TryF32(base, set + 4u);
  if (log_set) {
    rex::audio_trace::line("PEDSET",
                           "ctrl=%08X\tagent=%08X\tset=%08X\ts00=%g\ts04=%g\ts08=%g\ts0C=%g\ts10=%g\ts14=%g\ts18=%g\ts1C=%g\ts2C=%g\tsA8=%08X\ta144=%g\ta14C=%g\tc98=%g\tcA1=%u\tcE8=%08X\tn=%u",
                           ctrl, agent, set, s00, s04, TryF32(base, set + 8u), TryF32(base, set + 0xCu), TryF32(base, set + 0x10u),
                           TryF32(base, set + 0x14u), TryF32(base, set + 0x18u), TryF32(base, set + 0x1Cu), TryF32(base, set + 0x2Cu),
                           TryU32(base, set + 0xA8u), a144, a14C, c98, U8(base, ctrl + 0xA1u), TryU32(base, ctrl + 0xE8u), n);
  }
  if (log_move) {
    char vout[64], pos[64];
    V3(base, ctrl + 0x14u, vout);
    V3(base, ctrl + 0x8Cu, pos);
    rex::audio_trace::line("PEDMOVE", "ctrl=%08X\tn=%u\tdt=%.5f\tvin=%s\ta=%s\tvout=%s\tvmax=%g\tpos=%s\tsu=%u", ctrl, n, dt, vin, acc, vout,
                           s00 * a144 * s04, pos, su);
  }
}

// A2: path steering from the controller tick.
extern "C" REX_FUNC(sub_82E9DF70) {
  static const bool on = Named("pedmove");
  if (!on || ctx.lr != 0x82E9FFBCu) {
    __imp__sub_82E9DF70(ctx, base);
    return;
  }
  const uint32_t out = ctx.r3.u32, ctrl = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, brk = ctx.r7.u32;
  const double f1 = ctx.f1.f64;
  char v[64];
  V3(base, ctrl + 0x14u, v);
  __imp__sub_82E9DF70(ctx, base);
  uint32_t n = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_peds.find(ctrl);
    if (it == g_peds.end() || !it->second.detailed) return;
    const uint64_t now = GetTickCount64();
    if (now - it->second.last_turn < 100) return;
    it->second.last_turn = now;
    n = ++it->second.turn_n;
  }
  char o[64];
  V3(base, out, o);
  rex::audio_trace::line("PEDTURN", "ctrl=%08X\tn=%u\tf1=%g\tr5=%08X\tr6=%08X\tbrk=%08X\tv=%s\tout=%s", ctrl, n, f1, r5, r6, brk, v, o);
}

// A3: the avoider pair step (mask words +0x34 / +0x38, weight +0x28, the pair's shares).
extern "C" REX_FUNC(sub_82E97F30) {
  static const bool on = Named("pedmove");
  if (!on) {
    __imp__sub_82E97F30(ctx, base);
    return;
  }
  const uint32_t pair = ctx.r3.u32;
  __imp__sub_82E97F30(ctx, base);
  if (!Ptr(pair)) return;
  const uint32_t a = TryU32(base, pair + 4u), b = TryU32(base, pair + 52u);
  if (!Ptr(a) || !Ptr(b)) return;
  const uint32_t av[2] = {a, b};
  for (int i = 0; i < 2; ++i) {
    const uint32_t self = av[i], other = av[1 - i];
    const uint32_t m34 = TryU32(base, self + 0x34u), m38 = TryU32(base, self + 0x38u);
    bool log = false;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      auto it = g_av.find(self);
      if (it == g_av.end()) {
        if (g_av.size() < 64) {
          g_av.emplace(self, AvSeen{m34, m38});
          g_avoiders.insert(self);
          log = true;
        }
      } else if (it->second.m34 != m34 || it->second.m38 != m38) {
        it->second = AvSeen{m34, m38};
        log = true;
      }
    }
    if (log) {
      rex::audio_trace::line("AVMASK", "av=%08X\tm34=%08X\tm38=%08X\tw28=%g\tpair=%08X\tother=%08X\tp32=%g\tp80=%g", self, m34, m38,
                             TryF32(base, self + 0x28u), pair, other, TryF32(base, pair + 32u), TryF32(base, pair + 80u));
    }
  }
}

// B1: the NPC skater bail latch.
extern "C" REX_FUNC(sub_8246EF78) {
  static const bool on = Named("npcbail");
  if (!on) {
    __imp__sub_8246EF78(ctx, base);
    return;
  }
  const uint32_t pc = ctx.r3.u32;
  __imp__sub_8246EF78(ctx, base);
  if (!Ptr(pc)) return;
  const uint32_t sk = TryU32(base, pc + 20u);
  const uint32_t bail = U8(base, pc + 0x39Cu), cd = TryU32(base, pc + 0x378u);
  const char* ev = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_bail.find(pc);
    if (it == g_bail.end()) {
      BailSeen s;
      s.bail = bail;
      s.cd = cd;
      g_bail.emplace(pc, s);
      ev = "seen";
    } else {
      BailSeen& s = it->second;
      if (bail != s.bail) {
        ev = bail ? "rise" : "fall";
        if (bail) s.bailed = true;
        else s.cd_pending = true;
        s.bail = bail;
      } else if (s.cd_pending && s.cd != 0 && cd == 0) {
        ev = "cd";
        s.cd_pending = false;
      }
      s.cd = cd;
    }
  }
  if (!ev) return;
  rex::audio_trace::line("NPCLATCH", "ev=%s\tpc=%08X\tsk=%08X\tkind=%08X\tcnt=%u\tdly=%g\tcd=%u\tf1D=%u\tt750=%g", ev, pc, sk, Kind(base, sk),
                         TryU32(base, pc + 0x37Cu), TryF32(base, pc + 0x374u), cd, U8(base, pc + 0x1Du), Ptr(sk) ? TryF32(base, sk + 0x750u) : NAN);
}

namespace {
void BailLine(uint8_t* base, const char* ev, uint32_t pc, uint32_t sk) {
  rex::audio_trace::line("NPCBAIL", "ev=%s\tpc=%08X\tsk=%08X\tkind=%08X\tcnt=%u\tdly=%g\tf1D=%u\tt750=%g\tcd=%u\tb25C=-\tv240=-", ev, pc, sk,
                         Kind(base, sk), TryU32(base, pc + 0x37Cu), TryF32(base, pc + 0x374u), U8(base, pc + 0x1Du),
                         Ptr(sk) ? TryF32(base, sk + 0x750u) : NAN, TryU32(base, pc + 0x378u));
}
}  // namespace

// B2: the bail wait (start, fade, end) from the latch.
extern "C" REX_FUNC(sub_8246EA90) {
  static const bool on = Named("npcbail");
  if (!on || ctx.lr != 0x8246F060u) {
    __imp__sub_8246EA90(ctx, base);
    return;
  }
  const uint32_t pc = ctx.r3.u32, sk = ctx.r4.u32;
  const uint32_t f1D0 = U8(base, pc + 0x1Du), cd0 = TryU32(base, pc + 0x378u);
  const float t0 = Ptr(sk) ? TryF32(base, sk + 0x750u) : NAN;
  __imp__sub_8246EA90(ctx, base);
  const uint32_t cnt = TryU32(base, pc + 0x37Cu), f1D = U8(base, pc + 0x1Du), cd = TryU32(base, pc + 0x378u);
  const float t = Ptr(sk) ? TryF32(base, sk + 0x750u) : NAN;
  if (cnt == 1) BailLine(base, "start", pc, sk);
  if (f1D0 == 0 && f1D == 1) BailLine(base, "fade", pc, sk);
  if (t0 > 1.0f && t <= 1.0f) BailLine(base, "t750", pc, sk);
  if (cd0 == 0 && cd != 0 && cd != 0xFFFFFFFFu) BailLine(base, "end", pc, sk);
  if (cnt != 0xFFFFFFFFu && cnt % 30 == 0) BailLine(base, "hb", pc, sk);
}

// B2: the respawn place and the reset inside the bail wait.
extern "C" REX_FUNC(sub_8246E558) {
  static const bool on = Named("npcbail");
  if (on && ctx.lr == 0x8246ECE8u) BailLine(base, "place", ctx.r3.u32, Ptr(ctx.r3.u32) ? TryU32(base, ctx.r3.u32 + 20u) : 0xFFFFFFFFu);
  __imp__sub_8246E558(ctx, base);
}

extern "C" REX_FUNC(sub_82468C10) {
  static const bool on = Named("npcbail");
  if (on && (ctx.lr == 0x8246ED04u || ctx.lr == 0x8246ED48u)) BailLine(base, "reset", ctx.r3.u32, Ptr(ctx.r3.u32) ? TryU32(base, ctx.r3.u32 + 20u) : 0xFFFFFFFFu);
  __imp__sub_82468C10(ctx, base);
}

// B3: every NPC skater removal.
extern "C" REX_FUNC(sub_8245DCE0) {
  static const bool on = Named("npcbail");
  if (!on) {
    __imp__sub_8245DCE0(ctx, base);
    return;
  }
  const uint32_t mgr = ctx.r3.u32, sk = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t rec = Ptr(sk) ? TryU32(base, sk + 0x724u) : 0xFFFFFFFFu;
  const uint32_t pc = Ptr(rec) ? TryU32(base, rec + 0x68u) : 0xFFFFFFFFu;
  uint32_t baild = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_bail.find(pc);
    baild = it != g_bail.end() && it->second.bailed ? 1 : 0;
  }
  rex::audio_trace::line("NPCREMOVE", "mgr=%08X\tsk=%08X\tlr=%08X\tpc=%08X\tkind=%08X\tf1D=%u\tcnt=%u\tt750=%g\tbaild=%u", mgr, sk, lr, pc,
                         Kind(base, sk), Ptr(pc) ? U8(base, pc + 0x1Du) : 0xFFFFFFFFu, Ptr(pc) ? TryU32(base, pc + 0x37Cu) : 0xFFFFFFFFu,
                         Ptr(sk) ? TryF32(base, sk + 0x750u) : NAN, baild);
  __imp__sub_8245DCE0(ctx, base);
}

// b707: the line test with the NPC skater profile's path filter (null filter = no id-based skip for that skater).
extern "C" REX_FUNC(sub_82458860) {
  static const bool on = Named("npcline");
  if (!on) {
    __imp__sub_82458860(ctx, base);
    return;
  }
  static uint32_t calls = 0;
  const uint32_t line = ctx.r3.u32, filter = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82458860(ctx, base);
  const uint32_t result = ctx.r3.u32;
  const uint32_t c = ++calls;
  if (filter == 0 && c % 64 != 1) return;
  const uint32_t vt = Ptr(filter) ? TryU32(base, filter) : 0;
  const uint32_t target = Ptr(vt) ? TryU32(base, vt + 4u) : 0;
  char id[40] = "-";
  if (Ptr(line) && Readable(base, line + 0x20u, 16)) {
    for (int i = 0; i < 16; ++i) std::snprintf(id + i * 2, 3, "%02X", base[line + 0x20u + i]);
  }
  const uint32_t flags = Ptr(line) ? TryU32(base, line + 0x44u) : 0;
  rex::audio_trace::line("NPCLINE", "lr=%08X\tfilter=%08X\tvt=%08X\ttarget=%08X\tline=%s\tf200=%u\tres=%08X\tcalls=%u", lr, filter, vt, target, id,
                         (flags & 0x200u) ? 1u : 0u, result, c);
}
