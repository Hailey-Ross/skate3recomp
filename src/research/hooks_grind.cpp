// Research hooks (tracing tools; not intended for the upstream project): grind manager, board contact results and the
// grind Post closing check, to see what the board touches while grinding / sliding and what the Post decides.
// Category: SKATE3_TRACE=grind (src/research/trace_common.h).
//
// Objects: p = processed physics input (GroundAnimation fill 82D34150: self+16; the grind manager 82D8A828: r3+0);
// b = skateboard (82D34150: self+4; the board contact pass 82C07D20: r3). The fill hook records b -> p so the board
// lines can be gated on p's state word +2508 (400..405 grind kinds, 701).
//
// Gate ("active"): p+2508 in 400..405 or 701, plus the next 30 ticks after it leaves (why g / l; needs the board -> p
// link, which the 82C07D20 board does not match in practice); or a touching part point of the board (why b, then k
// for 1.5 s) inside the box from env SKATE3_GRIND_BOX="xmin xmax zmin zmax" (world x / z). Outside the gate: BRDPOSE at most
// 2 per second per board (positions for orientation). Every read is guarded.
//
// Kinds (fields after ms):
//   GRMGR   <ms> M | p | 2508 | 2512 | +1296 owner | +1264 start | +1280 end | +1120 point | +1248 | +1252 |
//           +1516 hex | +1488 byte | p+112 position | p+400 velocity | why (g gate, l leaving)          (15 fields)
//           at the end of the manager pre-update 82D8A828
//   BRDPOSE <ms> b | p | 2508 | contacts w0 w1 w2 w3 ft bt deck | deck pos (toolkit [b+304]+112) | deck up (+80) |
//           deck fwd (+96) | +656 closing velocity | +852 max closing | +860 | +868 byte | 7 x part normal (+96+16i) |
//           7 x part point (+208+16i) | 7 x part old velocity (+320+16i) | why                          (33 fields)
//           after the board contact pass 82C07D20 (points / normals valid only for touching parts)
//   GRPOST  <ms> obj | [obj+4] | [obj+8] | f1 xz limit | f2 vertical limit | +20 before | +22 before | +20 after |
//           +22 after | +200 after | +64 after | +56 after                                             (12 fields)
//           around the Post closing check 82D90AB8, while any p is active, or always when +22 / +20 gets set
//   HOOKARMED <ms> kind 0 0 0 0 (first call of each hook; 5-field layout like the other hooks)
// "-" / nan where a read is not possible.
#include "trace_common.h"

#include <atomic>
#include <cstdlib>
#include <unordered_map>

using namespace skate3_research;

namespace {

std::mutex g_grind_mutex;
std::unordered_map<uint32_t, uint32_t> g_board_to_p;    // board -> processed input
std::unordered_map<uint32_t, int> g_p_after;            // p -> ticks left after leaving the grind states
std::unordered_map<uint32_t, uint64_t> g_pose_last;     // board -> last ungated BRDPOSE (ms)
std::unordered_map<uint32_t, uint64_t> g_box_until;     // board -> full rate until (ms), after a touch in the box
std::atomic<int> g_any_active{0};
std::atomic<bool> g_armed_mgr{false}, g_armed_pose{false}, g_armed_post{false}, g_armed_fill{false};

struct Box {
  bool set = false;
  float x0 = 0, x1 = 0, z0 = 0, z1 = 0;
};
const Box& GrindBox() {
  static const Box box = [] {
    Box b;
    const char* s = std::getenv("SKATE3_GRIND_BOX");
    if (s && std::sscanf(s, "%f %f %f %f", &b.x0, &b.x1, &b.z0, &b.z1) == 4) b.set = true;
    return b;
  }();
  return box;
}

void Armed(std::atomic<bool>& flag, const char* kind) {
  bool expected = false;
  // Same 5-field layout as the other hooks (hook lr r3 r4 r5; trace.py checks it); no registers here, so zeros.
  if (flag.compare_exchange_strong(expected, true))
    rex::audio_trace::line("HOOKARMED", "%s\t%08X\t%08X\t%08X\t%08X", kind, 0u, 0u, 0u, 0u);
}

bool GrindState(uint32_t s) { return (s >= 400 && s <= 405) || s == 701; }

void V3(uint8_t* base, uint32_t addr, char* out, size_t n) {
  if (Readable(base, addr, 12)) {
    Vec3Text(base, addr, out, n);
  } else {
    std::snprintf(out, n, "nan nan nan");
  }
}

}  // namespace

