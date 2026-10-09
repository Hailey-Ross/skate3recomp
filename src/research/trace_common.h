// Research hooks (tracing tools; not intended for the upstream project). Shared helpers for the skate3 research hooks
// (src/research/hooks_*.cpp). All hooks write to one trace file with one time base
// (rex/audio/audio_trace.h: SKATE3_AUDIO_TRACE_FILE, SKATE3_AUDIO_TRACE_T0).
//
// Categories: SKATE3_TRACE=audio,audiox,dsp,physics,world,npc,traffic,skitch,aiskater,watch (comma list) limits which hooks log; unset or
// empty = all. Every read through a pointer whose meaning is inferred must be guarded (Readable /
// Try*): an unguarded read crashed the recomp on 2026-10-02.
#pragma once

#include "generated/skate3_init.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/audio/audio_trace.h>

namespace skate3_research {

inline uint32_t LoadU32(uint8_t* base, uint32_t addr) {
  uint32_t raw;
  std::memcpy(&raw, base + addr, 4);
  return __builtin_bswap32(raw);
}

inline uint64_t LoadU64(uint8_t* base, uint32_t addr) {
  return (static_cast<uint64_t>(LoadU32(base, addr)) << 32) | LoadU32(base, addr + 4);
}

inline float LoadF32(uint8_t* base, uint32_t addr) {
  const uint32_t raw = LoadU32(base, addr);
  float value;
  std::memcpy(&value, &raw, 4);
  return value;
}

inline bool Plausible(uint32_t addr) { return addr >= 0x10000; }

// Light mode (2026-10-02): probe the range by touching one byte per 4 KB page under SEH instead of a
// VirtualQuery syscall per check; an access violation is caught and reported as "not readable". Same
// time-of-check guarantee as before, much cheaper on hot hooks. Kept in its own function: SEH blocks
// can't share a function with C++ objects that need unwinding.
__declspec(noinline) inline bool ProbeRead(const volatile uint8_t* p, uint32_t size) {
  __try {
    uint8_t sink = p[0];
    for (uint32_t off = 4096 - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p) & 4095); off < size; off += 4096) {
      sink ^= p[off];
    }
    sink ^= p[size - 1];
    (void)sink;
    return true;
  } __except (1) {
    return false;
  }
}

// True when [addr, addr+size) of guest memory is readable on the host.
inline bool Readable(uint8_t* base, uint32_t addr, uint32_t size) {
  if (!Plausible(addr) || size == 0 || static_cast<uint64_t>(addr) + size > 0x100000000ull) return false;
  return ProbeRead(base + addr, size);
}

// Guarded reads: `fallback` when the memory is not readable.
inline uint32_t TryU32(uint8_t* base, uint32_t addr, uint32_t fallback = 0xFFFFFFFFu) {
  return Readable(base, addr, 4) ? LoadU32(base, addr) : fallback;
}
inline uint64_t TryU64(uint8_t* base, uint32_t addr, uint64_t fallback = ~0ull) {
  return Readable(base, addr, 8) ? LoadU64(base, addr) : fallback;
}
inline float TryF32(uint8_t* base, uint32_t addr, float fallback = NAN) {
  return Readable(base, addr, 4) ? LoadF32(base, addr) : fallback;
}
// "x y z" of three floats at addr ("nan nan nan" when unreadable).
inline void Vec3Text(uint8_t* base, uint32_t addr, char* out, size_t n) {
  if (!Readable(base, addr, 12)) {
    std::snprintf(out, n, "nan nan nan");
    return;
  }
  std::snprintf(out, n, "%.4f %.4f %.4f", LoadF32(base, addr), LoadF32(base, addr + 4), LoadF32(base, addr + 8));
}

// 48 bytes at a guest address as hex, so a sample can be found verbatim in the bank files.
inline void HexAt(uint8_t* base, uint32_t addr, char (&out)[97]) {
  static const char kDigits[] = "0123456789abcdef";
  if (!Readable(base, addr, 48)) {
    std::snprintf(out, sizeof out, "-");
    return;
  }
  for (int i = 0; i < 48; ++i) {
    const uint8_t byte = base[addr + i];
    out[2 * i] = kDigits[byte >> 4];
    out[2 * i + 1] = kDigits[byte & 15];
  }
  out[96] = 0;
}

// Return addresses: the direct caller (lr) and three more up the back-chain.
inline void CallerChain(PPCContext& ctx, uint8_t* base, char (&out)[64]) {
  uint32_t chain[4] = {static_cast<uint32_t>(ctx.lr), 0, 0, 0};
  uint32_t frame = ctx.r1.u32;
  for (int i = 1; i < 4; ++i) {
    const uint32_t back = Readable(base, frame, 4) ? LoadU32(base, frame) : 0;
    if (!Plausible(back) || back <= frame || !Readable(base, back - 8, 4)) break;
    chain[i] = LoadU32(base, back - 8);
    frame = back;
  }
  std::snprintf(out, sizeof(out), "%08X<%08X<%08X<%08X", chain[0], chain[1], chain[2], chain[3]);
}

