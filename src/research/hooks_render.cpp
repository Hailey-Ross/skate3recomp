// Research hooks (tracing tools; not intended for the upstream project): world render order and the writer of the
// g_ViewDotLight shader constant. Category: SKATE3_TRACE=render (not in the default launcher lists).
//
// A, draw order of one frame (armed SKATE3_DRAWORDER_DELAY_MS, default 10000, after the first transparentenvironment
// effect apply; records exactly one frame between two swaps, capped at 30000 applies):
//   DOFRAME  <ms> start|end|capped <seq> <ms since previous boundary>
//   DOLIST   <ms> <seq> <view> <list offset> <first> <count> <first key> <last key> <callers>
//   DOSEEN   <ms> <vtable> <fence|->   (first 16 distinct effect instance vtables, before recording)
//   DOAPPLY  <ms> <seq> <tid> <instance> <vtable> <bind> <depth index> <blend index> <raster index> <technique> <vs>
//            <callers>
// Effect instance apply = vtable slot 3 sub_82CA5C60 (r3 instance, r4 low byte = bind render states). State indices
// are the positions of the instance's state pointers (+88 depth, +92 blend, +96 raster) in the tables filled by
// sub_82CF5A10. Draw lists come from SceneRenderView::Render (sub_827FB158) via sub_827FAF50, hooked in
// skate3_native_render.cpp, which calls DrawOrderList; the swap hook there calls RenderFrameBoundary.
//
// B, who writes g_ViewDotLight (float4 at *(0x83083C60) + 0x44370, bound by name hash 0xE552F1C3 in sub_826DD6B8;
// .x changes with the view): each probed function compares the 16 bytes before and after it runs and logs
//   VDLWRITE <ms> <function> <G> <before x y z w> <after x y z w> <lr> <callers> <tid> <lines left>
// at most 6 times (the innermost probed writer exits first), plus a 1 Hz
//   VDLVAL <ms> <G> <x y z w> <frames changed>
// from the swap hook, at most 30 lines.
#include "trace_common.h"

using namespace skate3_research;

namespace {

constexpr uint32_t kTransparentEnvironmentVtable = 0x82325C8Cu;
constexpr uint32_t kViewPointer = 0x83083C60u;
constexpr uint32_t kViewDotLight = 0x44370u;
constexpr uint32_t kDepthTable = 0x8307DDA0u;
constexpr uint32_t kBlendTable = 0x8307DE80u;
constexpr uint32_t kRasterTable = 0x830782DCu;

std::atomic<int> g_do_state{0};  // 0 idle, 1 waiting for frame start, 2 recording, 3 done
std::atomic<uint32_t> g_do_seq{0};
std::atomic<uint64_t> g_do_world_ms{0};
uint64_t g_do_boundary_ms = 0;

std::atomic<int> g_vdl_left{6};
int g_vdl_values = 0;
int g_vdl_changes = 0;
uint64_t g_vdl_last_ms = 0;
uint8_t g_vdl_previous[16] = {};
bool g_vdl_have_previous = false;

uint64_t DelayMs() {
  static const uint64_t delay = [] {
    const char* v = std::getenv("SKATE3_DRAWORDER_DELAY_MS");
    return v && *v ? std::strtoull(v, nullptr, 10) : 10000ull;
  }();
  return delay;
}

int TableIndex(uint8_t* base, uint32_t table, int count, uint32_t pointer) {
  if (!pointer) return -1;
  for (int i = 0; i < count; ++i) {
    if (TryU32(base, table + 4u * i, 0) == pointer) return i;
  }
  return -1;
}

void FloatText(const uint8_t* bytes, char (&out)[96]) {
  float v[4];
  for (int i = 0; i < 4; ++i) {
    const uint32_t w = (uint32_t(bytes[4 * i]) << 24) | (uint32_t(bytes[4 * i + 1]) << 16) |
                       (uint32_t(bytes[4 * i + 2]) << 8) | uint32_t(bytes[4 * i + 3]);
    std::memcpy(&v[i], &w, 4);
  }
  std::snprintf(out, sizeof out, "%.6g %.6g %.6g %.6g", v[0], v[1], v[2], v[3]);
}

bool VdlRead(uint8_t* base, uint8_t (&out)[16], uint32_t& view) {
  view = TryU32(base, kViewPointer, 0);
  if (!Plausible(view) || !Readable(base, view + kViewDotLight, 16)) return false;
  std::memcpy(out, base + view + kViewDotLight, 16);
  return true;
}

void VdlCheck(PPCContext& ctx, uint8_t* base, uint32_t function, const uint8_t (&before)[16], uint32_t view) {
  if (TryU32(base, kViewPointer, 0) != view || !Readable(base, view + kViewDotLight, 16)) return;
  const uint8_t* after = base + view + kViewDotLight;
  if (std::memcmp(before, after, 16) == 0) return;
  const int left = g_vdl_left.fetch_sub(1) - 1;
  if (left < 0) return;
  char b[96], a[96], chain[64];
  FloatText(before, b);
  FloatText(after, a);
  CallerChain(ctx, base, chain);
  rex::audio_trace::line("VDLWRITE", "%08X\t%08X\t%s\t%s\t%08X\t%s\t%lu\t%d", function, view, b, a,
                         static_cast<uint32_t>(ctx.lr), chain, GetCurrentThreadId(), left);
}

}  // namespace