namespace skate3_research {

// Called from the GroundAnimation fill hook (82D34150, hooks_physics.cpp) with its r3.
void GrindNoteSkater(uint8_t* base, uint32_t self) {
  if (!On("grind") || !Readable(base, self, 20)) return;
  Armed(g_armed_fill, "GRFILL");
  const uint32_t board = LoadU32(base, self + 4), p = LoadU32(base, self + 16);
  if (!Plausible(board) || !Plausible(p)) return;
  std::lock_guard<std::mutex> lock(g_grind_mutex);
  g_board_to_p[board] = p;
}

// Called from the board contact pass hook (82C07D20, hooks_physics.cpp) after the pass, with the board.
void GrindBoardPass(uint8_t* base, uint32_t b) {
  if (!On("grind") || !Plausible(b)) return;
  Armed(g_armed_pose, "BRDPOSE");
  uint32_t p = 0;
  int after = 0;
  {
    std::lock_guard<std::mutex> lock(g_grind_mutex);
    auto it = g_board_to_p.find(b);
    if (it != g_board_to_p.end()) p = it->second;
    if (p) {
      auto a = g_p_after.find(p);
      if (a != g_p_after.end()) after = a->second;
    }
  }
  const uint32_t state = (p && Readable(base, p + 2508, 4)) ? LoadU32(base, p + 2508) : 0xFFFFFFFFu;
  const uint32_t tk = Readable(base, b + 304, 4) ? LoadU32(base, b + 304) : 0;
  const bool tk_ok = Plausible(tk) && Readable(base, tk + 80, 48);
  char why = 0;
  if (GrindState(state)) {
    why = 'g';
  } else if (after > 0) {
    why = 'l';
  } else if (GrindBox().set && Readable(base, b, 860)) {
    // The 82C07D20 board is not the fill's self+4, so p is usually unknown here: gate by place. A touching part
    // inside the box logs at full rate (b) and keeps the board at full rate for 1.5 s after (k: airborne parts
    // have no point).
    const Box& box = GrindBox();
    for (int i = 0; i < 7 && !why; ++i) {
      if (base[b + 844 + i] == 0) continue;
      const float x = LoadF32(base, b + 208 + 16 * i), z = LoadF32(base, b + 216 + 16 * i);
      if (x >= box.x0 && x <= box.x1 && z >= box.z0 && z <= box.z1) why = 'b';
    }
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_grind_mutex);
    uint64_t& until = g_box_until[b];
    if (why) {
      until = now + 1500;
    } else if (now < until) {
      why = 'k';
    }
  }
  if (!why) {
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_grind_mutex);
    uint64_t& last = g_pose_last[b];
    if (now - last < 500) return;
    last = now;
    why = 'r';
  }
  if (!Readable(base, b, 880)) return;
  char dp[64], du[64], df[64], cv[64];
  if (tk_ok) {
    Vec3Text(base, tk + 112, dp, sizeof dp);
    Vec3Text(base, tk + 80, du, sizeof du);
    Vec3Text(base, tk + 96, df, sizeof df);
  } else {
    std::snprintf(dp, sizeof dp, "nan nan nan");
    std::snprintf(du, sizeof du, "nan nan nan");
    std::snprintf(df, sizeof df, "nan nan nan");
  }
  Vec3Text(base, b + 656, cv, sizeof cv);
  char n[7][64], pt[7][64], v[7][64];
  for (int i = 0; i < 7; ++i) {
    Vec3Text(base, b + 96 + 16 * i, n[i], sizeof n[i]);
    Vec3Text(base, b + 208 + 16 * i, pt[i], sizeof pt[i]);
    Vec3Text(base, b + 320 + 16 * i, v[i], sizeof v[i]);
  }
  rex::audio_trace::line(
      "BRDPOSE",
      "%08X\t%08X\t%d\t%u%u%u%u%u%u%u\t%s\t%s\t%s\t%s\t%.4f\t%.4f\t%u\t"
      "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%c",
      b, p, static_cast<int>(state), base[b + 844] != 0, base[b + 845] != 0, base[b + 846] != 0,
      base[b + 847] != 0, base[b + 848] != 0, base[b + 849] != 0, base[b + 850] != 0, dp, du, df, cv,
      LoadF32(base, b + 852), LoadF32(base, b + 860), static_cast<unsigned>(base[b + 868]), n[0], n[1], n[2], n[3],
      n[4], n[5], n[6], pt[0], pt[1], pt[2], pt[3], pt[4], pt[5], pt[6], v[0], v[1], v[2], v[3], v[4], v[5], v[6],
      why);
}

}  // namespace skate3_research

