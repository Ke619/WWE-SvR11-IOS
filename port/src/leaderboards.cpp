// WWE SmackDown vs. Raw 2011 - the online LEADERBOARDS (leaderboards.h).

#include "leaderboards.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"
#include "json_lite.h"
#include "online_net.h"
#include "online_overlay.h"

namespace svr2011 {

namespace {

constexpr uint32_t kOk = 0, kInsufficientBuffer = 122, kNoMoreFiles = 18, kIoPending = 997;
constexpr uint32_t kHandleBase = 0xFEED0000;
constexpr uint16_t kRank = 65535, kRating = 65534, kName = 65533;

rex::memory::Memory* g_memory = nullptr;
rex::system::KernelState* g_kernel = nullptr;

uint8_t* Guest(uint32_t a) { return g_memory->TranslateVirtual<uint8_t*>(a); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }
void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
void Put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
void Put64(uint8_t* p, uint64_t v) { Put32(p, uint32_t(v >> 32)), Put32(p + 4, uint32_t(v)); }

bool SignedIn() {
  return rex::cvar::Query<bool>("online_enabled") && !rex::cvar::Query<std::string>("online_token").empty();
}

std::string Hex16(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(v));
  return b;
}

std::optional<Json> Post(const std::string& what, const std::string& body) {
  auto r = net::ServerRequest("POST", "/api/stats/" + what, body, {{"Content-Type", "application/json"}});
  Json j;
  if (!r || r->status != 200 || !JsonReader{r->body}.Value(j)) {
    REXLOG_WARN("leaderboards: {} - {}", what, r ? "the server said " + std::to_string(r->status) : "no answer");
    return std::nullopt;
  }
  return j;
}

// -- writing (XSessionWriteStats) ------------------------------------------------------

// XUSER_PROPERTY (24 bytes): id, XUSER_DATA at +8 (type byte, the value at +16).
std::string PropertyJson(const uint8_t* p) {
  const uint32_t id = Be32(p);
  const uint8_t type = p[8];
  const uint8_t* v = p + 16;
  std::string value;
  switch (type) {
    case 1: value = std::to_string(int32_t(Be32(v))); break;              // LONG
    case 2: value = std::to_string(int64_t(Be64(v))); break;              // LONGLONG
    case 3: {                                                             // double
      const uint64_t bits = Be64(v);
      double d;
      std::memcpy(&d, &bits, 8);
      value = std::to_string(d);
      break;
    }
    case 5: {                                                             // float
      const uint32_t bits = Be32(v);
      float f;
      std::memcpy(&f, &bits, 4);
      value = std::to_string(f);
      break;
    }
    case 7: value = std::to_string(int64_t(Be64(v))); break;              // FILETIME
    default: return {};                                                   // (strings, binary: not kept)
  }
  return "[" + std::to_string(id) + ", " + std::to_string(type) + ", " + value + "]";
}

// buffer: object, (pad), XUID at +8, view count at +16, XSESSION_VIEW_PROPERTIES* at +20.
uint32_t WriteStats(uint32_t buffer, uint32_t length) {
  const uint8_t* b = Guest(buffer);
  const uint64_t xuid = Be64(b + 8);
  const uint32_t count = std::min<uint32_t>(Be32(b + 16), 64), views = Be32(b + 20);
  const uint64_t mine = std::strtoull(rex::cvar::Query<std::string>("online_xuid").c_str(), nullptr, 16);
  if (length < 24 || !views || !count || !SignedIn()) return kOk;
  if (xuid != mine) return kOk;  // (each game writes its own player's: the server takes no one else's)
  std::string json = "{\"xuid\": \"" + Hex16(xuid) + "\", \"views\": [";
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* v = Guest(views + 12 * i);  // {view id, property count, XUSER_PROPERTY*}
    const uint32_t id = Be32(v), n = std::min<uint32_t>(Be32(v + 4), 64), props = Be32(v + 8);
    json += std::string(i ? ", " : "") + "{\"view\": " + std::to_string(id) + ", \"props\": [";
    bool first = true;
    for (uint32_t k = 0; props && k < n; ++k) {
      const std::string p = PropertyJson(Guest(props + 24 * k));
      if (p.empty()) continue;
      json += (first ? "" : ", ") + p;
      first = false;
    }
    json += "]}";
  }
  json += "]}";
  std::thread([json, count] {
    if (Post("write", json)) REXLOG_INFO("leaderboards: {} view(s) written", count);
  }).detach();
  return kOk;
}

// -- reading: XUSER_STATS_READ_RESULTS into the game's buffer ---------------------------

