// Research hooks (tracing tools; not intended for the upstream project): session-marker returns and the teleport
// flow behind them — distance and hold time, the streamed-or-load decision, game state changes, front-end sound
// requests and hub messages around a teleport. Category: SKATE3_TRACE=audiox (src/research/trace_common.h).
//
//   TPMARK   <ms> lr | distance | hold s | destination x y z | +80 | flags +84 +85 +86 | callers
//            (teleport request constructor sub_825582D0; for the session marker's call (lr 828993AC) the distance
//             is the caller's stack word +96 and the hold time its f31; other callers log nan)
//   TPDEC    <ms> destination x y z | r5 | r6 | r7 r8 r9 | load | busy | callers
//            (teleport decision sub_82706F50: load = 1 when the destination needs a streaming load (the loading
//             state is entered), 0 = placed at once; busy = the state's +40 byte before the call)
//   TPSTREAM <ms> destination x y z | r4 | result | callers   (streamed-in check sub_82864C40, ≤ 10 / s)
//   TPFX     <ms> object | amount                              (VisualDirector teleport-effect handler sub_827A9C60, on change)
//   FEREQ    <ms> key | r4 | callers                           (front-end sound request sub_825DFAF0, every call)
//   GSTATE   <ms> machine | state function | r5 | r6 | callers (state change sub_826DFB30, ≤ 30 / s)
//   GEVENT   <ms> queue | event | callers                      (state event push sub_826DB648, ≤ 30 / s)
//   HUBMSG   <ms> id | r5 | callers                            (message post sub_82B938D8, ≤ 2 / s per id)
//   TPOVR    <ms> lr | menu destination x y z | menu forward x z | override x y z | yaw deg
//            (any category; only when env SKATE3_TP_OVERRIDE="x y z yaw" is set: Challenge Map menu teleports
//             (lr 8286483C / 82864A08) land at that pose instead; yaw in degrees, forward = (sin yaw, 0, cos yaw); "pose; pose; ..." = one pose
//             per menu teleport in order, the last one repeats)
#include "trace_common.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <vector>

using namespace skate3_research;

