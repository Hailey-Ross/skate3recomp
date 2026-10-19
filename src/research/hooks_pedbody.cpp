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
//   PCPOS <ms> partsA | partsB | part 0 bytes 0..191 of A (hex) | of B (hex) | A +192..+207 (4 floats) | B +192..+207
//               (with every PCPAIR line)
// tA / tB = record word +0xC, pA / pB = part 0 word +212 (the pair table row); the run shows which one is the group.
// The on-foot wipeout check sub_82D90148 (r3 = the request block W, character = W+8): its inputs and the request
// bytes it set (W+20 contact, +21 displacement, +27 vehicle, +38 squash).
//   WIPEOB <ms> lr | W | character | region strengths C+880..C+908 | speed^2 | displacement | squash |
//               bytes 20 21 27 38 | counter W+200 | skipped
// Logged when a region strength is above zero or a byte is set (30 lines/s) plus a 1 Hz heartbeat.
// ---- Pedestrian avoidance of moving skaters (the SkaterAvoidance nav modifier at ped+2832) ----
//   AVOIDK    <ms> sense_r2 | min_speed | t_min | probe | full | half   (once: the image constants the rule reads)
//   AVOIDCPA  <ms> avoider | ped pos | mover | mover pos | mover vel | speed | t | inwin | miss | lookahead | radius |
//             hit   sub_82E2E738 (closest approach); inputs from its sub_82E15A58 call (lr 0x82E2E7CC). t = +0x34,
//             miss = sqrt(+0x30) (only written when 0 < t < lookahead), lookahead = +0x44, radius = +0x38.
//   AVOIDTEST <ms> avoider | owner | mover | f30..f4C | escape | iface | getter | type | flag3937 | jumpgate | probe |
//             angle | b5C before | after | skipped   sub_82E2E890 (decide + submit). type / flag3937 from the mover's
//             interface 0x82586478 slot 0x14 result, read only when that slot is a one-instruction getter.
//             probe = sub_82C46460 result (lr 0x82E2EA94), angle = [r3] of sub_8296EC98 (lr 0x82E2EB18).
//   AVOID     <ms> avoider | ped | jump/step | dir | ped+5916 | ped+5936 | jumpq | stepq | lr | callers
//             sub_82E36B58 (jump) / sub_82E36C18 (step) entry; dir from sub_825494B0 (lr 0x82E2EB98 / 0x82E2EBAC).
// ---- Player position, physical state and board wipeout requests ----
//   PLAYERPOS <ms> p | state | pos (p+240) | vel (p+400) | speed   (state selector sub_82D8ADE8, 100 ms per skater)
//   PSTATE    <ms> p | prev | cur | suggested | pos   (every change of the current state id per skater)
//   WIPEREQ   <ms> where | lr | W | ch | p | state | bytes 0x14..0x27 before | after | count before | after |
//             cooldown | squash | disp | regions | heartbeat   ground check sub_82D8F9E0 (G) / air check sub_82D90358
//             (A); request byte W+0x14+reason, count W+200. On change (30/s) plus a 1 Hz heartbeat per check.
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

