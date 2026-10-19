// Research hooks (tracing tools; not intended for the upstream project): pedestrian collision bodies, the bodies a
// skater collides with when running into a pedestrian. Category: SKATE3_TRACE=pedbody (src/research/trace_common.h).
//
// Each pedestrian's collision object builds two collision bodies (sub_82E268C8, body ctor sub_82E0E2A0), one in
// collision group 9 and one in group 11 (the one the skater's contact pass meets). Per body, sub_82E0E580 (r3 = the
// body, r4 = a struct whose first word points at one float mass per part) gives each part its mass; the parts are
// 160-byte records at *(*(body+28)+0), their count at *(*(body+28)+4), and each part's first word is its collision
// volume (type = *(*(vol+64)), radius vol+80). Field meanings below the type / radius are a first pass: the raw
// bytes are logged so a run can pin them down.
//   PEDBODY <ms> body | parts | callers | body bytes 0..47 (hex)
//   PEDPART <ms> body | index | mass | type | radius | vol+68 +72 +76 | vol+48 +52 +56 | part+92 | part bytes
//                0..47 | 48..95 | 96..143 (hex) | vol bytes 0..47 | 48..95 (hex)
// Add / remove (sub_82E0E5D8 / sub_82E0E730, r3 = the body): logged only when the body's in-world word (+36)
// changes across the call, so the per-frame pose and add path costs two reads.
//   PEDBADD / PEDBREM <ms> body | +36 before | +36 after | body+32 | callers | body bytes 0..47 (hex)
#include "trace_common.h"

using namespace skate3_research;

namespace {
constexpr uint32_t kPartStride = 160;
constexpr uint32_t kMaxParts = 4;

void LogParts(uint8_t* base, uint32_t body, uint32_t masses) {
  const uint32_t holder = TryU32(base, body + 28, 0);
  const uint32_t parts = holder ? TryU32(base, holder, 0) : 0;
  const uint32_t count = holder ? TryU32(base, holder + 4, 0) : 0;
  for (uint32_t i = 0; i < count && i < kMaxParts && parts; ++i) {
    const uint32_t part = parts + kPartStride * i;
    const uint32_t vol = TryU32(base, part, 0);
    const uint32_t type_ptr = vol ? TryU32(base, vol + 64, 0) : 0;
    const uint32_t type = type_ptr ? TryU32(base, type_ptr) : 0xFFFFFFFFu;
    const float mass = masses ? TryF32(base, masses + 4 * i) : NAN;
    char p0[97], p1[97], p2[97], v0[97], v1[97];
    HexAt(base, part, p0);
    HexAt(base, part + 48, p1);
    HexAt(base, part + 96, p2);
    if (vol) {
      HexAt(base, vol, v0);
      HexAt(base, vol + 48, v1);
    } else {
      std::snprintf(v0, sizeof v0, "-");
      std::snprintf(v1, sizeof v1, "-");
    }
    rex::audio_trace::line("PEDPART", "%08X\t%u\t%.4f\t%d\t%.4f\t%.4f %.4f %.4f\t%.4f %.4f %.4f\t%08X\t%s\t%s\t%s\t%s\t%s",
                           body, i, mass, static_cast<int>(type), vol ? TryF32(base, vol + 80) : NAN,
                           vol ? TryF32(base, vol + 68) : NAN, vol ? TryF32(base, vol + 72) : NAN,
                           vol ? TryF32(base, vol + 76) : NAN, vol ? TryF32(base, vol + 48) : NAN,
                           vol ? TryF32(base, vol + 52) : NAN, vol ? TryF32(base, vol + 56) : NAN,
                           TryU32(base, part + 92), p0, p1, p2, v0, v1);
  }
}

void LogChange(PPCContext& ctx, uint8_t* base, const char* kind, uint32_t body, uint32_t before) {
  const uint32_t after = TryU32(base, body + 36);
  if (after == before) return;
  char callers[64] = "-", bytes[97];
  CallerChain(ctx, base, callers);
  HexAt(base, body, bytes);
  rex::audio_trace::line(kind, "%08X\t%08X\t%08X\t%08X\t%s\t%s", body, before, after, TryU32(base, body + 32), callers,
                         bytes);
}
}  // namespace

// Per-part masses of one body (once per body, at the pedestrian's collision setup).
extern "C" REX_FUNC(sub_82E0E580) {
  const bool on = On("pedbody");
  const uint32_t body = ctx.r3.u32, mass_struct = ctx.r4.u32;
  char callers[64] = "-";
  if (on) CallerChain(ctx, base, callers);
  __imp__sub_82E0E580(ctx, base);
  if (!on) return;
  const uint32_t holder = TryU32(base, body + 28, 0);
  char bytes[97];
  HexAt(base, body, bytes);
  rex::audio_trace::line("PEDBODY", "%08X\t%u\t%s\t%s", body, holder ? TryU32(base, holder + 4, 0) : 0, callers, bytes);
  LogParts(base, body, mass_struct ? TryU32(base, mass_struct, 0) : 0);
}

