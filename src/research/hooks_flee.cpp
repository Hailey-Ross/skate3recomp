// Research hooks (tracing tools; not intended for the upstream project): why peds scatter / flee around a pushed
// dynamic prop. Category: SKATE3_TRACE=flee (src/research/trace_common.h).
//
//   SCATTEST <ms> tag | mgr | ret | timer | trick | b71 | b70 | id6C | calls | lr   (10 fields)
//            The scatter test sub_826C1348 (r3 = mgr): ret = return byte (scatter on), timer = float mgr+0x2163C,
//            trick = the local player's PhysOut_Trick record (r9 after the call), b71 / b70 = its bytes +0x71 / +0x70,
//            id6C = its int +0x6C. tag "chg" when any of ret, trick, b71, b70, id6C or (timer > 0) changed, "hb" for
//            the 1 Hz heartbeat; calls = calls since the previous line. trick 0 / b71 ff when the test returned early.
//   XSRC     <ms> x | vtbl | t | id5984 | b5988 | b5989 | callers   (7 fields)
//            sub_82595FA8 entry (r3 = X, r4 = T; copies X+5984.. into T+10452..): on change per X (up to 16 X).
//   XVT      <ms> x_vtbl | words 0x823006A0..0x823006F4   (2 fields, once)
//   XWRITE   <ms> fn | x | before | after | callers   (5 fields)
//            An X method (r3 = X) after which "X+5984 X+5988 X+5989" differ from their values at entry.
//   FLEEBEGIN / SCATBEGIN <ms> r3 | r4 | ped | flags5936 | pos | callers   (6 fields)
//            Flee begin sub_826A3530 / Scatter begin sub_826A8E48; ped = [r4+4]; pos = [[ped+0x7E4]+0x80] x y z.
//            At most 20 lines a second each.
//   PEDLADD / PEDLREM   first-pass lines of sub_82C568D0 / sub_82C56990 (r3 = the DMO interface at DMO+0x90).
//   DMOLIST  <ms> dmo | count | entries | callers   (4 fields)
//            From the sub_82C54618 hook: the IPedestrian listener vector at DMO+0x1080, when its size changes.
//   PEDVF6C  <ms> dmo | entry | ped | ped_vtbl | target   (5 fields)
//            The ped vfunc +0x6C that sub_82C54618 calls per listener, once per distinct target (up to 16).
#include "trace_common.h"

using namespace skate3_research;

namespace {

bool Ptr(uint32_t p) { return p != 0xFFFFFFFFu && Plausible(p); }
// "flee" arms every group; fleescat / fleex / fleebeg / fleedmo arm one group each (bisecting a crash).
bool FleeOn(const char* group) { return On("flee") || On(group); }
uint32_t U8(uint8_t* base, uint32_t a) { return Readable(base, a, 1) ? base[a] : 0xFFFFFFFFu; }
uint32_t Follow(uint8_t* base, uint32_t ptr, uint32_t offset) {
  return Ptr(ptr) ? TryU32(base, ptr + offset) : 0xFFFFFFFFu;
}

// Same walk as hooks_props.cpp FindInterface (sub_82965630 without calling it).
uint32_t FindInterface(uint8_t* base, uint32_t map, uint32_t type) {
  const uint32_t begin = Follow(base, map, 0), end = Follow(base, map, 4);
  if (begin == 0xFFFFFFFFu || end == 0xFFFFFFFFu || end < begin || end - begin > 8 * 4096) return 0xFFFFFFFFu;
  for (uint32_t e = begin; e < end; e += 8) {
    if (TryU32(base, e) == type) return TryU32(base, e + 4);
  }
  return 0;
}

std::mutex g_scat_lock;
struct {
  uint32_t ret = 2, trick = 1, b71 = 0x100, b70 = 0x100, id = 0, calls = 0;
  bool timer_on = false;
  uint64_t hb_ms = 0;
} g_scat;

}  // namespace

