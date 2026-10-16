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