namespace {

struct Limit {
  std::mutex mutex;
  uint64_t second = 0;
  uint32_t lines = 0;
};

bool Allow(Limit& limit, uint32_t per_second) {
  std::lock_guard<std::mutex> lock(limit.mutex);
  const uint64_t second = GetTickCount64() / 1000;
  if (second != limit.second) {
    limit.second = second;
    limit.lines = 0;
  }
  if (limit.lines >= per_second) return false;
  ++limit.lines;
  return true;
}

void Vec(uint8_t* base, uint32_t addr, char (&out)[64]) {
  if (Readable(base, addr, 12)) {
    std::snprintf(out, sizeof(out), "%.2f %.2f %.2f", LoadF32(base, addr), LoadF32(base, addr + 4),
                  LoadF32(base, addr + 8));
  } else {
    std::snprintf(out, sizeof(out), "nan nan nan");
  }
}

uint32_t Byte(uint8_t* base, uint32_t addr) { return Readable(base, addr, 1) ? base[addr] : 255u; }

void StoreF32(uint8_t* base, uint32_t addr, float value) {
  uint32_t raw;
  std::memcpy(&raw, &value, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(base + addr, &raw, 4);
}

// SKATE3_TP_OVERRIDE="x y z yaw[; x y z yaw ...]" (yaw in degrees; forward = (sin yaw, 0, cos yaw)): parsed once;
// the n-th menu teleport takes the n-th pose (the last one repeats); unset = off.
struct TpOverride {
  bool on = false;
  float x = 0, y = 0, z = 0, yaw_deg = 0;
};

TpOverride NextTpOverride() {
  static const std::vector<TpOverride> poses = [] {
    std::vector<TpOverride> list;
    const char* env = std::getenv("SKATE3_TP_OVERRIDE");
    while (env && *env) {
      TpOverride v;
      if (std::sscanf(env, "%f %f %f %f", &v.x, &v.y, &v.z, &v.yaw_deg) == 4) {
        v.on = true;
        list.push_back(v);
      }
      env = std::strchr(env, ';');
      if (env) ++env;
    }
    return list;
  }();
  static std::atomic<uint32_t> next{0};
  if (poses.empty()) return {};
  const uint32_t i = next.fetch_add(1);
  return poses[i < poses.size() ? i : poses.size() - 1];
}

}  // namespace

extern "C" REX_FUNC(sub_825582D0) {
  if (On("audiox")) {
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    const bool marker = lr == 0x828993ACu;
    const float distance = marker ? TryF32(base, ctx.r1.u32 + 96) : NAN;
    const double hold = marker ? ctx.f31.f64 : NAN;
    char dest[64];
    Vec(base, ctx.r4.u32 + 48, dest);
    char callers[64];
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("TPMARK", "%08X\t%.3f\t%.4f\t%s\t%08X\t%u %u %u\t%s", lr, distance, hold, dest, ctx.r5.u32,
                           ctx.r6.u32 & 255u, ctx.r7.u32 & 255u, ctx.r8.u32 & 255u, callers);
  }
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t src = ctx.r4.u32;
  const bool menu = lr == 0x8286483Cu || lr == 0x82864A08u;
  const TpOverride tp = menu ? NextTpOverride() : TpOverride{};
  if (!tp.on || !Readable(base, src, 64)) {
    __imp__sub_825582D0(ctx, base);
    return;
  }
  // Menu teleport with SKATE3_TP_OVERRIDE set: swap the destination matrix (rows: right, up, forward, position) for
  // the requested pose for the duration of the call (the constructor copies the 64 bytes into the request), then
  // put the caller's bytes back.
  uint8_t saved[64];
  std::memcpy(saved, base + src, 64);
  char orig[64];
  Vec(base, src + 48, orig);
  const float fwd_x = LoadF32(base, src + 32), fwd_z = LoadF32(base, src + 40);
  const float s = std::sin(tp.yaw_deg * 3.14159265f / 180.0f), c = std::cos(tp.yaw_deg * 3.14159265f / 180.0f);
  const float rows[16] = {c, 0.0f, -s, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, s, 0.0f, c, 0.0f, tp.x, tp.y, tp.z, 1.0f};
  for (int i = 0; i < 16; ++i) StoreF32(base, src + 4u * i, rows[i]);
  __imp__sub_825582D0(ctx, base);
  std::memcpy(base + src, saved, 64);
  rex::audio_trace::line("TPOVR", "%08X\t%s\t%.2f %.2f\t%.2f %.2f %.2f\t%.1f", lr, orig, fwd_x, fwd_z, tp.x, tp.y, tp.z,
                         tp.yaw_deg);
}

extern "C" REX_FUNC(sub_82706F50) {
  if (!On("audiox")) {
    __imp__sub_82706F50(ctx, base);
    return;
  }
  char dest[64];
  Vec(base, ctx.r4.u32 + 48, dest);
  const uint32_t r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32 & 255u, r8 = ctx.r8.u32 & 255u,
                 r9 = ctx.r9.u32 & 255u;
  const uint32_t busy = Byte(base, ctx.r3.u32 + 40);
  char callers[64];
  CallerChain(ctx, base, callers);
  __imp__sub_82706F50(ctx, base);
  rex::audio_trace::line("TPDEC", "%s\t%08X\t%08X\t%u %u %u\t%u\t%u\t%s", dest, r5, r6, r7, r8, r9, ctx.r3.u32 & 255u,
                         busy, callers);
}

extern "C" REX_FUNC(sub_82864C40) {
  static Limit limit;
  if (!On("audiox") || !Allow(limit, 10)) {
    __imp__sub_82864C40(ctx, base);
    return;
  }
  char dest[64];
  Vec(base, ctx.r3.u32 + 48, dest);
  const uint32_t r4 = ctx.r4.u32 & 255u;
  char callers[64];
  CallerChain(ctx, base, callers);
  __imp__sub_82864C40(ctx, base);
  rex::audio_trace::line("TPSTREAM", "%s\t%u\t%u\t%s", dest, r4, ctx.r3.u32 & 255u, callers);
}

extern "C" REX_FUNC(sub_827A9C60) {
  if (On("audiox")) {
    static std::atomic<uint32_t> last{0xFFFFFFFFu};
    const uint32_t bits = TryU32(base, ctx.r4.u32 + 16, 0xFFFFFFFFu);
    if (last.exchange(bits) != bits) {
      float amount;
      std::memcpy(&amount, &bits, 4);
      rex::audio_trace::line("TPFX", "%08X\t%.4f", ctx.r3.u32, amount);
    }
  }
  __imp__sub_827A9C60(ctx, base);
}

extern "C" REX_FUNC(sub_825DFAF0) {
  if (On("audiox")) {
    char callers[64];
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("FEREQ", "%016llX\t%08X\t%s", static_cast<unsigned long long>(ctx.r3.u64), ctx.r4.u32,
                           callers);
  }
  __imp__sub_825DFAF0(ctx, base);
}

extern "C" REX_FUNC(sub_826DFB30) {
  static Limit limit;
  if (On("audiox") && Allow(limit, 30)) {
    char callers[64];
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("GSTATE", "%08X\t%08X\t%08X\t%08X\t%s", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                           callers);
  }
  __imp__sub_826DFB30(ctx, base);
}

extern "C" REX_FUNC(sub_826DB648) {
  static Limit limit;
  if (On("audiox") && Allow(limit, 30)) {
    char callers[64];
    CallerChain(ctx, base, callers);
    rex::audio_trace::line("GEVENT", "%08X\t%d\t%s", ctx.r3.u32, ctx.r4.s32, callers);
  }
  __imp__sub_826DB648(ctx, base);
}

extern "C" REX_FUNC(sub_82B938D8) {
  if (On("audiox")) {
    static std::mutex mutex;
    static std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> seen;
    const uint32_t id = TryU32(base, ctx.r3.u32, 0xFFFFFFFFu);
    bool log = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const uint64_t second = GetTickCount64() / 1000;
      auto& s = seen[id];
      if (s.first != second) s = {second, 0};
      log = s.second < 2;
      ++s.second;
    }
    if (log) {
      char callers[64];
      CallerChain(ctx, base, callers);
      rex::audio_trace::line("HUBMSG", "%08X\t%d\t%s", id, ctx.r5.s32, callers);
    }
  }
  __imp__sub_82B938D8(ctx, base);
}