// Printable name of up to 32 bytes at a guest address ('-' when empty or unreadable).
inline void NameAt(uint8_t* base, uint32_t addr, char (&out)[33]) {
  int n = 0;
  if (Readable(base, addr, 32)) {
    for (; n < 32; ++n) {
      const uint8_t c = base[addr + n];
      if (c == 0) break;
      out[n] = (c >= 32 && c < 127 && c != '\t') ? static_cast<char>(c) : '?';
    }
  }
  if (n == 0) out[n++] = '-';
  out[n] = 0;
}

// Category gate (SKATE3_TRACE). Combined with the trace file being set.
// Light mode (2026-10-02): parsed once into a table of known categories; each call is a few short strcmp's,
// no allocation (previously two std::string concatenations per call, on hooks called thousands of times a
// second). Unknown categories are treated like the old substring match, computed on the fly.
inline bool On(const char* category) {
  static const char* const kKnown[] = {"audio", "audiox", "dsp", "dspmod", "physics", "world", "npc", "traffic", "skitch", "aiskater", "watch", "loadscreen", "render"};
  constexpr int kCount = sizeof kKnown / sizeof kKnown[0];
  struct Table {
    bool enabled = false;
    bool all = false;
    std::string list;
    bool on[kCount] = {};
  };
  static const Table table = [] {
    Table t;
    t.enabled = rex::audio_trace::enabled();
    const char* v = std::getenv("SKATE3_TRACE");
    t.list = std::string(",") + (v ? v : "") + ",";
    t.all = !v || !*v;
    for (int i = 0; i < kCount; ++i) {
      t.on[i] = t.all || t.list.find(std::string(",") + kKnown[i] + ",") != std::string::npos;
    }
    return t;
  }();
  if (!table.enabled) return false;
  for (int i = 0; i < kCount; ++i) {
    if (std::strcmp(category, kKnown[i]) == 0) return table.on[i];
  }
  return table.all || table.list.find(std::string(",") + category + ",") != std::string::npos;
}

// ---- First-pass hooks (argument meanings unknown) ----
// FIRST_PASS_HOOK(addr, category, kind, tag) logs raw r3..r8 / f1 f2 on entry, r3 / f1 after the call and the
// callers, rate limited to kFirstPassPerSecond lines per second per hook; `skipped=N` counts dropped calls.
//   <KIND> <ms> [tag] skipped=N | r3 r4 r5 r6 r7 r8 | f1 f2 | ret r3 | ret f1 | callers
// Summarise by counting distinct values per field per kind (constant = object/flag, varying = data).
constexpr uint32_t kFirstPassPerSecond = 20;

struct FirstPassLimit {
  std::mutex mutex;
  uint64_t second = 0;
  uint32_t lines = 0;
  uint32_t skipped = 0;
};

inline bool FirstPassAllow(FirstPassLimit& limit, uint32_t& skipped) {
  std::lock_guard<std::mutex> lock(limit.mutex);
  const uint64_t second = GetTickCount64() / 1000;
  if (second != limit.second) {
    limit.second = second;
    limit.lines = 0;
  }
  if (limit.lines >= kFirstPassPerSecond) {
    ++limit.skipped;
    return false;
  }
  ++limit.lines;
  skipped = limit.skipped;
  limit.skipped = 0;
  return true;
}

struct FirstPassEntry {
  uint32_t r[6];
  double f[2];
  char callers[64];
};

inline FirstPassEntry FirstPassCapture(PPCContext& ctx, uint8_t* base) {
  FirstPassEntry e{{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32}, {ctx.f1.f64, ctx.f2.f64}, {}};
  CallerChain(ctx, base, e.callers);
  return e;
}

inline void FirstPassLog(const char* kind, const char* tag, uint32_t skipped, const FirstPassEntry& e, PPCContext& ctx) {
  rex::audio_trace::line(kind, "%sskipped=%u\t%08X %08X %08X %08X %08X %08X\t%.4f %.4f\t%08X\t%.4f\t%s", tag, skipped,
                         e.r[0], e.r[1], e.r[2], e.r[3], e.r[4], e.r[5], e.f[0], e.f[1], ctx.r3.u32, ctx.f1.f64,
                         e.callers);
}

}  // namespace skate3_research

#define FIRST_PASS_HOOK(addr, category, kind, tag)                   \
  extern "C" REX_FUNC(sub_##addr) {                                  \
    using namespace skate3_research;                                 \
    static FirstPassLimit limit;                                     \
    uint32_t skipped = 0;                                            \
    const bool log = On(category) && FirstPassAllow(limit, skipped); \
    FirstPassEntry e{};                                              \
    if (log) e = FirstPassCapture(ctx, base);                        \
    __imp__sub_##addr(ctx, base);                                    \
    if (log) FirstPassLog(kind, tag, skipped, e, ctx);               \
  }