namespace skate3_research {

// From the existing sub_827FAF50 hook (skate3_native_render.cpp), before the original runs.
void DrawOrderList(uint8_t* base, uint32_t view, uint32_t list, uint32_t first, uint32_t count, PPCContext& ctx) {
  if (g_do_state.load(std::memory_order_relaxed) != 2 || !On("render")) return;
  const uint32_t data = TryU32(base, list, 0);
  const uint32_t first_key = count ? TryU32(base, data + 8u * first, 0) : 0;
  const uint32_t last_key = count ? TryU32(base, data + 8u * (first + count - 1), 0) : 0;
  char chain[64];
  CallerChain(ctx, base, chain);
  rex::audio_trace::line("DOLIST", "%u\t%08X\t%d\t%u\t%u\t%08X\t%08X\t%s", g_do_seq.load(), view,
                         static_cast<int>(list - view), first, count, first_key, last_key, chain);
}

// From the existing swap hook sub_82B82E08 (skate3_native_render.cpp), before the original runs.
void RenderFrameBoundary(uint8_t* base) {
  if (!On("render")) return;
  const uint64_t now = GetTickCount64();
  const int state = g_do_state.load();
  const uint64_t world = g_do_world_ms.load();
  if (state == 0 && world && now - world >= DelayMs()) {
    g_do_state.store(1);
  } else if (state == 1) {
    g_do_seq.store(0);
    g_do_state.store(2);
    rex::audio_trace::line("DOFRAME", "start\t%u\t%llu", 0u, now - g_do_boundary_ms);
  } else if (state == 2) {
    g_do_state.store(3);
    rex::audio_trace::line("DOFRAME", "end\t%u\t%llu", g_do_seq.load(), now - g_do_boundary_ms);
  }
  g_do_boundary_ms = now;

  if (g_vdl_left.load(std::memory_order_relaxed) > 0 && g_vdl_values < 30) {
    uint8_t value[16];
    uint32_t view = 0;
    if (VdlRead(base, value, view)) {
      if (g_vdl_have_previous && std::memcmp(value, g_vdl_previous, 16) != 0) ++g_vdl_changes;
      std::memcpy(g_vdl_previous, value, 16);
      g_vdl_have_previous = true;
      if (now - g_vdl_last_ms >= 1000) {
        g_vdl_last_ms = now;
        ++g_vdl_values;
        char text[96];
        FloatText(value, text);
        rex::audio_trace::line("VDLVAL", "%08X\t%s\t%d", view, text, g_vdl_changes);
      }
    }
  }
}

}  // namespace skate3_research

// Effect instance apply (vtable slot 3, shared by the world effect instances).
extern "C" REX_FUNC(sub_82CA5C60) {
  if (g_do_state.load(std::memory_order_relaxed) == 2 && On("render")) {
    const uint32_t seq = g_do_seq.fetch_add(1);
    if (seq >= 30000) {
      int expected = 2;
      if (g_do_state.compare_exchange_strong(expected, 3)) {
        rex::audio_trace::line("DOFRAME", "capped\t%u\t%llu", seq, GetTickCount64() - g_do_boundary_ms);
      }
    } else {
      const uint32_t inst = ctx.r3.u32;
      char chain[64];
      CallerChain(ctx, base, chain);
      rex::audio_trace::line(
          "DOAPPLY", "%u\t%lu\t%08X\t%08X\t%u\t%d\t%d\t%d\t%08X\t%08X\t%s", seq, GetCurrentThreadId(), inst,
          TryU32(base, inst, 0), ctx.r4.u32 & 0xFFu, TableIndex(base, kDepthTable, 23, TryU32(base, inst + 88, 0)),
          TableIndex(base, kBlendTable, 31, TryU32(base, inst + 92, 0)),
          TableIndex(base, kRasterTable, 5, TryU32(base, inst + 96, 0)), TryU32(base, inst + 84, 0),
          TryU32(base, inst + 144, 0), chain);
    }
  } else if (g_do_state.load(std::memory_order_relaxed) < 2 && On("render")) {
    // Arm on the first apply of any effect (the fence effect alone did not arm in the 2026-10-09 check run), and
    // name the first 16 distinct instance vtables so the run shows which effects are applied.
    const uint32_t vtbl = TryU32(base, ctx.r3.u32, 0);
    if (g_do_world_ms.load(std::memory_order_relaxed) == 0) g_do_world_ms.store(GetTickCount64());
    static std::mutex seen_lock;
    static uint32_t seen[16] = {};
    static int seen_count = 0;
    std::lock_guard<std::mutex> guard(seen_lock);
    if (seen_count < 16 && std::find(seen, seen + seen_count, vtbl) == seen + seen_count) {
      seen[seen_count++] = vtbl;
      rex::audio_trace::line("DOSEEN", "%08X	%s", vtbl, vtbl == kTransparentEnvironmentVtable ? "fence" : "-");
    }
  }
  __imp__sub_82CA5C60(ctx, base);
}