// ---- Skater see-through ("ghost") opacity (2026-10-08). Gate: env SKATE3_GHOST=1 plus the trace file; any category.
//   GHOST    <ms> slot | record index | object | flags +188 (hex) | T | i_params.x | +192 int | mode
//            (per-frame fade sub_8278C4D8, per character slot of the presentation manager (+1840, 7 slots), after the
//             call; on change of flags / mode or alpha moved > 0.004, plus 1 Hz per slot. T recomputed from the flags
//             as the code does: bit 0x200000 clear -> mode ? 1 : 0.5; 0x200000 + 0x100000 + mode -> 1; else
//             ((flags >> 12) & 255) / 255. mode = last return of sub_8279E180 called from sub_8278C4D8)
//   GHREC    <ms> dst | src | src +160 id | src alpha byte +190 | src +192 (hex) | callers
//            (record copy sub_8278C3B0, on change of the source's +190 byte or +192 bits 0xF000 per destination)
//   GHSRC    <ms> memcpy dst | src | size | source word address | its +188 / +192 words | callers
//            (memcpy sub_82AED480 from the blob writer sub_82DBB890 landing on a render record seen with bit 0x4000)
//   GHWATCH  <ms> armed | watched guest addresses | module  (one-shot hardware write watch, all threads, on that
//             source's +188 / +192 words; disarmed after 48 hits)
//   GHHIT    <ms> n/total | dr6 | value +188 | value +192 | host rva chain (llvm-symbolizer on skate3.exe)
//            (first 48 writes that hit the watch; host frames are the recompiled guest functions)
//   GHHOP    <ms> hop | memcpy dst | src | size | old watch -> new watch | new words | lr | callers
//            (a memcpy about to overwrite the watched words moves the watch onto its source, up to 6 hops)
#include <tlhelp32.h>
#include <thread>