struct Column {
  uint16_t id;
  uint8_t type;
  int64_t i = 0;
  double d = 0;
};
struct Row {
  uint64_t xuid = 0;
  uint32_t rank = 0;
  int64_t rating = 0;
  std::string name;
  std::vector<Column> columns;
};
struct View {
  uint32_t id = 0, total = 0;
  std::vector<Row> rows;
};

// XUSER_STATS_SPEC (136 bytes): view id, column count, WORD column ids[64].
struct Spec {
  uint32_t view;
  std::vector<uint16_t> columns;
};
std::vector<Spec> ReadSpecs(uint32_t specs, uint32_t count) {
  std::vector<Spec> out;
  for (uint32_t i = 0; specs && i < std::min<uint32_t>(count, 64); ++i) {
    const uint8_t* s = Guest(specs + 136 * i);
    Spec sp{Be32(s), {}};
    for (uint32_t k = 0; k < std::min<uint32_t>(Be32(s + 4), 64); ++k) sp.columns.push_back(Be16(s + 8 + 2 * k));
    out.push_back(std::move(sp));
  }
  return out;
}

// A server row, with the columns a spec asks for.
Row ToRow(const Json& j, const Spec& spec) {
  Row r;
  r.xuid = std::strtoull(j.Str("xuid").c_str(), nullptr, 16);
  r.rank = j["rank"] ? uint32_t(j["rank"]->n) : 0;
  r.rating = j["rating"] ? int64_t(j["rating"]->n) : 0;
  r.name = j.Str("name").substr(0, 15);
  const Json* cols = j["columns"];
  for (uint16_t id : spec.columns) {
    Column c{id, 1};
    if (id == kRank) {
      c.i = r.rank;
    } else if (id == kRating) {
      c.type = 2, c.i = r.rating;
    } else if (id == kName) {
      c.type = 4;  // (the name: a string)
    } else if (const Json* v = cols ? (*cols)[std::to_string(id).c_str()] : nullptr; v && v->items.size() == 2) {
      c.type = uint8_t(v->items[0].n);
      c.i = int64_t(v->items[1].n), c.d = v->items[1].n;
    } else {
      c.type = 0;  // (not written yet: none)
    }
    r.columns.push_back(c);
  }
  return r;
}

size_t ResultsSize(const std::vector<View>& views) {
  size_t n = 8 + 16 * views.size();
  for (const auto& v : views)
    for (const auto& r : v.rows) {
      n += 48 + 24 * r.columns.size();
      for (const auto& c : r.columns)
        if (c.type == 4) n += ((r.name.size() + 1) * 2 + 3) & ~size_t(3);
    }
  return n;
}

void WriteResults(uint32_t at, const std::vector<View>& views) {
  uint8_t* out = Guest(at);
  std::memset(out, 0, ResultsSize(views));
  Put32(out, uint32_t(views.size()));
  Put32(out + 4, at + 8);
  uint32_t next = at + 8 + 16 * uint32_t(views.size());
  for (size_t i = 0; i < views.size(); ++i) {
    uint8_t* v = out + 8 + 16 * i;
    Put32(v, views[i].id), Put32(v + 4, views[i].total), Put32(v + 8, uint32_t(views[i].rows.size()));
    Put32(v + 12, views[i].rows.empty() ? 0 : next);
    const uint32_t rows = next;
    next += 48 * uint32_t(views[i].rows.size());
    for (size_t k = 0; k < views[i].rows.size(); ++k) {
      const Row& r = views[i].rows[k];
      uint8_t* p = Guest(rows + 48 * uint32_t(k));
      Put64(p, r.xuid), Put32(p + 8, r.rank), Put64(p + 16, uint64_t(r.rating));
      std::memcpy(p + 24, r.name.data(), std::min<size_t>(r.name.size(), 15));
      Put32(p + 40, uint32_t(r.columns.size()));
      Put32(p + 44, r.columns.empty() ? 0 : next);
      const uint32_t cols = next;
      next += 24 * uint32_t(r.columns.size());
      for (size_t c = 0; c < r.columns.size(); ++c) {
        const Column& col = r.columns[c];
        uint8_t* q = Guest(cols + 24 * uint32_t(c));
        Put16(q, col.id);
        q[8] = col.type;
        if (col.type == 1) {
          Put32(q + 16, uint32_t(int32_t(col.i)));
        } else if (col.type == 2 || col.type == 7) {
          Put64(q + 16, uint64_t(col.i));
        } else if (col.type == 3) {
          uint64_t bits;
          std::memcpy(&bits, &col.d, 8);
          Put64(q + 16, bits);
        } else if (col.type == 5) {
          const float f = float(col.d);
          uint32_t bits;
          std::memcpy(&bits, &f, 4);
          Put32(q + 16, bits);
        } else if (col.type == 4) {  // UTF-16BE, in the buffer
          const uint32_t bytes = uint32_t(r.name.size() + 1) * 2;
          Put32(q + 16, bytes), Put32(q + 20, next);
          uint8_t* s = Guest(next);
          for (size_t ch = 0; ch < r.name.size(); ++ch) s[2 * ch] = 0, s[2 * ch + 1] = uint8_t(r.name[ch]);
          next += (bytes + 3) & ~3u;
        }
      }
    }
  }
}