extern "C" REX_FUNC(sub_826C1348) {
  static const bool on = FleeOn("fleescat");
  if (!on) {
    __imp__sub_826C1348(ctx, base);
    return;
  }
  const uint32_t mgr = ctx.r3.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_826C1348(ctx, base);
  const uint32_t ret = ctx.r3.u32 & 0xFFu;
  // After `lwz r9,48(r3)` / `lbz r8,113(r9)` nothing in the function writes r8 or r9; checked against memory here
  // because the early-return paths leave other values there.
  uint32_t trick = ctx.r9.u32, b71 = ctx.r8.u32;
  if (b71 > 1 || !Ptr(trick) || !Readable(base, trick + 0x6C, 0x12) || U8(base, trick + 0x71) != b71) {
    trick = 0;
    b71 = 0xFFu;
  }
  const uint32_t b70 = trick ? U8(base, trick + 0x70) : 0xFFu;
  const uint32_t id = trick ? TryU32(base, trick + 0x6C) : 0xFFFFFFFFu;
  const float timer = TryF32(base, mgr + 0x2163C);
  const bool timer_on = timer > 0.0f;
  const uint64_t now = GetTickCount64();
  std::lock_guard<std::mutex> lock(g_scat_lock);
  ++g_scat.calls;
  const bool changed = ret != g_scat.ret || trick != g_scat.trick || b71 != g_scat.b71 || b70 != g_scat.b70 ||
                       id != g_scat.id || timer_on != g_scat.timer_on;
  const bool hb = now - g_scat.hb_ms >= 1000;
  if (!changed && !hb) return;
  rex::audio_trace::line("SCATTEST", "%s\t%08X\t%u\t%.3f\t%08X\t%X\t%X\t%d\t%u\t%08X", changed ? "chg" : "hb", mgr,
                         ret, timer, trick, b71, b70, static_cast<int32_t>(id), g_scat.calls, lr);
  g_scat.ret = ret;
  g_scat.trick = trick;
  g_scat.b71 = b71;
  g_scat.b70 = b70;
  g_scat.id = id;
  g_scat.timer_on = timer_on;
  g_scat.calls = 0;
  if (hb) g_scat.hb_ms = now;
}

namespace {

struct XRow {
  uint32_t x = 0;
  uint8_t v[6] = {};
};
std::mutex g_x_lock;
XRow g_x[16];
int g_x_count = 0;
std::atomic<bool> g_xvt_done{false};

void XText(const uint8_t (&v)[6], char (&out)[40]) {
  const int32_t id = static_cast<int32_t>((uint32_t(v[0]) << 24) | (uint32_t(v[1]) << 16) | (uint32_t(v[2]) << 8) | v[3]);
  std::snprintf(out, sizeof out, "%d %u %u", id, v[4], v[5]);
}

bool XRead(uint8_t* base, uint32_t x, uint8_t (&v)[6]) {
  if (!Ptr(x) || !Readable(base, x + 5984, 6)) return false;
  std::memcpy(v, base + x + 5984, 6);
  return true;
}

void XWriteCheck(PPCContext& ctx, uint8_t* base, uint32_t fn, uint32_t x, const uint8_t (&before)[6], uint64_t lr) {
  uint8_t after[6];
  if (!XRead(base, x, after) || std::memcmp(before, after, 6) == 0) return;
  static std::atomic<int> left{400};
  if (left.fetch_sub(1) <= 0) return;
  char b[40], a[40], callers[64];
  XText(before, b);
  XText(after, a);
  ctx.lr = lr;  // the entry value (unchanged by the callee's blr), so CallerChain names this call's callers
  CallerChain(ctx, base, callers);
  rex::audio_trace::line("XWRITE", "%08X\t%08X\t%s\t%s\t%s", fn, x, b, a, callers);
}

}  // namespace