namespace {

bool GhostOn() {
  static const bool on = [] {
    const char* v = std::getenv("SKATE3_GHOST");
    return rex::audio_trace::enabled() && v && *v && *v != '0';
  }();
  return on;
}

std::atomic<uint32_t> g_ghost_mode{0xFFu};

struct GhostHit {
  std::atomic<uint32_t> ready{0};
  uint32_t dr6 = 0;
  uint32_t frames = 0;
  uint64_t rip[14] = {};
};
constexpr uint32_t kGhostHits = 48;
GhostHit g_ghost_hits[kGhostHits];
std::atomic<uint32_t> g_ghost_hit_count{0};
std::atomic<uint32_t> g_ghost_hit_total{0};
uint32_t g_ghost_drained = 0;
uint32_t g_ghost_watch[4] = {};  // guest addresses (DR0..DR3)
uint32_t g_ghost_rec[2] = {};    // render-side record +188 addresses (double buffer)
uint64_t g_ghost_module = 0;

LONG CALLBACK GhostVeh(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD64 dr6 = ep->ContextRecord->Dr6;
  if ((dr6 & 0xF) == 0) return EXCEPTION_CONTINUE_SEARCH;
  g_ghost_hit_total.fetch_add(1);
  const uint32_t i = g_ghost_hit_count.fetch_add(1);
  if (i < kGhostHits) {
    GhostHit& h = g_ghost_hits[i];
    h.dr6 = static_cast<uint32_t>(dr6 & 0xF);
    CONTEXT c = *ep->ContextRecord;
    uint32_t n = 0;
    for (; n < 14 && c.Rip; ++n) {
      h.rip[n] = c.Rip;
      DWORD64 image = 0;
      PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
      if (!fe) {
        c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
        c.Rsp += 8;
      } else {
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fe, &c, &handler_data, &establisher, nullptr);
      }
    }
    h.frames = n;
    h.ready.store(1, std::memory_order_release);
  }
  ep->ContextRecord->Dr6 = 0;
  if (i + 1 >= kGhostHits) ep->ContextRecord->Dr7 = 0;  // cap reached: disarm this thread at once
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets DR0..DR3 (write, 4 bytes) on every thread of the process; run from a helper thread so the caller is armed too.
void GhostArmAll(uint8_t* base) {
  std::thread([base] {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    DWORD64 dr7 = 0;
    for (int k = 0; k < 4; ++k) {
      if (g_ghost_watch[k]) dr7 |= (1ull << (2 * k)) | (0x1ull << (16 + 4 * k)) | (0x3ull << (18 + 4 * k));
    }
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
      HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
      if (!t) continue;
      if (SuspendThread(t) != static_cast<DWORD>(-1)) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
          c.Dr0 = g_ghost_watch[0] ? reinterpret_cast<DWORD64>(base + g_ghost_watch[0]) : 0;
          c.Dr1 = g_ghost_watch[1] ? reinterpret_cast<DWORD64>(base + g_ghost_watch[1]) : 0;
          c.Dr2 = g_ghost_watch[2] ? reinterpret_cast<DWORD64>(base + g_ghost_watch[2]) : 0;
          c.Dr3 = g_ghost_watch[3] ? reinterpret_cast<DWORD64>(base + g_ghost_watch[3]) : 0;
          c.Dr7 = dr7;
          SetThreadContext(t, &c);
        }
        ResumeThread(t);
      }
      CloseHandle(t);
    }
    CloseHandle(snap);
  }).join();
}