// Body add (only adds when the body is not in the world).
extern "C" REX_FUNC(sub_82E0E5D8) {
  if (!On("pedbody")) {
    __imp__sub_82E0E5D8(ctx, base);
    return;
  }
  const uint32_t body = ctx.r3.u32;
  const uint32_t before = TryU32(base, body + 36);
  __imp__sub_82E0E5D8(ctx, base);
  LogChange(ctx, base, "PEDBADD", body, before);
}

// Body remove.
extern "C" REX_FUNC(sub_82E0E730) {
  if (!On("pedbody")) {
    __imp__sub_82E0E730(ctx, base);
    return;
  }
  const uint32_t body = ctx.r3.u32;
  const uint32_t before = TryU32(base, body + 36);
  __imp__sub_82E0E730(ctx, base);
  LogChange(ctx, base, "PEDBREM", body, before);
}

// ---- Player x pedestrian contact on foot ----
// The collision pair job (shared by the Skater x Ped, Skater x Skater, Ped x Ped and prop pairs) calls the pair
// handler sub_8277A508 per candidate pair: r3 / r7 = the two records' part arrays (224-byte parts, record = parts - 96),
// r6 / r10 = their part counts, the caller's stack word +84 = the contact buffer (count at +0, contacts from +128).
//   PCTYPE <ms> tA | tB | pA | pB | nA | nB | kindA | kindB | callers          (once per distinct type key)
//   PCPAIR <ms> partsA | partsB | tA | tB | pA | pB | nA | nB | idA | idB | before | after | ret | onfoot_age_ms |
//               skipped | first new contact (hex)                               (pairs that added contacts and touch
//               the ped type, SKATE3_PCPED_TYPE, default 11)
//   PCPOS <ms> partsA | partsB | part 0 bytes 0..191 of A (hex) | of B (hex)  (with every PCPAIR line)
// tA / tB = record word +0xC, pA / pB = part 0 word +212 (the pair table row); the run shows which one is the group.
// The on-foot wipeout check sub_82D90148 (r3 = the request block W, character = W+8): its inputs and the request
// bytes it set (W+20 contact, +21 displacement, +27 vehicle, +38 squash).
//   WIPEOB <ms> lr | W | character | region strengths C+880..C+908 | speed^2 | displacement | squash |
//               bytes 20 21 27 38 | counter W+200 | skipped
// Logged when a region strength is above zero or a byte is set (30 lines/s) plus a 1 Hz heartbeat.
namespace {
std::atomic<uint64_t> g_onfoot_ms{0};
std::mutex g_pctype_mutex;
uint64_t g_pctype_seen[64];
uint32_t g_pctype_count = 0;
FirstPassLimit g_pcpair_limit;

uint32_t PedType() {
  static const uint32_t type = [] {
    const char* v = std::getenv("SKATE3_PCPED_TYPE");
    return v && *v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 11u;
  }();
  return type;
}

uint32_t ByteAt(uint8_t* base, uint32_t addr) {
  const uint32_t word = TryU32(base, addr & ~3u, 0);
  return (word >> (8 * (3 - (addr & 3)))) & 0xFF;
}

bool FirstTypeKey(uint32_t tA, uint32_t tB, uint32_t pA, uint32_t pB) {
  const uint64_t key = (static_cast<uint64_t>(tA & 0xFFFF) << 48) | (static_cast<uint64_t>(tB & 0xFFFF) << 32) |
                       ((pA & 0xFFFF) << 16) | (pB & 0xFFFF);
  std::lock_guard<std::mutex> lock(g_pctype_mutex);
  for (uint32_t i = 0; i < g_pctype_count; ++i) {
    if (g_pctype_seen[i] == key) return false;
  }
  if (g_pctype_count >= 64) return false;
  g_pctype_seen[g_pctype_count++] = key;
  return true;
}
}  // namespace