#define VDL_PROBE(addr)                                                                               \
  extern "C" REX_FUNC(sub_##addr) {                                                                   \
    uint8_t before[16];                                                                               \
    uint32_t view = 0;                                                                                \
    const bool armed =                                                                                \
        g_vdl_left.load(std::memory_order_relaxed) > 0 && On("render") && VdlRead(base, before, view); \
    __imp__sub_##addr(ctx, base);                                                                     \
    if (armed) VdlCheck(ctx, base, 0x##addr##u, before, view);                                        \
  }

// Renderer (SceneRenderView, vtable 0x823120F0) methods, slots 2..6, 8, 9, 14, 16, 17, 20, 22..24, 26, 27, and the
// view matrix writer sub_82528208.
VDL_PROBE(827FA460)
VDL_PROBE(827FA5E8)
VDL_PROBE(827FA7F0)
VDL_PROBE(827FAAA0)
VDL_PROBE(827FB158)
VDL_PROBE(827F9A58)
VDL_PROBE(827F9C28)
VDL_PROBE(827FBBA0)
VDL_PROBE(827FBDC8)
VDL_PROBE(827FC4D0)
VDL_PROBE(827FCB20)
VDL_PROBE(827FCC60)
VDL_PROBE(827FD0E0)
VDL_PROBE(827FD170)
VDL_PROBE(82A0E258)
VDL_PROBE(8280E618)
VDL_PROBE(82528208)

// Second round (2026-10-09: the value changed every frame but none of the above wrote it): the callees of Render and
// the callers of the view matrix writer sub_82528208 (sub_825D4E50 is hooked elsewhere and is not probed).
VDL_PROBE(827FA9A8)
VDL_PROBE(827FAB50)
VDL_PROBE(82B7F6C8)
VDL_PROBE(825D4D48)
VDL_PROBE(825DF618)
VDL_PROBE(825E2DF0)
VDL_PROBE(825E31F8)
VDL_PROBE(8267FDE0)
VDL_PROBE(826D2EB0)
VDL_PROBE(826D67F0)
VDL_PROBE(8275A0D8)
VDL_PROBE(8277F838)
VDL_PROBE(82784298)
VDL_PROBE(8278F4C0)
VDL_PROBE(8278F788)
VDL_PROBE(827A0A10)
VDL_PROBE(827A6D78)
VDL_PROBE(827B6490)
VDL_PROBE(827E39F0)
VDL_PROBE(827F7270)
VDL_PROBE(827F7928)
VDL_PROBE(827F82F8)
VDL_PROBE(827FE770)
VDL_PROBE(827FE8D8)
VDL_PROBE(827FEFD0)
VDL_PROBE(827FF1D0)
VDL_PROBE(827FF7D0)
VDL_PROBE(82805938)
VDL_PROBE(8280AD00)
VDL_PROBE(8280AF30)

// Third round (2026-10-09): VDLWRITE named sub_827FF7D0 (the renderer per-frame update) but a static read found
// no store there; probe its remaining direct callees.
VDL_PROBE(824AD2F0)
VDL_PROBE(8256AA10)
VDL_PROBE(825E7108)
VDL_PROBE(826DCA78)
VDL_PROBE(82758B38)
VDL_PROBE(827873B0)
VDL_PROBE(82789420)
VDL_PROBE(82799418)
VDL_PROBE(8279E748)
VDL_PROBE(827AFC80)
VDL_PROBE(827B5E60)
VDL_PROBE(827B6320)
VDL_PROBE(827E8D68)
VDL_PROBE(827EDDB8)
VDL_PROBE(827F53B0)
VDL_PROBE(82800FF8)
VDL_PROBE(82801940)
VDL_PROBE(82801990)
VDL_PROBE(828F44E8)
VDL_PROBE(82CFC870)

// Fourth round: VDLWRITE named sub_82800FF8 (render-location parameter copy, no float maths); its callees.
VDL_PROBE(82800F20)
VDL_PROBE(827E7280)
VDL_PROBE(826DE7D8)
VDL_PROBE(827F9C60)
VDL_PROBE(827FFE30)
VDL_PROBE(827A0C70)
VDL_PROBE(828012D0)