void GhostDrain(uint8_t* base) {
  while (g_ghost_drained < kGhostHits && g_ghost_drained < g_ghost_hit_count.load()) {
    GhostHit& h = g_ghost_hits[g_ghost_drained];
    if (!h.ready.load(std::memory_order_acquire)) break;
    char chain[14 * 18 + 1];
    int off = 0;
    for (uint32_t k = 0; k < h.frames && off < static_cast<int>(sizeof chain) - 18; ++k) {
      off += std::snprintf(chain + off, sizeof chain - off, "%s%llX", k ? "<" : "",
                           static_cast<unsigned long long>(h.rip[k] - g_ghost_module));
    }
    chain[off] = 0;
    const uint32_t w = g_ghost_watch[0];
    rex::audio_trace::line("GHHIT", "%u/%u\t%X\t%08X\t%08X\t%s", g_ghost_drained, g_ghost_hit_total.load(), h.dr6,
                           w ? TryU32(base, w, 0xFFFFFFFFu) : 0u, w ? TryU32(base, w + 4, 0xFFFFFFFFu) : 0u, chain);
    ++g_ghost_drained;
  }
  static bool disarmed = false;
  if (!disarmed && g_ghost_drained >= kGhostHits) {  // one-shot: clear the watch on every thread
    disarmed = true;
    for (uint32_t& w : g_ghost_watch) w = 0;
    GhostArmAll(base);
    rex::audio_trace::line("GHWATCH", "0\tdisarmed after %u hits", g_ghost_hit_total.load());
  }
}

}  // namespace

extern "C" REX_FUNC(sub_8279E180) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_8279E180(ctx, base);
  if (lr > 0x8278C4D8u && lr < 0x8278C8A8u && GhostOn()) g_ghost_mode.store(ctx.r3.u32 & 255u);
}

extern "C" REX_FUNC(sub_8278C4D8) {
  const uint32_t mgr = ctx.r3.u32;
  __imp__sub_8278C4D8(ctx, base);
  if (!GhostOn()) return;
  GhostDrain(base);
  struct Last {
    uint32_t flags = 0xFFFFFFFFu, mode = 0xFFFFFFFFu;
    float a = -1.0f;
    uint64_t ms = 0;
  };
  static Last last[7];
  const uint32_t mode = g_ghost_mode.load();
  const uint64_t now = GetTickCount64();
  for (uint32_t i = 0; i < 7; ++i) {
    const uint32_t obj = TryU32(base, mgr + 1840 + 4 * i, 0);
    if (!obj || !Readable(base, obj + 496, 4) || !Readable(base, obj + 868, 4)) continue;
    const uint32_t idx = LoadU32(base, obj + 868);
    if (idx >= 8) continue;
    const uint32_t rec = mgr + 176 * idx;
    const uint32_t flags = TryU32(base, rec + 188, 0);
    const int32_t i192 = static_cast<int32_t>(TryU32(base, rec + 192, 0));
    const float a = LoadF32(base, obj + 496);
    float t;
    if (!(flags & 0x200000u)) t = mode ? 1.0f : 0.5f;
    else if ((flags & 0x100000u) && mode) t = 1.0f;
    else t = static_cast<float>((flags >> 12) & 255u) / 255.0f;
    Last& l = last[i];
    if (flags != l.flags || mode != l.mode || std::fabs(a - l.a) > 0.004f || now - l.ms >= 1000) {
      l.flags = flags;
      l.mode = mode;
      l.a = a;
      l.ms = now;
      rex::audio_trace::line("GHOST", "%u\t%u\t%08X\t%08X\t%.4f\t%.4f\t%d\t%u", i, idx, obj, flags, t, a, i192, mode);
    }
  }
}