extern "C" REX_FUNC(sub_8277A508) {
  const uint32_t a = ctx.r3.u32, b = ctx.r7.u32;
  if (!On("pedbody") || ctx.lr == 0x8277B2C4 || a == b) {
    __imp__sub_8277A508(ctx, base);
    return;
  }
  const uint32_t nA = ctx.r6.u32, nB = ctx.r10.u32;
  const uint32_t buf = TryU32(base, ctx.r1.u32 + 84, 0);
  const uint32_t before = buf ? TryU32(base, buf, 0) : 0;
  char callers[64] = "-";
  CallerChain(ctx, base, callers);
  __imp__sub_8277A508(ctx, base);
  const uint32_t ret = ctx.r3.u32 & 0xFF;
  const uint32_t after = buf ? TryU32(base, buf, 0) : 0;
  const uint32_t tA = TryU32(base, a - 96 + 0xC), tB = TryU32(base, b - 96 + 0xC);
  const uint32_t pA = TryU32(base, a + 212), pB = TryU32(base, b + 212);
  if (FirstTypeKey(tA, tB, pA, pB)) {
    rex::audio_trace::line("PCTYPE", "%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%s", tA, tB, pA, pB, nA, nB, TryU32(base, a + 4),
                           TryU32(base, b + 4), callers);
  }
  const uint32_t ped = PedType();
  const bool pedA = tA == ped || pA == ped, pedB = tB == ped || pB == ped;
  if (after <= before || after > 101 || pedA == pedB) return;
  uint32_t skipped = 0;
  if (!FirstPassAllow(g_pcpair_limit, skipped)) return;
  const uint64_t onfoot = g_onfoot_ms.load();
  const long long age = onfoot ? static_cast<long long>(GetTickCount64() - onfoot) : -1;
  char contact[97];
  HexAt(base, buf + 128 + 128 * before, contact);
  rex::audio_trace::line("PCPAIR", "%08X\t%08X\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%lld\t%u\t%s", a, b, tA, tB,
                         pA, pB, nA, nB, TryU32(base, a, 0) >> 16, TryU32(base, b, 0) >> 16, before, after, ret, age,
                         skipped, contact);
  // The two records' part 0 (224-byte parts), first 192 bytes each: the run finds the world position in them and
  // follows the player and the ped over the contacts (does the player stop, or does the ped give way).
  char a0[97], a1[97], a2[97], a3[97], b0[97], b1[97], b2[97], b3[97];
  HexAt(base, a, a0);
  HexAt(base, a + 48, a1);
  HexAt(base, a + 96, a2);
  HexAt(base, a + 144, a3);
  HexAt(base, b, b0);
  HexAt(base, b + 48, b1);
  HexAt(base, b + 96, b2);
  HexAt(base, b + 144, b3);
  rex::audio_trace::line("PCPOS", "%08X\t%08X\t%s%s%s%s\t%s%s%s%s", a, b, a0, a1, a2, a3, b0, b1, b2, b3);
}

extern "C" REX_FUNC(sub_82D90148) {
  if (!On("pedbody")) {
    __imp__sub_82D90148(ctx, base);
    return;
  }
  static std::mutex limit_mutex;
  static uint64_t second = 0, heartbeat = 0;
  static uint32_t lines = 0, skipped = 0;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr), w = ctx.r3.u32;
  const uint64_t now = GetTickCount64();
  if (lr == 0x82D32ABC) g_onfoot_ms.store(now);
  const uint32_t ch = TryU32(base, w + 8, 0);
  const uint32_t holder = ch ? TryU32(base, ch + 6448, 0) : 0;
  const uint32_t c = holder ? TryU32(base, holder + 160, 0) : 0;
  float r[8];
  float strongest = 0;
  for (int i = 0; i < 8; ++i) {
    r[i] = c ? TryF32(base, c + 880 + 4 * i) : NAN;
    if (r[i] > strongest) strongest = r[i];
  }
  __imp__sub_82D90148(ctx, base);
  const uint32_t b20 = ByteAt(base, w + 20), b21 = ByteAt(base, w + 21), b27 = ByteAt(base, w + 27),
                 b38 = ByteAt(base, w + 38);
  const bool event = strongest > 0 || b20 || b21 || b27 || b38;
  uint32_t skip = 0;
  {
    std::lock_guard<std::mutex> lock(limit_mutex);
    const bool beat = now - heartbeat >= 1000;
    if (!event && !beat) return;
    if (now / 1000 != second) {
      second = now / 1000;
      lines = 0;
    }
    if (lines >= 30) {
      ++skipped;
      return;
    }
    ++lines;
    if (beat) heartbeat = now;
    skip = skipped;
    skipped = 0;
  }
  float v[3], d[3];
  for (int i = 0; i < 3; ++i) {
    v[i] = ch ? TryF32(base, ch + 16336 + 4 * i, 0) : 0;
    d[i] = ch ? TryF32(base, ch + 16272 + 4 * i, 0) : 0;
  }
  rex::audio_trace::line("WIPEOB", "%08X\t%08X\t%08X\t%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\t%.4f\t%.4f\t%.4f\t%u %u %u %u\t%u\t%u",
                         lr, w, ch, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                         v[0] * v[0] + v[1] * v[1] + v[2] * v[2], std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]),
                         ch ? TryF32(base, ch + 16384) : NAN, b20, b21, b27, b38, TryU32(base, w + 200, 0), skip);
}