// Grind manager pre-update (r3 = manager, [r3] = processed input p).
extern "C" REX_FUNC(sub_82D8A828) {
  const uint32_t m = ctx.r3.u32;
  __imp__sub_82D8A828(ctx, base);
  if (!On("grind") || !Plausible(m)) return;
  Armed(g_armed_mgr, "GRMGR");
  const uint32_t p = TryU32(base, m, 0);
  if (!Plausible(p) || !Readable(base, p, 2520)) return;
  const uint32_t s = LoadU32(base, p + 2508);
  char why;
  {
    std::lock_guard<std::mutex> lock(g_grind_mutex);
    int& after = g_p_after[p];
    if (GrindState(s)) {
      if (after == 0) g_any_active.fetch_add(1);
      after = 30;
      why = 'g';
    } else if (after > 0) {
      --after;
      if (after == 0) g_any_active.fetch_sub(1);
      why = 'l';
    } else {
      return;
    }
  }
  char st[64], en[64], pt[64], pos[64], vel[64];
  Vec3Text(base, p + 1264, st, sizeof st);
  Vec3Text(base, p + 1280, en, sizeof en);
  Vec3Text(base, p + 1120, pt, sizeof pt);
  Vec3Text(base, p + 112, pos, sizeof pos);
  Vec3Text(base, p + 400, vel, sizeof vel);
  rex::audio_trace::line("GRMGR", "%08X\t%08X\t%d\t%d\t%08X\t%s\t%s\t%s\t%08X\t%08X\t%08X\t%u\t%s\t%s\t%c", m, p,
                         static_cast<int>(s), static_cast<int>(LoadU32(base, p + 2512)), LoadU32(base, p + 1296), st,
                         en, pt, LoadU32(base, p + 1248), LoadU32(base, p + 1252), LoadU32(base, p + 1516),
                         static_cast<unsigned>(base[p + 1488]), pos, vel, why);
}

// Grind Post closing check (r3 = Post object, f1 xz limit, f2 vertical limit); sets +22 (and maybe +20) on a request.
extern "C" REX_FUNC(sub_82D90AB8) {
  const uint32_t o = ctx.r3.u32;
  const double f1 = ctx.f1.f64, f2 = ctx.f2.f64;
  const bool on = On("grind") && Plausible(o) && Readable(base, o, 204);
  uint8_t b20 = 0, b22 = 0;
  if (on) {
    Armed(g_armed_post, "GRPOST");
    b20 = base[o + 20];
    b22 = base[o + 22];
  }
  __imp__sub_82D90AB8(ctx, base);
  if (!on) return;
  const uint8_t a20 = base[o + 20], a22 = base[o + 22];
  const bool set = (a20 && !b20) || (a22 && !b22);
  if (!set && g_any_active.load() <= 0) return;
  rex::audio_trace::line("GRPOST", "%08X\t%08X\t%08X\t%.4f\t%.4f\t%u\t%u\t%u\t%u\t%u\t%.4f\t%.4f", o,
                         LoadU32(base, o + 4), LoadU32(base, o + 8), f1, f2, b20, b22, a20, a22,
                         LoadU32(base, o + 200), LoadF32(base, o + 64), LoadF32(base, o + 56));
}
