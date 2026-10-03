// Research hooks (tracing tools; not intended for the upstream project): a watch list of guest memory, so any global or
// object field can be inspected without writing a hook. Category: SKATE3_TRACE=watch.
//
// SKATE3_WATCH=<file>: one watch per line, `#` comments:
//   <label> <type> <address chain> [every=<ms>]
//   types: u8 u16 u32 s32 u64 f32 vec3 hex<N> (N bytes, 1..64)
//   chain: a hex start address, then steps: `@` = load u32 (pointer) at the current address, `+N` / `-N`
//          = add (hex 0x.. or decimal). Every load is guarded (Readable).
//   e.g.  wp_key  u64  0x83083C38 @ +0x2F0B0
//         mode    u32  0x830CFDC4 @ +892  every=1000
// Evaluated once per game frame (audio tick sub_82485190, after it runs). Logs
//   WATCH <ms> <label> <value>
// whenever the value changes, and also every <ms> when `every=` is given. Unreadable = "unreadable".
#include "trace_common.h"

#include <fstream>
#include <sstream>
#include <vector>

using namespace skate3_research;

namespace {

struct Step {
  bool deref;
  int64_t add;
};

struct Watch {
  std::string label;
  std::string type;
  uint32_t start = 0;
  std::vector<Step> steps;
  uint64_t every_ms = 0;
  std::string last;
  uint64_t last_ms = 0;
};

std::vector<Watch> g_watches;
std::once_flag g_loaded;

int64_t ParseNumber(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 0); }

void Load() {
  const char* path = std::getenv("SKATE3_WATCH");
  if (!path || !*path) return;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream words(line);
    Watch w;
    std::string start;
    if (!(words >> w.label >> w.type >> start)) continue;
    w.start = static_cast<uint32_t>(ParseNumber(start));
    std::string token;
    while (words >> token) {
      if (token == "@") {
        w.steps.push_back({true, 0});
      } else if (token.rfind("every=", 0) == 0) {
        w.every_ms = static_cast<uint64_t>(ParseNumber(token.substr(6)));
      } else if (token[0] == '+' || token[0] == '-') {
        const int64_t v = ParseNumber(token.substr(1));
        w.steps.push_back({false, token[0] == '-' ? -v : v});
      }
    }
    g_watches.push_back(std::move(w));
  }
  rex::audio_trace::line("WATCHLIST", "%zu watches from %s", g_watches.size(), path);
}

// The watch's final address, or 0 when a pointer step is unreadable.
uint32_t Resolve(uint8_t* base, const Watch& w) {
  uint32_t addr = w.start;
  for (const Step& s : w.steps) {
    if (s.deref) {
      if (!Readable(base, addr, 4)) return 0;
      addr = LoadU32(base, addr);
    } else {
      addr = static_cast<uint32_t>(static_cast<int64_t>(addr) + s.add);
    }
  }
  return addr;
}

std::string Format(uint8_t* base, const Watch& w, uint32_t addr) {
  char out[160];
  const std::string& t = w.type;
  uint32_t size = 4;
  if (t == "u8") size = 1;
  else if (t == "u16") size = 2;
  else if (t == "u64") size = 8;
  else if (t == "vec3") size = 12;
  else if (t.rfind("hex", 0) == 0) size = static_cast<uint32_t>(std::clamp<int64_t>(ParseNumber(t.substr(3)), 1, 64));
  if (!addr || !Readable(base, addr, size)) return "unreadable";
  if (t == "u8") std::snprintf(out, sizeof out, "%u", base[addr]);
  else if (t == "u16") std::snprintf(out, sizeof out, "%u", static_cast<unsigned>((base[addr] << 8) | base[addr + 1]));
  else if (t == "s32") std::snprintf(out, sizeof out, "%d", static_cast<int32_t>(LoadU32(base, addr)));
  else if (t == "u64") std::snprintf(out, sizeof out, "%016llX", static_cast<unsigned long long>(LoadU64(base, addr)));
  else if (t == "f32") std::snprintf(out, sizeof out, "%.6g", LoadF32(base, addr));
  else if (t == "vec3") Vec3Text(base, addr, out, sizeof out);
  else if (t.rfind("hex", 0) == 0) {
    static const char kDigits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < size; ++i) {
      out[2 * i] = kDigits[base[addr + i] >> 4];
      out[2 * i + 1] = kDigits[base[addr + i] & 15];
    }
    out[2 * size] = 0;
  } else std::snprintf(out, sizeof out, "%u", LoadU32(base, addr));
  return out;
}

}  // namespace

void QueueWatchdog(uint8_t* base);  // hooks_queue.cpp (QSTAT every 100 ms, category audio)

extern "C" REX_FUNC(sub_82485190) {
  __imp__sub_82485190(ctx, base);
  if (On("audio")) QueueWatchdog(base);
  if (!On("watch")) return;
  std::call_once(g_loaded, Load);
  if (g_watches.empty()) return;
  const uint64_t now = GetTickCount64();
  for (Watch& w : g_watches) {
    const std::string value = Format(base, w, Resolve(base, w));
    if (value != w.last || (w.every_ms && now - w.last_ms >= w.every_ms)) {
      w.last = value;
      w.last_ms = now;
      rex::audio_trace::line("WATCH", "%s\t%s", w.label.c_str(), value.c_str());
    }
  }
}