// buffer: title, xuid count, XUID*, spec count, XUSER_STATS_SPEC*, the results' size, the results.
uint32_t ReadStats(uint32_t buffer) {
  const uint8_t* b = Guest(buffer);
  const uint32_t nx = std::min<uint32_t>(Be32(b + 4), 100), xuids = Be32(b + 8);
  const auto specs = ReadSpecs(Be32(b + 16), Be32(b + 12));
  const uint32_t have = Be32(b + 20), results = Be32(b + 24);  // (the buffer's size, and the buffer)
  std::vector<uint64_t> who;
  for (uint32_t i = 0; xuids && i < nx; ++i) who.push_back(Be64(Guest(xuids + 8 * i)));
  std::vector<View> views;
  std::optional<Json> j;
  if (SignedIn() && !specs.empty()) {
    std::string body = "{\"xuids\": [";
    for (size_t i = 0; i < who.size(); ++i) body += std::string(i ? ", " : "") + "\"" + Hex16(who[i]) + "\"";
    body += "], \"views\": [";
    for (size_t i = 0; i < specs.size(); ++i) body += std::string(i ? ", " : "") + std::to_string(specs[i].view);
    body += "]}";
    j = Post("read", body);
  }
  for (size_t i = 0; i < specs.size(); ++i) {
    View v{specs[i].view, 0, {}};
    if (j && (*j)["views"] && i < (*j)["views"]->items.size()) {
      const Json& jv = (*j)["views"]->items[i];
      v.total = jv["total"] ? uint32_t(jv["total"]->n) : 0;
      if (const Json* rows = jv["rows"])
        for (const Json& r : rows->items) v.rows.push_back(ToRow(r, specs[i]));
    }
    views.push_back(std::move(v));
  }
  {
    std::string what;
    for (const auto& v : views) what += " view " + std::to_string(v.id) + ": " + std::to_string(v.rows.size()) + " row(s)";
    for (const auto& sp : specs) what += " [" + std::to_string(sp.columns.size()) + " col]";
    REXLOG_INFO("leaderboards: read{} - {} of {} bytes", what, ResultsSize(views), have);
  }
  // (what doesn't fit is left out: rows, then the views')
  while (ResultsSize(views) > have && !views.empty()) {
    auto it = std::find_if(views.rbegin(), views.rend(), [](const View& v) { return !v.rows.empty(); });
    if (it == views.rend()) break;
    it->rows.pop_back();
  }
  if (!results || ResultsSize(views) > have) return kInsufficientBuffer;
  WriteResults(results, views);
  REXLOG_INFO("leaderboards: read {} view(s) for {} player(s)", views.size(), who.size());
  return kOk;
}

// -- the enumerators ---------------------------------------------------------------------

struct Enumerator {
  int mode = 0;  // 0: around a player (xuid), 1: by rank
  uint64_t pivot = 0;
  uint32_t rows = 10;
  std::vector<Spec> specs;
  bool done = false;
};
std::mutex g_mutex;
std::map<uint32_t, Enumerator> g_enums;
uint32_t g_next = 1;

size_t PageSize(const Enumerator& e) {
  std::vector<View> views;
  for (const auto& s : e.specs) {
    View v{s.view, 0, {}};
    Row r;
    r.name = std::string(15, 'x');
    for (uint16_t id : s.columns) r.columns.push_back(Column{id, uint8_t(id == kName ? 4 : 1)});  // (only the name is text)
    v.rows.assign(e.rows, r);
    views.push_back(std::move(v));
  }
  return ResultsSize(views);
}