void Vec4Text(uint8_t* base, uint32_t addr, char* out, size_t n) {
  std::snprintf(out, n, "%.4f %.4f %.4f %.4f", TryF32(base, addr), TryF32(base, addr + 4), TryF32(base, addr + 8),
                TryF32(base, addr + 12));
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
  char ta[64], tb[64];
  Vec4Text(base, a + 192, ta, sizeof ta);
  Vec4Text(base, b + 192, tb, sizeof tb);
  rex::audio_trace::line("PCPOS", "%08X\t%08X\t%s%s%s%s\t%s%s%s%s\t%s\t%s", a, b, a0, a1, a2, a3, b0, b1, b2, b3, ta, tb);
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

// ---- Pedestrian avoidance ----
namespace {
struct CpaInputs {
  bool valid = false;
  char ped[48] = {}, pos[48] = {}, vel[48] = {};
  float speed = NAN;
};
thread_local CpaInputs g_cpa;
thread_local uint32_t g_avoider = 0;  // non-zero while sub_82E2E890 runs on this thread
thread_local uint32_t g_dir = 0;      // char* of the direction name passed to sub_825494B0
thread_local int g_probe = -1;        // sub_82C46460 result inside sub_82E2E890
thread_local float g_angle = NAN;     // sub_8296EC98 result inside sub_82E2E890
std::atomic<bool> g_avoidk{false};

uint32_t Follow(uint8_t* base, uint32_t ptr, uint32_t offset) {
  if (ptr == 0 || ptr == 0xFFFFFFFFu) return 0xFFFFFFFFu;
  return TryU32(base, ptr + offset);
}

// sub_82965630 without calling it (copy of hooks_props.cpp FindInterface): 8-byte (type, interface) pairs.
uint32_t FindInterface(uint8_t* base, uint32_t map, uint32_t type) {
  const uint32_t begin = Follow(base, map, 0), end = Follow(base, map, 4);
  if (begin == 0xFFFFFFFFu || end == 0xFFFFFFFFu || end < begin || end - begin > 8 * 4096) return 0xFFFFFFFFu;
  for (uint32_t e = begin; e < end; e += 8) {
    if (TryU32(base, e) == type) return TryU32(base, e + 4);
  }
  return 0;
}

// The result of a guest getter `lwz r3,d(r3); blr` or `addi r3,r3,d; blr` for `self`, without running guest code.
// 0xFFFFFFFF when the function is anything else (or its words are not readable).
// Also follows the forwarding form `lwz r3,d(r3); lwz r11,0(r3); lwz r10,n(r11); mtctr r10; bctr` (the mover's
// slot 0x14 is sub_8258E8D0: d = 1764, n = 92), up to four hops.
uint32_t GetterResult(uint8_t* base, uint32_t fn, uint32_t self, int depth = 0) {
  const uint32_t i0 = TryU32(base, fn, 0), i1 = TryU32(base, fn + 4, 0);
  if ((i0 & 0xFFFF0000u) == 0x80630000u && i1 == 0x81630000u && depth < 4 &&
      (TryU32(base, fn + 8, 0) & 0xFFFF0000u) == 0x814B0000u && TryU32(base, fn + 12, 0) == 0x7D4903A6u &&
      TryU32(base, fn + 16, 0) == 0x4E800420u) {
    const uint32_t next = TryU32(base, self + static_cast<int16_t>(i0 & 0xFFFF));
    const uint32_t slot = static_cast<uint16_t>(TryU32(base, fn + 8, 0) & 0xFFFF);
    return GetterResult(base, Follow(base, Follow(base, next, 0), slot), next, depth + 1);
  }
  if (i1 != 0x4E800020u) return 0xFFFFFFFFu;
  const int32_t d = static_cast<int16_t>(i0 & 0xFFFF);
  if ((i0 & 0xFFFF0000u) == 0x80630000u) return TryU32(base, self + d);
  if ((i0 & 0xFFFF0000u) == 0x38630000u) return self + d;
  return 0xFFFFFFFFu;
}

float Len(uint8_t* base, uint32_t v) {
  const float x = TryF32(base, v), y = TryF32(base, v + 4), z = TryF32(base, v + 8);
  return std::sqrt(x * x + y * y + z * z);
}
}  // namespace

extern "C" REX_FUNC(sub_82E15A58) {
  if (static_cast<uint32_t>(ctx.lr) == 0x82E2E7CCu && On("pedbody")) {
    g_cpa.valid = true;
    Vec3Text(base, ctx.r6.u32, g_cpa.ped, sizeof g_cpa.ped);
    Vec3Text(base, ctx.r4.u32, g_cpa.pos, sizeof g_cpa.pos);
    Vec3Text(base, ctx.r5.u32, g_cpa.vel, sizeof g_cpa.vel);
    g_cpa.speed = Len(base, ctx.r5.u32);
  }
  __imp__sub_82E15A58(ctx, base);
}

extern "C" REX_FUNC(sub_82E2E738) {
  if (!On("pedbody")) {
    __imp__sub_82E2E738(ctx, base);
    return;
  }
  const uint32_t a = ctx.r3.u32;
  g_cpa.valid = false;
  __imp__sub_82E2E738(ctx, base);
  const uint32_t hit = ctx.r3.u32 & 0xFF;
  if (!g_avoidk.exchange(true)) {
    rex::audio_trace::line("AVOIDK", "%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f", TryF32(base, 0x820ED57Cu),
                           TryF32(base, 0x8231A844u), TryF32(base, 0x82165A10u), TryF32(base, 0x820641A8u),
                           TryF32(base, 0x820B411Cu), TryF32(base, 0x82060C44u));
  }
  static FirstPassLimit hits, misses;
  uint32_t skipped = 0;
  if (!g_cpa.valid || !FirstPassAllow(hit ? hits : misses, skipped)) return;
  const float t = TryF32(base, a + 0x34), look = TryF32(base, a + 0x44);
  const bool inwin = t < look && t > TryF32(base, 0x82165A10u);
  char miss[24] = "-";
  if (inwin) std::snprintf(miss, sizeof miss, "%.3f", std::sqrt(TryF32(base, a + 0x30)));
  rex::audio_trace::line("AVOIDCPA", "%08X\t%s\t%08X\t%s\t%s\t%.3f\t%.3f\t%u\t%s\t%.3f\t%.3f\t%u", a, g_cpa.ped,
                         TryU32(base, a + 0x54), g_cpa.pos, g_cpa.vel, g_cpa.speed, t, inwin ? 1u : 0u, miss, look,
                         TryF32(base, a + 0x38), hit);
}

extern "C" REX_FUNC(sub_82E2E890) {
  if (!On("pedbody")) {
    __imp__sub_82E2E890(ctx, base);
    return;
  }
  const uint32_t a = ctx.r3.u32;
  static FirstPassLimit limit;
  uint32_t skipped = 0;
  const bool log = FirstPassAllow(limit, skipped);
  char floats[160] = {}, escape[48] = {};
  uint32_t owner = 0, mover = 0, iface = 0xFFFFFFFFu, getter = 0xFFFFFFFFu, type = 0xFFFFFFFFu, flag = 0xFFFFFFFFu;
  uint32_t before = 0, jumpgate = 0;
  if (log) {
    float f[8];
    for (int i = 0; i < 8; ++i) f[i] = TryF32(base, a + 0x30 + 4 * i);
    std::snprintf(floats, sizeof floats, "%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f", f[0], f[1], f[2], f[3], f[4], f[5],
                  f[6], f[7]);
    Vec3Text(base, a + 0x20, escape, sizeof escape);
    owner = TryU32(base, a + 0x50);
    mover = TryU32(base, a + 0x54);
    if (mover != 0 && mover != 0xFFFFFFFFu) iface = FindInterface(base, mover + 4, 0x82586478u);
    if (iface != 0 && iface != 0xFFFFFFFFu) {
      getter = Follow(base, Follow(base, iface, 0), 0x14);
      const uint32_t r = GetterResult(base, getter, iface);
      if (r != 0xFFFFFFFFu) {
        type = Follow(base, Follow(base, r, 64), 0);
        const uint32_t fo = Follow(base, r, 60);
        if (fo != 0xFFFFFFFFu) flag = ByteAt(base, fo + 14647);
      }
    }
    jumpgate = (type != 6 && flag == 0 && f[0] < f[3] * f[3]) ? 1u : 0u;  // speed term: see AVOIDCPA speed vs f[7]
    before = ByteAt(base, a + 0x5C);
  }
  g_avoider = a;
  g_dir = 0;
  g_probe = -1;
  g_angle = NAN;
  __imp__sub_82E2E890(ctx, base);
  g_avoider = 0;
  if (!log) return;
  char probe[8] = "-", angle[24] = "-";
  if (g_probe >= 0) std::snprintf(probe, sizeof probe, "%d", g_probe);
  if (!std::isnan(g_angle)) std::snprintf(angle, sizeof angle, "%.4f", g_angle);
  rex::audio_trace::line("AVOIDTEST", "%08X\t%08X\t%08X\t%s\t%s\t%08X\t%08X\t%d\t%d\t%u\t%s\t%s\t%02X\t%02X\t%u", a, owner,
                         mover, floats, escape, iface, getter, static_cast<int>(type), static_cast<int>(flag), jumpgate,
                         probe, angle, before, ByteAt(base, a + 0x5C), skipped);
}

// Shared helpers with many callers: one compare unless sub_82E2E890 runs on this thread.
extern "C" REX_FUNC(sub_825494B0) {
  if (g_avoider != 0) {
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    if (lr == 0x82E2EB98u || lr == 0x82E2EBACu) g_dir = ctx.r4.u32;
  }
  __imp__sub_825494B0(ctx, base);
}

extern "C" REX_FUNC(sub_82C46460) {
  const bool mine = g_avoider != 0 && static_cast<uint32_t>(ctx.lr) == 0x82E2EA94u;
  __imp__sub_82C46460(ctx, base);
  if (mine) g_probe = static_cast<int>(ctx.r3.u32);
}

extern "C" REX_FUNC(sub_8296EC98) {
  const bool mine = g_avoider != 0 && static_cast<uint32_t>(ctx.lr) == 0x82E2EB18u;
  __imp__sub_8296EC98(ctx, base);
  if (mine) g_angle = TryF32(base, ctx.r3.u32);
}

namespace {
void LogAvoid(PPCContext& ctx, uint8_t* base, const char* kind) {
  static FirstPassLimit limit;
  uint32_t skipped = 0;
  if (!FirstPassAllow(limit, skipped)) return;
  const uint32_t ped = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t controller = TryU32(base, ped + 5888);
  const bool from890 = g_avoider != 0 && (lr == 0x82E2EBA4u || lr == 0x82E2EBB8u);
  char dir[33] = {'-', 0};
  if (from890 && g_dir != 0) NameAt(base, g_dir, dir);
  char callers[64] = "-";
  CallerChain(ctx, base, callers);
  const bool ok = controller != 0 && controller != 0xFFFFFFFFu;
  rex::audio_trace::line("AVOID", "%08X\t%08X\t%s\t%s\t%08X\t%08X\t%d\t%d\t%08X\t%s", from890 ? g_avoider : 0u, ped,
                         kind, dir, TryU32(base, ped + 5916), TryU32(base, ped + 5936),
                         ok ? static_cast<int>(ByteAt(base, controller + 1416)) : -1,
                         ok ? static_cast<int>(ByteAt(base, controller + 1880)) : -1, lr, callers);
}
}  // namespace

extern "C" REX_FUNC(sub_82E36B58) {
  if (On("pedbody")) LogAvoid(ctx, base, "jump");
  __imp__sub_82E36B58(ctx, base);
}

extern "C" REX_FUNC(sub_82E36C18) {
  if (On("pedbody")) LogAvoid(ctx, base, "step");
  __imp__sub_82E36C18(ctx, base);
}

// ---- Player position and physical state (called from the sub_82D8ADE8 hook in hooks_vehicles.cpp) ----
namespace {
struct SkaterSeen {
  uint64_t last_pos = 0;
  uint32_t cur = 0;
  bool seen = false;
};
std::mutex g_seen_mutex;
std::unordered_map<uint32_t, SkaterSeen> g_seen;  // p -> last line / state

bool StateOf(uint32_t p, uint32_t& state) {
  std::lock_guard<std::mutex> lock(g_seen_mutex);
  auto it = g_seen.find(p);
  if (it == g_seen.end() || !it->second.seen) return false;
  state = it->second.cur;
  return true;
}
}  // namespace

namespace skate3_research {
void PedbodySelector(uint8_t* base, uint32_t sel, uint32_t cur, uint32_t result) {
  const uint32_t p = TryU32(base, sel + 4, 0);
  if (!Readable(base, p, 416)) return;
  const uint64_t now = GetTickCount64();
  bool pos_line = false, changed = false, first = false;
  uint32_t prev = 0;
  {
    std::lock_guard<std::mutex> lock(g_seen_mutex);
    SkaterSeen& s = g_seen[p];
    first = !s.seen;
    changed = first || s.cur != cur;
    prev = s.cur;
    s.cur = cur;
    s.seen = true;
    if (now - s.last_pos >= 100) {
      s.last_pos = now;
      pos_line = true;
    }
  }
  if (!pos_line && !changed) return;
  char pos[48];
  Vec3Text(base, p + 240, pos, sizeof pos);
  if (changed) {
    char prev_text[16] = "-";
    if (!first) std::snprintf(prev_text, sizeof prev_text, "%u", prev);
    rex::audio_trace::line("PSTATE", "%08X\t%s\t%u\t%u\t%s", p, prev_text, cur, result, pos);
  }
  if (pos_line) {
    char vel[48];
    Vec3Text(base, p + 400, vel, sizeof vel);
    rex::audio_trace::line("PLAYERPOS", "%08X\t%u\t%s\t%s\t%.3f", p, cur, pos, vel, Len(base, p + 400));
  }
}
}  // namespace skate3_research

// ---- Board wipeout requests: ground check sub_82D8F9E0, air check sub_82D90358 (r3 = W) ----
namespace {
void Bytes20(uint8_t* base, uint32_t w, char (&out)[41]) {
  for (int i = 0; i < 5; ++i) std::snprintf(out + 8 * i, 9, "%08X", TryU32(base, w + 0x14 + 4 * i, 0));
}

void WipeCheck(PPCContext& ctx, uint8_t* base, char where, void (*imp)(PPCContext&, uint8_t*)) {
  if (!On("pedbody")) {
    imp(ctx, base);
    return;
  }
  const uint32_t lr = static_cast<uint32_t>(ctx.lr), w = ctx.r3.u32;
  char before[41], after[41];
  Bytes20(base, w, before);
  const uint32_t count_before = TryU32(base, w + 200, 0);
  const float cooldown = TryF32(base, w + 0xC0);
  imp(ctx, base);
  Bytes20(base, w, after);
  const uint32_t count_after = TryU32(base, w + 200, 0);
  const bool event = count_after != count_before || std::strcmp(before, after) != 0;
  static std::mutex m;
  static uint64_t beat[2] = {0, 0}, second = 0;
  static uint32_t lines = 0;
  const uint64_t now = GetTickCount64();
  const int k = where == 'G' ? 0 : 1;
  bool heartbeat = false;
  {
    std::lock_guard<std::mutex> lock(m);
    heartbeat = now - beat[k] >= 1000;
    if (!event && !heartbeat) return;
    if (now / 1000 != second) {
      second = now / 1000;
      lines = 0;
    }
    if (!event && lines >= 30) return;  // requests are never dropped
    ++lines;
    if (heartbeat) beat[k] = now;
  }
  const uint32_t ch = TryU32(base, w + 8, 0), p = TryU32(base, w + 0x10, 0);
  const uint32_t holder = ch ? TryU32(base, ch + 6448, 0) : 0;
  const uint32_t c = holder ? TryU32(base, holder + 160, 0) : 0;
  char regions[128];
  float r[8];
  for (int i = 0; i < 8; ++i) r[i] = c ? TryF32(base, c + 880 + 4 * i) : NAN;
  std::snprintf(regions, sizeof regions, "%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f", r[0], r[1], r[2], r[3], r[4], r[5],
                r[6], r[7]);
  uint32_t state = 0;
  char state_text[16] = "-";
  if (p && StateOf(p, state)) std::snprintf(state_text, sizeof state_text, "%u", state);
  rex::audio_trace::line("WIPEREQ", "%c\t%08X\t%08X\t%08X\t%08X\t%s\t%s\t%s\t%u\t%u\t%.4f\t%.4f\t%.4f\t%s\t%u", where, lr,
                         w, ch, p, state_text, before, after, count_before, count_after, cooldown,
                         ch ? TryF32(base, ch + 0x4000) : NAN, ch ? Len(base, ch + 0x3F90) : NAN, regions,
                         heartbeat ? 1u : 0u);
}
}  // namespace

extern "C" REX_FUNC(sub_82D8F9E0) { WipeCheck(ctx, base, 'G', __imp__sub_82D8F9E0); }
extern "C" REX_FUNC(sub_82D90358) { WipeCheck(ctx, base, 'A', __imp__sub_82D90358); }