extern "C" REX_FUNC(sub_8278C3B0) {
  if (GhostOn()) {
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    if (Readable(base, src, 208)) {
      const uint32_t w188 = LoadU32(base, src + 188), w192 = LoadU32(base, src + 192);
      const uint32_t alpha = (w188 >> 8) & 255u;
      const uint32_t key = (alpha << 16) | (w192 & 0xF000u);
      static std::mutex mutex;
      static std::unordered_map<uint32_t, uint32_t> seen;
      bool log = false, arm = false;
      {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = seen.find(dst);
        if (it == seen.end() || it->second != key) {
          seen[dst] = key;
          log = true;
        }
        // Run 1 showed the render records are filled by the generic blob mover sub_82DBB890 (memcpy sub_82AED480):
        // remember the render-side record words here; the memcpy hook below arms the watch on their source.
        const uint32_t a188 = src + 188;
        if ((w192 & 0x4000u) && g_ghost_rec[0] != a188 && g_ghost_rec[1] != a188 && !g_ghost_rec[1]) {
          g_ghost_rec[g_ghost_rec[0] ? 1 : 0] = a188;
        }
      }
      if (log) {
        char callers[64];
        CallerChain(ctx, base, callers);
        rex::audio_trace::line("GHREC", "%08X\t%08X\t%016llX\t%u\t%08X\t%s", dst, src,
                               static_cast<unsigned long long>(LoadU64(base, src + 160)), alpha, w192, callers);
      }
      (void)arm;
    }
  }
  __imp__sub_8278C3B0(ctx, base);
}

// memcpy (sub_82AED480) as called by the blob mover (lr 82DBB928): when the copy lands on a remembered render record,
// log the source (GHSRC) and arm the one-shot write watch on the source words (+188 / +192 equivalents).
extern "C" REX_FUNC(sub_82AED480) {
  // Hop (GHHOP): a memcpy about to write the watched words moves the watch onto its source, up to 6 hops, so the
  // final hits show the code that writes the words in place (the game-side producer), not a copy stage.
  if (g_ghost_watch[0] && GhostOn()) {
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32, w = g_ghost_watch[0];
    static std::atomic<uint32_t> hops{0};
    if (w >= dst && w + 8 <= dst + n && hops.load() < 6) {
      static std::mutex hop_mutex;
      std::lock_guard<std::mutex> lock(hop_mutex);
      const uint32_t s = src + (w - dst);
      if (g_ghost_watch[0] == w && s != w && Readable(base, s, 8)) {
        const uint32_t hop = hops.fetch_add(1) + 1;
        g_ghost_watch[0] = s;
        g_ghost_watch[1] = s + 4;
        char callers[64];
        CallerChain(ctx, base, callers);
        rex::audio_trace::line("GHHOP", "%u\t%08X\t%08X\t%u\t%08X -> %08X\t%08X %08X\t%08X\t%s", hop, dst, src, n, w, s,
                               LoadU32(base, s), LoadU32(base, s + 4), static_cast<uint32_t>(ctx.lr), callers);
        GhostArmAll(base);
      }
    }
  }
  if (static_cast<uint32_t>(ctx.lr) == 0x82DBB928u && g_ghost_rec[0] && !g_ghost_watch[0] && g_ghost_hit_count.load() == 0 && GhostOn()) {
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
    for (uint32_t r : g_ghost_rec) {
      if (!r || r < dst || r + 8 > dst + n) continue;
      const uint32_t s = src + (r - dst);
      if (!Readable(base, s, 8) || !(LoadU32(base, s + 4) & 0x4000u)) continue;
      g_ghost_watch[0] = s;
      g_ghost_watch[1] = s + 4;
      char callers[64];
      CallerChain(ctx, base, callers);
      rex::audio_trace::line("GHSRC", "%08X\t%08X\t%u\t%08X\t%08X %08X\t%s", dst, src, n, s, LoadU32(base, s),
                             LoadU32(base, s + 4), callers);
      static std::once_flag veh;
      std::call_once(veh, [] {
        g_ghost_module = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        AddVectoredExceptionHandler(1, GhostVeh);
      });
      GhostArmAll(base);
      rex::audio_trace::line("GHWATCH", "1\t%08X %08X\t%llX", g_ghost_watch[0], g_ghost_watch[1],
                             static_cast<unsigned long long>(g_ghost_module));
      break;
    }
  }
  __imp__sub_82AED480(ctx, base);
}