std::vector<View> FetchPage(const Enumerator& e) {
  std::vector<View> views;
  for (const auto& s : e.specs) {
    View v{s.view, 0, {}};
    std::optional<Json> j;
    NoteOnlineActivity(OnlineActivity::kLeaderboards);  // (the presence poll's place)
    if (SignedIn()) {
      const std::string pivot = e.mode == 0 ? "\"" + Hex16(e.pivot) + "\"" : std::to_string(e.pivot ? e.pivot : 1);
      j = Post("page", "{\"view\": " + std::to_string(s.view) + ", \"mode\": \"" + (e.mode == 0 ? "xuid" : "rank") +
                           "\", \"pivot\": " + pivot + ", \"count\": " + std::to_string(e.rows) + "}");
    }
    if (j) {
      v.total = (*j)["total"] ? uint32_t((*j)["total"]->n) : 0;
      if (const Json* rows = (*j)["rows"])
        for (const Json& r : rows->items) v.rows.push_back(ToRow(r, s));
    }
    views.push_back(std::move(v));
  }
  return views;
}

}  // namespace

void InstallLeaderboards(rex::memory::Memory* memory, rex::system::KernelState* kernel) {
  g_memory = memory;
  g_kernel = kernel;
}

std::optional<uint32_t> LeaderboardsXgi(uint32_t message, uint32_t buffer, uint32_t length) {
  if (!g_memory || !buffer) return std::nullopt;
  switch (message) {
    case 0xB0025: return WriteStats(buffer, length);  // XSessionWriteStats
    case 0xB0021: return ReadStats(buffer);           // XUserReadStats
    default: return std::nullopt;
  }
}

}  // namespace svr2011

namespace {

// XamUserCreateStatsEnumerator(title, kind, pivot, rows, spec count, specs, DWORD* size, HANDLE*),
// from the game's wrappers: (title, pivot, rows, spec count, specs, size*, handle*).
void CreateEnumerator(PPCContext& ctx, uint8_t* base, int mode) {
  using namespace svr2011;
  Enumerator e;
  e.mode = mode;
  e.pivot = mode == 0 ? ctx.r4.u64 : ctx.r4.u32;
  e.rows = std::clamp<uint32_t>(ctx.r5.u32, 1, 100);
  e.specs = ReadSpecs(ctx.r7.u32, ctx.r6.u32);
  const uint32_t size_ptr = ctx.r8.u32, handle_ptr = ctx.r9.u32;
  uint32_t handle;
  {
    std::lock_guard lock(g_mutex);
    handle = kHandleBase | (g_next++ & 0xFFFF);
    g_enums[handle] = e;
  }
  if (size_ptr) Put32(base + size_ptr, uint32_t(PageSize(e)));
  if (handle_ptr) Put32(base + handle_ptr, handle);
  REXLOG_INFO("leaderboards: list {} ({} rows {} {:X}, {} view(s))", handle, e.rows, mode == 0 ? "around" : "from rank",
              e.pivot, e.specs.size());
  ctx.r3.u64 = 0;
}

}  // namespace

// XUserCreateStatsEnumeratorByRank (the game's wrapper: kind 1).
REX_HOOK_RAW(sub_82904C20) { CreateEnumerator(ctx, base, 1); }
// XUserCreateStatsEnumeratorByXuid (kind 0).
REX_HOOK_RAW(sub_82904C80) { CreateEnumerator(ctx, base, 0); }

// XEnumerate(handle, buffer, size, DWORD* items, XOVERLAPPED*) (the game's, on XamEnumerate): a page of
// one of the port's stats lists; others go on to the SDK.
REX_HOOK_RAW(sub_829048B0) {
  using namespace svr2011;
  const uint32_t handle = ctx.r3.u32;
  if ((handle & 0xFFFF0000) != kHandleBase) {
    __imp__sub_829048B0(ctx, base);
    return;
  }
  Enumerator e;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_enums.find(handle);
    if (it == g_enums.end()) {
      ctx.r3.u64 = 6;  // ERROR_INVALID_HANDLE
      return;
    }
    e = it->second;
    it->second.done = true;
  }
  const uint32_t buffer = ctx.r4.u32, size = ctx.r5.u32, items_ptr = ctx.r6.u32, overlapped = ctx.r7.u32;
  uint32_t result = kNoMoreFiles, items = 0;
  if (!e.done) {
    const auto views = FetchPage(e);
    if (ResultsSize(views) <= size && buffer) {
      WriteResults(buffer, views);
      items = 1, result = kOk;
      REXLOG_INFO("leaderboards: list {}: {} row(s) of {}", handle, views.empty() ? 0 : views[0].rows.size(),
                  views.empty() ? 0 : views[0].total);
    } else {
      result = kInsufficientBuffer;
    }
  }
  if (items_ptr) Put32(base + items_ptr, items);
  if (overlapped && g_kernel) {
    g_kernel->CompleteOverlappedImmediateEx(overlapped, result, result, items);
    ctx.r3.u64 = kIoPending;
  } else {
    ctx.r3.u64 = result;
  }
}