extern "C" REX_FUNC(sub_82595FA8) {
  static const bool on = FleeOn("fleex");
  if (on) {
    const uint32_t x = ctx.r3.u32, t = ctx.r4.u32;
    uint8_t v[6];
    if (XRead(base, x, v)) {
      bool log = false;
      {
        std::lock_guard<std::mutex> lock(g_x_lock);
        int i = 0;
        while (i < g_x_count && g_x[i].x != x) ++i;
        if (i == g_x_count && g_x_count < 16) {
          g_x[g_x_count++].x = x;
          log = true;
        }
        if (i < g_x_count && (log || std::memcmp(g_x[i].v, v, 6) != 0)) {
          std::memcpy(g_x[i].v, v, 6);
          log = true;
        }
      }
      if (log) {
        char text[40], callers[64];
        XText(v, text);
        CallerChain(ctx, base, callers);
        rex::audio_trace::line("XSRC", "%08X\t%08X\t%08X\t%s\t%s", x, TryU32(base, x), t, text, callers);
      }
    }
    if (!g_xvt_done.exchange(true)) {
      char words[22 * 9 + 1] = {};
      for (int i = 0; i < 22; ++i) {
        std::snprintf(words + i * 9, 10, "%08X ", TryU32(base, 0x823006A0u + 4u * i));
      }
      rex::audio_trace::line("XVT", "%08X\t%s", TryU32(base, x), words);
    }
  }
  __imp__sub_82595FA8(ctx, base);
}

// The X vtable methods at 0x823006A0..0x823006F4 (b637 section 6 item 3; the XVT row checks the slots at runtime).
#define X_PROBE(addr)                                                     \
  extern "C" REX_FUNC(sub_##addr) {                                       \
    static const bool on = FleeOn("fleex");                                    \
    const uint32_t x = ctx.r3.u32;                                        \
    const uint64_t lr = ctx.lr;                                           \
    uint8_t before[6];                                                    \
    const bool armed = on && XRead(base, x, before);                      \
    __imp__sub_##addr(ctx, base);                                         \
    if (armed) XWriteCheck(ctx, base, 0x##addr##u, x, before, lr);        \
  }

X_PROBE(8258E788)
X_PROBE(82594E50)
X_PROBE(8258F420)
X_PROBE(8258F2E8)
X_PROBE(8258F330)
X_PROBE(8258F338)
X_PROBE(8258F358)
X_PROBE(8258F360)

namespace {
void LogBegin(PPCContext& ctx, uint8_t* base, const char* kind, FirstPassLimit& limit) {
  uint32_t skipped = 0;
  if (!FirstPassAllow(limit, skipped)) return;
  const uint32_t ped = Follow(base, ctx.r4.u32, 4);
  char pos[64], callers[64];
  Vec3Text(base, Follow(base, ped, 0x7E4) + 0x80, pos, sizeof pos);
  CallerChain(ctx, base, callers);
  rex::audio_trace::line(kind, "%08X\t%08X\t%08X\t%X\t%s\t%s", ctx.r3.u32, ctx.r4.u32, ped,
                         Ptr(ped) ? U8(base, ped + 5936) : 0xFFFFFFFFu, pos, callers);
}
}  // namespace

extern "C" REX_FUNC(sub_826A3530) {
  static FirstPassLimit limit;
  static const bool on = FleeOn("fleebeg");
  if (on) LogBegin(ctx, base, "FLEEBEGIN", limit);
  __imp__sub_826A3530(ctx, base);
}

extern "C" REX_FUNC(sub_826A8E48) {
  static FirstPassLimit limit;
  static const bool on = FleeOn("fleebeg");
  if (on) LogBegin(ctx, base, "SCATBEGIN", limit);
  __imp__sub_826A8E48(ctx, base);
}

// WANTSET <ms> brain | want | flag | target (2 words, brain + 0x19C + want * 12 after the call) | callers   (5 fields)
// The want setter sub_82E42A58 (r3 = brain, r4 = want, r5 = 1 raise / 0 clear). Logs every raise and every flee
// (want 10) change, at most 20 lines a second. Want enum: 0 none, 1 angrychase, 2 warn, 3 taunt, 4 returngreet, 5 greet, 6 startconversation,
// 7 alertto, 8 joinchase, 9 throwhandprop, 10 flee, 11 startle, 12 taze, 13 nearbycollisionreaction, 14 slamreaction,
// 15 startspectate, 16 nearbyskatertrick.
extern "C" REX_FUNC(sub_82E42A58) {
  static FirstPassLimit limit;
  static const bool on = FleeOn("fleebeg");
  const uint32_t brain = ctx.r3.u32, want = ctx.r4.u32, flag = ctx.r5.u32;
  char callers[64] = "-";
  const bool log = on && (flag == 1 || want == 10);
  if (log) CallerChain(ctx, base, callers);
  __imp__sub_82E42A58(ctx, base);
  uint32_t skipped = 0;
  if (!log || want > 64 || !FirstPassAllow(limit, skipped)) return;
  const uint32_t slot = brain + 0x19Cu + want * 12u;
  rex::audio_trace::line("WANTSET", "%08X\t%u\t%u\t%08X %08X\t%s", brain, want, flag, TryU32(base, slot),
                         TryU32(base, slot + 4u), callers);
}

FIRST_PASS_HOOK(82C568D0, "fleeped", "PEDLADD", "")
FIRST_PASS_HOOK(82C56990, "fleeped", "PEDLREM", "")

namespace skate3_research {

// From the sub_82C54618 hook (hooks_props.cpp) before the original runs; self = DMO+0x88.
void FleeDmoListeners(PPCContext& ctx, uint8_t* base, uint32_t self) {
  static const bool on = FleeOn("fleedmo");
  if (!on) return;
  const uint32_t begin = TryU32(base, self + 4088), end = TryU32(base, self + 4092);
  if (begin == 0xFFFFFFFFu || end == 0xFFFFFFFFu || end < begin || end - begin > 4 * 256) return;
  const uint32_t count = (end - begin) / 4;
  static std::mutex lock;
  static uint32_t dmos[64] = {}, counts[64] = {};
  static int n = 0;
  static uint32_t targets[16] = {};
  static int n_targets = 0;
  std::lock_guard<std::mutex> guard(lock);
  int i = 0;
  while (i < n && dmos[i] != self) ++i;
  if (i == n) {
    if (n == 64 || count == 0) return;  // only DMOs that ever have listeners
    dmos[n] = self;
    counts[n++] = 0xFFFFFFFFu;
  }
  if (counts[i] == count) return;
  counts[i] = count;
  char entries[40] = "-", callers[64];
  for (uint32_t k = 0; k < count && k < 4; ++k) {
    std::snprintf(entries + k * 9, 10, "%08X ", TryU32(base, begin + 4 * k));
  }
  CallerChain(ctx, base, callers);
  rex::audio_trace::line("DMOLIST", "%08X\t%u\t%s\t%s", self - 0x88, count, entries, callers);
  for (uint32_t k = 0; k < count && k < 32; ++k) {
    const uint32_t e = TryU32(base, begin + 4 * k);
    if (!Ptr(e)) continue;
    const uint32_t iface = FindInterface(base, e + 4, 0x8269A8C8u);
    if (!Ptr(iface)) continue;
    const uint32_t ped = iface - 4, vtbl = TryU32(base, ped), target = Follow(base, vtbl, 0x6C);
    if (std::find(targets, targets + n_targets, target) != targets + n_targets || n_targets == 16) continue;
    targets[n_targets++] = target;
    rex::audio_trace::line("PEDVF6C", "%08X\t%08X\t%08X\t%08X\t%08X", self - 0x88, e, ped, vtbl, target);
  }
}

}  // namespace skate3_research
