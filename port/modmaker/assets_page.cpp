// Game assets page: everything in the game's pac files, read-only. A tree
// of the pac folder (EPAC / EPK8 -> groups -> entries -> PACH children ->
// texture bundles / textures / models / motion banks / tables / text), read
// lazily (one entry at a time, never a whole 300 MB file), with a viewer for
// each kind and Export for any node. Formats: docs/ARENA_MOD_MAKER_PLAN.md,
// docs/MOVE_PACKS.md, docs/SUPERSTAR_MODS.md, docs/SVRMOD_FORMAT.md.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

#include "app.h"
#include "svrfmt/disc.h"
#include "svrfmt/jboy.h"
#include "svrfmt/pac.h"
#include "svrfmt/png.h"
#include "svrfmt/wwe13.h"

namespace mm {
namespace assets_page {

using namespace svrfmt;

namespace {

enum class Kind { kFolder, kFile, kGroup, kEntry, kPach, kBundle, kTexture, kModel, kBank, kStrings, kRoster, kText, kData };

struct Node {
  Kind kind = Kind::kData;
  std::string label;    // what the tree shows
  std::string detail;   // the viewer's header line
  fs::path file;        // the pac file (kFile and below) in a folder ...
  std::shared_ptr<Disc> disc;          // ... or on a disc image: the disc and its entry (kFolder / kFile)
  int di = -1;
  std::shared_ptr<const Bytes> whole;  // kFile and below: the file in memory (an xcompress pac, unwrapped)
  PacEntryInfo entry;   // kEntry: where in the file
  uint32_t id = 0;      // kPach children: the PACH id
  Bytes data;           // the node's bytes, unpacked (kEntry and below; empty until opened)
  size_t stored = 0;    // as stored (packed) size
  bool opened = false;  // children listed
  bool loading = false;
  std::vector<std::unique_ptr<Node>> children;
  Node* parent = nullptr;
  // viewers
  Picture picture;             // kTexture
  std::vector<Picture> thumbs; // kBundle
  std::string error;
};

std::unique_ptr<Node> g_root;
Node* g_sel = nullptr;
int g_tree_gen = 0;  // (a new tree: background results for the old one are dropped)
std::wstring g_root_for;
// where the tree comes from: the game folder, a disc image or any folder
std::wstring g_source;  // "" = the game's pac folder
std::string g_source_name = "This game (pac folder)";
std::string g_test_path;  // --assets <label/label/...>: opened and selected once the tree exists
char g_find[64] = "";
float g_zoom = 1;
int g_hex_page = 0;

// ---- classifying bytes

bool Printable(const Bytes& b) {
  if (b.empty()) return false;
  // printable up to the first NUL; a NUL ends text only after a real run of it
  // (a short tag like "DPACX" then zeros is a binary header, not text)
  size_t n = 0;
  for (size_t i = 0; i < b.size() && i < 2048; ++i) {
    const uint8_t c = b[i];
    if (c == 0) return n >= 16 || i + 1 == b.size();
    if (c >= 32 && c < 127 || c == '\n' || c == '\r' || c == '\t') ++n;
    else return false;
  }
  return n > 0;
}

bool IsBank(const Bytes& b) { return b.size() >= 0x114 && (!std::memcmp(b.data(), "YMBs", 4) || !std::memcmp(b.data(), "YMKs", 4)); }
bool IsDds(const Bytes& b) { return b.size() >= 128 && !std::memcmp(b.data(), "DDS ", 4); }
// a string table: {u32 0, u32 count} + 16-byte records sorted by id, then UTF-8
bool IsStrings(const Bytes& b) {
  if (b.size() < 8 + 16 || Le32(&b[0]) != 0) return false;
  const uint32_t n = Le32(&b[4]);
  if (!n || 8 + 16 * size_t(n) > b.size()) return false;
  const uint32_t off = Le32(&b[8]), len = Le32(&b[12]);
  return off == 8 + 16 * n && len > 0 && off + len <= b.size();
}
// CHAR/DAT: 4-byte header + 260-byte records (the Mod Maker reads names at +34)
bool IsRoster(const Node& n) { return n.parent && n.parent->label.rfind("DAT", 0) == 0 && n.data.size() == 4 + 260; }

Kind Classify(const Node& n, const Bytes& b) {
  if (IsPach(b)) return Kind::kPach;
  if (IsTextureBundle(b)) return Kind::kBundle;
  if (IsDds(b)) return Kind::kTexture;
  if (IsJboy(b)) return Kind::kModel;
  if (IsBank(b)) return Kind::kBank;
  if (IsStrings(b)) return Kind::kStrings;
  if (IsRoster(n)) return Kind::kRoster;
  if (Printable(b)) return Kind::kText;
  return Kind::kData;
}

const char* KindName(Kind k) {
  switch (k) {
    case Kind::kFolder: return "folder";
    case Kind::kFile: return "pac file";
    case Kind::kGroup: return "group";
    case Kind::kEntry: return "entry";
    case Kind::kPach: return "PACH (a list of children by id)";
    case Kind::kBundle: return "texture bundle";
    case Kind::kTexture: return "texture (DDS)";
    case Kind::kModel: return "model (JBOY)";
    case Kind::kBank: return "motion bank";
    case Kind::kStrings: return "string table";
    case Kind::kRoster: return "roster record (CHAR/DAT)";
    case Kind::kText: return "text";
    default: return "data";
  }
}

// What the tree shows after the label.
std::string Tag(const Node& n) {
  switch (n.kind) {
    case Kind::kPach: return "  [" + std::to_string(n.children.size()) + "]";
    case Kind::kBundle: return "  tex";
    case Kind::kTexture: return "  dds";
    case Kind::kModel: return "  3d";
    case Kind::kBank: return n.data.size() >= 4 && n.data[2] == 'B' ? "  YMBs" : "  YMKs";
    case Kind::kStrings: return "  str";
    case Kind::kText: return "  txt";
    default: return "";
  }
}

// ---- building the tree

// The node's file (or a part of it): from memory, the disc image or the folder.
bool ReadRange(const Node& n, uint64_t offset, uint64_t length, Bytes& out) {
  if (n.whole) {
    const uint64_t at = std::min<uint64_t>(offset, n.whole->size()), len = std::min<uint64_t>(length, n.whole->size() - at);
    out.assign(n.whole->begin() + ptrdiff_t(at), n.whole->begin() + ptrdiff_t(at + len));
    return true;
  }
  if (n.disc) return n.disc->Read(n.di, offset, length, out);
  FILE* f = _wfopen(n.file.c_str(), L"rb");
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  const uint64_t size = uint64_t(_ftelli64(f));
  offset = std::min(offset, size);
  length = std::min(length, size - offset);
  out.resize(size_t(length));
  const bool ok = _fseeki64(f, int64_t(offset), SEEK_SET) == 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

uint64_t FileSize(const Node& n) {
  if (n.whole) return n.whole->size();
  if (n.disc) return n.disc->At(n.di).size;
  std::error_code ec;
  return uint64_t(fs::file_size(n.file, ec));
}

void Inherit(Node& c, const Node& n) {
  c.file = n.file;
  c.disc = n.disc;
  c.di = n.di;
  c.whole = n.whole;
}

void SetData(Node& n, Bytes raw) {
  n.stored = raw.size();
  n.data = Unpack(raw);
  n.kind = Classify(n, n.data);
  if (n.kind == Kind::kPach) {
    std::vector<PachEntry> kids;
    if (PachRead(n.data, kids))
      for (auto& k : kids) {
        auto c = std::make_unique<Node>();
        c->parent = &n;
        c->id = k.id;
        char b[32];
        std::snprintf(b, sizeof b, "%u (0x%X)", k.id, k.id);
        c->label = b;
        Inherit(*c, n);
        c->stored = k.data.size();
        // small children are classified now (the tree shows what they are); big ones when opened
        if (k.data.size() <= 256 * 1024) SetData(*c, std::move(k.data));
        else c->data = std::move(k.data), c->kind = Kind::kEntry;  // (still packed: opened on demand)
        n.children.push_back(std::move(c));
      }
  } else if (n.kind == Kind::kBundle) {
    std::vector<BundleTexture> texs;
    if (BundleRead(n.data, texs))
      for (auto& t : texs) {
        auto c = std::make_unique<Node>();
        c->parent = &n;
        c->label = t.name;
        Inherit(*c, n);
        c->stored = t.data.size();
        c->data = std::move(t.data);
        c->kind = Kind::kTexture;
        n.children.push_back(std::move(c));
      }
  }
  n.opened = true;
}

// A node still packed (a big PACH child): unpacked and classified now.
void Open(Node& n) {
  if (n.opened) return;
  if (n.kind == Kind::kEntry && !n.data.empty()) {
    Bytes raw = std::move(n.data);
    n.data.clear();
    SetData(n, std::move(raw));
    return;
  }
  if (n.kind == Kind::kEntry) {  // a pac entry: read from the file
    Bytes raw;
    if (!ReadRange(n, n.entry.offset, n.entry.size, raw)) {
      n.error = "could not read the entry";
      n.opened = true;
      return;
    }
    SetData(n, std::move(raw));
    return;
  }
  if (n.kind == Kind::kFile) {
    if (n.loading) return;
    Bytes head;
    if (!ReadRange(n, 0, 0x4000, head)) {
      n.error = "could not read the file";
      n.opened = true;
      return;
    }
    std::string how;
    if (IsXcompress(head)) {  // WWE '13 and later: the pac inside an xcompress (LZX) wrapper, unpacked on a thread
      n.loading = true;
      n.detail = "unpacking (xcompress) ...";
      Node copy;  // (what the thread needs to read the file)
      Inherit(copy, n);
      const uint64_t size = FileSize(n);
      std::thread([gen = g_tree_gen, node = &n, file = copy.file, disc = copy.disc, di = copy.di, size] {
        Node src;
        src.file = file, src.disc = disc, src.di = di;
        Bytes all, plain;
        std::string err;
        const bool ok = ReadRange(src, 0, size, all) && XcompressDecode(all, plain, &err);
        auto result = std::make_shared<Bytes>(std::move(plain));
        OnUiThread([gen, node, ok, err, result] {
          if (gen != g_tree_gen) return;  // (another tree by now)
          node->loading = false;
          if (!ok) {
            node->error = "the xcompress file did not unpack" + (err.empty() ? std::string() : ": " + err);
            node->opened = true;
            return;
          }
          node->whole = std::make_shared<const Bytes>(std::move(*result));
          node->detail.clear();
          Open(*node);
          if (node->detail.rfind("xcompress", 0) != 0) node->detail = "xcompress, " + node->detail;
        });
      }).detach();
      return;
    }
    PacIndex idx;
    if (!ParsePacIndex(head, idx)) {  // not a pac: the file itself (PACH, a texture, text, ...)
      const uint64_t size = FileSize(n);
      if (size > (96ull << 20)) {
        n.error = "not an EPAC / EPK8 pac (" + std::to_string(size >> 20) + " MB: too big to show)";
        n.opened = true;
        return;
      }
      Bytes all;
      if (!ReadRange(n, 0, size, all)) {
        n.error = "could not read the file";
        n.opened = true;
        return;
      }
      SetData(n, std::move(all));
      n.detail = how + KindName(n.kind);
      return;
    }
    n.detail = how + (idx.epk8 ? "EPK8" : "EPAC") + ", " + std::to_string(idx.entries.size()) + " entries";
    Node* group = nullptr;
    for (const auto& e : idx.entries) {
      if (!group || group->label != e.group) {
        auto g = std::make_unique<Node>();
        g->kind = Kind::kGroup;
        g->label = e.group;
        Inherit(*g, n);
        g->parent = &n;
        g->opened = true;
        n.children.push_back(std::move(g));
        group = n.children.back().get();
      }
      auto c = std::make_unique<Node>();
      c->kind = Kind::kEntry;
      c->label = e.name;
      Inherit(*c, n);
      c->entry = e;
      c->stored = e.size;
      c->parent = group;
      group->children.push_back(std::move(c));
    }
    n.opened = true;
    return;
  }
  if (n.kind == Kind::kFolder && n.disc) {  // a folder on a disc image: every file (non-pacs open as data)
    std::vector<int> kids = n.disc->At(n.di).children;
    std::sort(kids.begin(), kids.end(), [&](int a, int b) {
      const auto &ea = n.disc->At(a), &eb = n.disc->At(b);
      return ea.dir != eb.dir ? ea.dir : Lower(ea.name) < Lower(eb.name);
    });
    for (int k : kids) {
      const DiscEntry& e = n.disc->At(k);
      auto c = std::make_unique<Node>();
      c->kind = e.dir ? Kind::kFolder : Kind::kFile;
      c->label = e.name;
      c->disc = n.disc;
      c->di = k;
      c->parent = &n;
      c->stored = size_t(e.size);
      n.children.push_back(std::move(c));
    }
    n.opened = true;
    return;
  }
  if (n.kind == Kind::kFolder) {
    std::error_code ec;
    std::vector<fs::directory_entry> items;
    for (const auto& e : fs::directory_iterator(n.file, ec)) items.push_back(e);
    std::sort(items.begin(), items.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
      const bool da = a.is_directory(), db = b.is_directory();
      return da != db ? da : a.path().filename() < b.path().filename();
    });
    for (const auto& e : items) {
      const bool dir = e.is_directory(ec);
      if (!dir && !g_source.empty() && Lower(e.path().extension().string()) != ".pac" && e.file_size(ec) > (96ull << 20)) continue;
      if (!dir && g_source.empty() && Lower(e.path().extension().string()) != ".pac") continue;
      auto c = std::make_unique<Node>();
      c->kind = dir ? Kind::kFolder : Kind::kFile;
      c->label = PathStr(e.path().filename());
      c->file = e.path();
      c->parent = &n;
      if (!dir) c->stored = size_t(e.file_size(ec));
      n.children.push_back(std::move(c));
    }
    n.opened = true;
  }
}

void Select(Node* n) {
  g_sel = n;
  g_hex_page = 0;
  if (!n) return;
  Open(*n);
  if (n->kind == Kind::kTexture && n->picture.Empty()) {
    Image img;
    if (DdsDecode(n->data, img)) n->picture.Set(std::move(img));
    else n->error = "the texture did not decode";
  }
  if (n->kind == Kind::kBundle && n->thumbs.empty())
    for (auto& c : n->children) {
      Image img;
      Picture p;
      if (DdsDecode(c->data, img)) p.Set(Resize(img, std::max(1, std::min(img.w, 128)), std::max(1, std::min(img.h, 128))));
      n->thumbs.push_back(std::move(p));
    }
}

std::string Path(const Node* n) {
  std::string s;
  for (; n; n = n->parent) s = n->label + (s.empty() ? "" : "/" + s);
  return s;
}

void Tree(Node& n, const std::string& find) {
  for (auto& c : n.children) {
    Node& k = *c;
    const bool leaf = k.kind == Kind::kTexture || k.kind == Kind::kModel || k.kind == Kind::kBank || k.kind == Kind::kStrings ||
                      k.kind == Kind::kRoster || k.kind == Kind::kText || k.kind == Kind::kData;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (g_sel == &k) flags |= ImGuiTreeNodeFlags_Selected;
    if (!find.empty() && Lower(k.label).find(find) == std::string::npos && !(k.kind == Kind::kFolder || k.kind == Kind::kFile || k.kind == Kind::kGroup)) continue;
    const std::string label = k.label + Tag(k) + "##" + std::to_string(reinterpret_cast<uintptr_t>(&k));
    for (const Node* a = g_sel ? g_sel->parent : nullptr; a; a = a->parent)
      if (a == &k) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) Select(&k);
    if (ImGui::IsItemHovered() && k.stored) ImGui::SetTooltip("%s", Human(k.stored).c_str());
    if (open && !leaf) {
      Open(k);
      Tree(k, find);
      ImGui::TreePop();
    }
  }
}

// ---- viewers

void Export(const Node& n, bool png) {
  const std::string stem = IdFrom(n.label, "entry");
  if (png && !n.picture.Empty()) {
    const COMDLG_FILTERSPEC spec[] = {{L"PNG picture (*.png)", L"*.png"}};
    const std::wstring f = PickFile(true, L"Export the picture", spec, 1, L"png", (Wide(stem) + L".png").c_str());
    if (!f.empty()) Status(SavePng(Utf8(f), n.picture.image) ? "Exported " + Utf8(f) : "Could not write " + Utf8(f));
    return;
  }
  const wchar_t* ext = n.kind == Kind::kTexture ? L"dds" : n.kind == Kind::kText ? L"txt" : L"bin";
  const COMDLG_FILTERSPEC spec[] = {{L"File", L"*.*"}};
  const std::wstring f = PickFile(true, L"Export the data (unpacked)", spec, 1, ext, (Wide(stem) + L"." + ext).c_str());
  if (!f.empty()) Status(WriteFile(Utf8(f), n.data) ? "Exported " + Utf8(f) : "Could not write " + Utf8(f));
}

void HexView(const Bytes& b) {
  const size_t per_page = 4096, pages = (b.size() + per_page - 1) / per_page;
  if (pages > 1) {
    ImGui::SetNextItemWidth(200 * g_scale);
    ImGui::SliderInt("page", &g_hex_page, 0, int(pages) - 1);
  }
  g_hex_page = std::clamp(g_hex_page, 0, int(std::max<size_t>(1, pages)) - 1);
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.85f);
  ImGui::BeginChild("hex", ImVec2(0, 0), true);
  const size_t start = size_t(g_hex_page) * per_page, end = std::min(b.size(), start + per_page);
  for (size_t off = start; off < end; off += 16) {
    char line[128];
    int p = std::snprintf(line, sizeof line, "%08zX  ", off);
    for (size_t i = 0; i < 16; ++i)
      p += off + i < end ? std::snprintf(line + p, sizeof line - p, "%02X ", b[off + i]) : std::snprintf(line + p, sizeof line - p, "   ");
    p += std::snprintf(line + p, sizeof line - p, " ");
    for (size_t i = 0; i < 16 && off + i < end; ++i) {
      const uint8_t c = b[off + i];
      p += std::snprintf(line + p, sizeof line - p, "%c", c >= 32 && c < 127 ? c : '.');
    }
    ImGui::TextUnformatted(line);
  }
  ImGui::EndChild();
  ImGui::PopFont();
}

void BankView(const Bytes& b) {
  const bool ymbs = b[2] == 'B';
  const uint32_t n = Le32(&b[0x110]);
  ImGui::Text("%s bank: %u entries. %s", ymbs ? "YMBs" : "YMKs", n,
              ymbs ? "Menu motions (ADPCM): the Animations page plays them." : "Match motions: only the game plays them (Animations > In the game).");
  if (ImGui::BeginTable("bank", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Move", ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("x", ImGuiTableColumnFlags_WidthFixed, 40 * g_scale);
    ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthFixed, 60 * g_scale);
    ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin(int(n));
    while (clip.Step())
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const uint8_t* e = &b[0x114 + 16 * size_t(i)];
        if (0x114 + 16 * size_t(i + 1) > b.size()) break;
        const int id = Le16(e + 2);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", id);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(MoveName(id).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%d", e[1]);
        ImGui::TableNextColumn();
        ImGui::Text("%s", e[0] == 0 ? "attacker" : e[0] == 1 ? "victim" : std::to_string(e[0]).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%u", Le32(e + 8));
      }
    ImGui::EndTable();
  }
}

void StringsView(const Bytes& b) {
  const uint32_t n = Le32(&b[4]);
  ImGui::Text("%u strings (id -> text, UTF-8). The game's menus look these up by id.", n);
  static char filter[64] = "";
  ImGui::SetNextItemWidth(300 * g_scale);
  ImGui::InputTextWithHint("##sf", "find text or id (hex)", filter, sizeof filter);
  const std::string f = Lower(filter);
  if (ImGui::BeginTable("strings", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
    ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* r = &b[8 + 16 * size_t(i)];
      const uint32_t off = Le32(r), len = Le32(r + 4), id = Le32(r + 8);
      if (off >= b.size()) continue;
      const char* s = reinterpret_cast<const char*>(&b[off]);
      const std::string text(s, strnlen(s, std::min<size_t>(len ? len : 1, b.size() - off)));
      char hex[16];
      std::snprintf(hex, sizeof hex, "%04X", id);
      if (!f.empty() && Lower(text).find(f) == std::string::npos && Lower(hex).find(f) == std::string::npos) continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(hex);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(text.c_str());
    }
    ImGui::EndTable();
  }
}

void RosterView(const Bytes& b) {
  const uint8_t* r = &b[4];
  auto str = [&](size_t at, size_t n) { return std::string(reinterpret_cast<const char*>(r + at), strnlen(reinterpret_cast<const char*>(r + at), n)); };
  ImGui::Text("Name: %s   Second: %s   Short: %s", str(34, 32).c_str(), str(102, 32).c_str(), str(170, 32).c_str());
  const char* attrs[7] = {"Grapple", "Submission", "Speed", "Strikes", "Hardcore", "Charisma", "Durability"};
  std::string a;
  for (int k = 0; k < 7; ++k) a += std::string(attrs[k]) + " " + std::to_string(r[k]) + "  ";
  ImGui::TextUnformatted(a.c_str());
  ImGui::Text("Id %u (+32), same person %u (+228), selectable %u (+221), select tile %u (+226), DLC %u (+257)", Le16(r + 32),
              Le16(r + 228), r[221], Le16(r + 226), r[257]);
  std::string ab = "Abilities (+230): ";
  for (int k = 0; k < 8 && r[230 + k]; ++k) ab += std::to_string(r[230 + k]) + " ";
  ImGui::TextUnformatted(ab.c_str());
  std::string signs = "Crowd signs (+210): ";
  for (int k = 0; k < 4; ++k) signs += std::to_string(Le16(r + 210 + 2 * k)) + " ";
  ImGui::TextUnformatted(signs.c_str());
  ImGui::Separator();
  HexView(b);
}

void ModelView(const Bytes& b) {
  static const Bytes* last = nullptr;
  static Model model;
  static bool ok = false;
  if (last != &b) last = &b, ok = JboyRead(b, model);
  if (!ok) {
    ImGui::TextColored(kBad, "The model did not read.");
    HexView(b);
    return;
  }
  size_t verts = 0, tris = 0;
  for (const auto& m : model.meshes) {
    verts += m.verts.size();
    for (const auto& s : m.strips) tris += s.indices.size() > 2 ? s.indices.size() - 2 : 0;
  }
  ImGui::Text("%s: %zu meshes, %zu vertices, about %zu triangles, %zu nodes, %zu textures.", model.name.c_str(),
              model.meshes.size(), verts, tris, model.nodes.size(), model.textures.size());
  ImGui::TextDisabled("Arena models open in the 3D editor (Arena page); character models on the Animations page.");
  if (ImGui::BeginTable("meshes", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 200 * g_scale))) {
    ImGui::TableSetupColumn("Mesh");
    ImGui::TableSetupColumn("Vertices");
    ImGui::TableSetupColumn("Bones");
    ImGui::TableSetupColumn("Shader");
    ImGui::TableSetupColumn("Texture");
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < model.meshes.size(); ++i) {
      const Mesh& m = model.meshes[i];
      int slot = -1;
      for (const auto& p : m.params)
        if (p.type == 0x0f && p.name == "texDiffuse" && p.value.size() >= 4) slot = int(Be32(p.value.data()));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%zu", i);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", m.verts.size());
      ImGui::TableNextColumn();
      ImGui::Text("%zu", m.palette.size());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(m.shader.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(slot >= 0 && slot < int(model.textures.size()) ? model.textures[size_t(slot)].c_str() : "-");
    }
    ImGui::EndTable();
  }
  ImGui::TextUnformatted("Nodes");
  ImGui::BeginChild("nodes", ImVec2(0, 0), true);
  for (size_t i = 0; i < model.nodes.size(); ++i) {
    const auto& nd = model.nodes[i];
    ImGui::Text("%zu  %s  parent %d  t %.1f %.1f %.1f", i, nd.name.c_str(), nd.parent, nd.t[0], nd.t[1], nd.t[2]);
  }
  ImGui::EndChild();
}

void Viewer() {
  Node* n = g_sel;
  if (!n) {
    ImGui::TextDisabled("Pick something in the tree. Folders and pac files open as you expand them; nothing is read "
                        "until you look at it.");
    return;
  }
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.15f);
  ImGui::TextUnformatted(Path(n).c_str());
  ImGui::PopFont();
  std::string info = KindName(n->kind);
  if (n->stored) info += ", " + Human(n->stored) + " stored";
  if (!n->data.empty() && n->data.size() != n->stored) info += ", " + Human(n->data.size()) + " unpacked (BPE)";
  if (!n->detail.empty()) info += ", " + n->detail;
  ImGui::TextDisabled("%s", info.c_str());
  if (!n->error.empty()) ImGui::TextColored(kBad, "%s", n->error.c_str());
  const bool list_file = !n->data.empty() && (IsPach(n->data) || (n->data.size() >= 4 && !std::memcmp(n->data.data(), "HSPC", 4)));
  if (n->kind == Kind::kFile && n->opened && n->error.empty() && (!n->children.empty() || list_file)) {
    PushAccent();
    const bool edit = ImGui::Button("Edit a copy of this pac...");
    PopAccent();
    ImGui::SameLine();
    Hint("Opens a copy in the pac editor: every level (EPAC / EPK8, WWE 2K17 HSPC, PACH and SHDC lists inside "
         "entries); preview, export, replace, rename, remove or add entries, then save the copy somewhere else in "
         "the pac's own layout. The original - the game's own pac folder, a disc - is never written.");
    if (edit) {
      Bytes all;
      Node raw;  // (the file as it is: an xcompress one too, so it is saved xcompressed again)
      Inherit(raw, *n);
      raw.whole.reset();
      if (!ReadRange(raw, 0, FileSize(raw), all)) all.clear();
      if (all.empty()) Status("Could not read the pac.");
      else pac_editor::Open(std::move(all), g_source_name + ": " + Path(n), n->disc ? Wide(n->label) : n->file.wstring(), "");
    }
  }
  if (!n->data.empty()) {
    if (ImGui::Button("Export...")) Export(*n, false);
    if (n->kind == Kind::kTexture && !n->picture.Empty()) {
      ImGui::SameLine();
      if (ImGui::Button("Export as PNG...")) Export(*n, true);
    }
  }
  ImGui::Separator();
  switch (n->kind) {
    case Kind::kTexture: {
      DdsInfo di;
      if (DdsInfoOf(n->data, di))
        ImGui::Text("%d x %d, %s, %d mip level%s", di.w, di.h,
                    di.format == DxtFormat::kDxt1 ? "DXT1" : di.format == DxtFormat::kDxt3 ? "DXT3" : di.format == DxtFormat::kDxt5 ? "DXT5" : di.format == DxtFormat::kArgb ? "ARGB" : "?",
                    di.mips, di.mips == 1 ? "" : "s");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120 * g_scale);
      ImGui::SliderFloat("zoom", &g_zoom, 0.25f, 8, "x%.2f");
      if (!n->picture.Empty()) {
        ImGui::BeginChild("pic", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = n->picture.image.w * g_zoom, h = n->picture.image.h * g_zoom;
        // a checkerboard behind: transparency shows
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (float y = 0; y < h; y += 16)
          for (float x = 0; x < w; x += 16)
            dl->AddRectFilled(ImVec2(p.x + x, p.y + y), ImVec2(p.x + std::min(x + 16, w), p.y + std::min(y + 16, h)),
                              ((int(x / 16) + int(y / 16)) & 1) ? IM_COL32(70, 70, 76, 255) : IM_COL32(50, 50, 56, 255));
        ImGui::Image(n->picture.Id(), ImVec2(w, h));
        ImGui::EndChild();
      }
      break;
    }
    case Kind::kBundle: {
      ImGui::Text("%zu textures. Click one.", n->children.size());
      const float tw = 128 * g_scale;
      const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (tw + 12 * g_scale)));
      ImGui::BeginChild("thumbs", ImVec2(0, 0), false);
      for (size_t i = 0; i < n->children.size(); ++i) {
        if (i % cols) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushID(int(i));
        if (!n->thumbs[i].Empty()) {
          if (ImGui::ImageButton("t", n->thumbs[i].Id(), ImVec2(tw, tw * n->thumbs[i].image.h / std::max(1, n->thumbs[i].image.w)))) Select(n->children[i].get());
        } else if (ImGui::Button("?", ImVec2(tw, tw / 2))) {
          Select(n->children[i].get());
        }
        ImGui::PopID();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(p, ImVec2(p.x + tw, p.y + ImGui::GetTextLineHeight()), true);
        ImGui::TextUnformatted(n->children[i]->label.c_str());
        ImGui::PopClipRect();
        ImGui::EndGroup();
        if (g_sel != n) break;  // (Select changed the selection)
      }
      ImGui::EndChild();
      break;
    }
    case Kind::kBank: BankView(n->data); break;
    case Kind::kStrings: StringsView(n->data); break;
    case Kind::kRoster: RosterView(n->data); break;
    case Kind::kModel: ModelView(n->data); break;
    case Kind::kText: {
      ImGui::BeginChild("text", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
      ImGui::TextUnformatted(reinterpret_cast<const char*>(n->data.data()), reinterpret_cast<const char*>(n->data.data() + std::min<size_t>(n->data.size(), 200000)));
      ImGui::EndChild();
      break;
    }
    case Kind::kPach:
      ImGui::Text("%zu children by id. Expand it in the tree.", n->children.size());
      break;
    case Kind::kFile:
    case Kind::kGroup:
    case Kind::kFolder:
      ImGui::Text("%zu %s. Expand it in the tree.", n->children.size(), n->children.size() == 1 ? "item" : "items");
      break;
    default:
      if (!n->data.empty()) HexView(n->data);
      break;
  }
}

}  // namespace

void TestOpen(const std::string& path) { g_test_path = path; }

void TestSource(const std::wstring& source) {
  g_source = source;
  g_source_name = PathStr(fs::path(source).filename());
}

// Opens the tree along a path of labels and selects the last.
void OpenPath(const std::string& path) {
  Node* n = g_root.get();
  for (size_t at = 0; n && at <= path.size();) {
    size_t end = path.find('/', at);
    if (end == std::string::npos) end = path.size();
    const std::string label = path.substr(at, end - at);
    at = end + 1;
    if (label.empty()) continue;
    Open(*n);
    Node* next = nullptr;
    for (auto& c : n->children)
      if (c->label == label || (c->parent && c->parent->kind == Kind::kPach && std::to_string(c->id) == label)) next = c.get();
    if (!next) { Status("assets: no \"" + label + "\" in " + Path(n)); return; }
    n = next;
  }
  if (n) Select(n);
}

// Where the tree comes from: this game, one of the user's discs, another image or a folder.
void SourceRow() {
  ImGui::SetNextItemWidth(420 * g_scale);
  if (ImGui::BeginCombo("Look in", g_source_name.c_str())) {
    if (ImGui::Selectable("This game (pac folder)", g_source.empty())) g_source.clear(), g_source_name = "This game (pac folder)";
    if (!DiscsReady()) ImGui::TextDisabled("Reading your disc images ...");
    else if (Discs().empty()) ImGui::TextDisabled("No disc images in the disc folders.");
    for (const DiscRef& d : Discs()) {
      const std::string label = d.game + "##" + Utf8(d.path);
      if (ImGui::Selectable(label.c_str(), g_source == d.path)) g_source = d.path, g_source_name = d.game;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", Utf8(d.path).c_str(), d.id.c_str());
    }
    ImGui::Separator();
    if (ImGui::Selectable("A disc image (.iso) ...")) {
      static const COMDLG_FILTERSPEC kIso[1] = {{L"Disc images (*.iso)", L"*.iso"}};
      const std::wstring f = PickFile(false, L"A disc image (Xbox 360 / PS2)", kIso, 1, nullptr, nullptr, OtherGameStart());
      if (!f.empty()) OtherGamePicked(f), g_source = f, g_source_name = PathStr(fs::path(f).filename());
    }
    if (ImGui::Selectable("A folder (an extracted disc) ...")) {
      const std::wstring f = PickFolder(L"A folder with game files", OtherGameStart());
      if (!f.empty()) OtherGamePicked(f), g_source = f, g_source_name = PathStr(fs::path(f).filename());
    }
    ImGui::EndCombo();
  }
  Hint(std::string("Look inside this game, or straight inside your other WWE discs (Xbox 360 and PS2 disc images, nothing "
        "extracted): the disc images in the disc folders are listed by game. "
        "WWE '13 and later pack most files with xcompress; they are unpacked when opened.").c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Disc folders...")) {
    const std::wstring f = PickFolder(L"A folder with your disc images (added to the list)");
    if (!f.empty()) {
      if (g_settings.disc_folders.empty()) g_settings.disc_folders = DiscFolders();
      if (std::find(g_settings.disc_folders.begin(), g_settings.disc_folders.end(), f) == g_settings.disc_folders.end())
        g_settings.disc_folders.push_back(f);
      SaveSettings();
      RescanDiscs();
    }
  }
}

void Draw() {
  Heading("Game assets", "Everything in the game's pac files - or in your other WWE discs: expand a file, click an "
                         "entry to see what it is - textures, models, motion banks, string tables, roster records - "
                         "export any of it, or edit a copy of a pac.");
  if (g_game.empty()) {
    ImGui::TextDisabled("No game folder.");
    return;
  }
  SourceRow();
  const std::wstring want = g_source.empty() ? g_game : g_source;
  if (!g_root || g_root_for != want) {
    g_root = std::make_unique<Node>();
    g_root->kind = Kind::kFolder;
    g_root_for = want;
    g_sel = nullptr;
    ++g_tree_gen;
    if (g_source.empty()) {
      g_root->label = "pac";
      g_root->file = fs::path(g_game) / L"pac";
    } else if (fs::is_directory(g_source)) {
      g_root->label = PathStr(fs::path(g_source).filename());
      g_root->file = g_source;
    } else {
      auto disc = std::make_shared<Disc>();
      std::string err;
      if (!disc->Open(g_source, &err)) {
        Status("That disc image did not open: " + err);
        g_source.clear(), g_source_name = "This game (pac folder)";
        g_root.reset();
        return;
      }
      g_root->label = disc->Id().empty() ? PathStr(fs::path(g_source).filename()) : disc->Id();
      g_root->disc = disc;
      g_root->di = disc->Root();
      g_root->detail = std::string(disc->Kind()) + ", " + std::to_string(disc->Count() - 1) + " files and folders";
      Log("assets: " + g_source_name + " - " + g_root->detail);
    }
    Open(*g_root);
    if (!g_test_path.empty()) OpenPath(g_test_path), g_test_path.clear();
  }
  if (pac_editor::IsOpen()) {  // (the pac editor has a tree of its own: the whole width)
    ImGui::BeginChild("viewer", ImVec2(0, 0), true);
    pac_editor::Draw();
    ImGui::EndChild();
    return;
  }
  ImGui::BeginChild("tree", ImVec2(340 * g_scale, 0), true);
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##find", "find in the open parts of the tree", g_find, sizeof g_find);
  Tree(*g_root, Lower(g_find));
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("viewer", ImVec2(0, 0), true);
  Viewer();
  ImGui::EndChild();
}

}  // namespace assets_page
}  // namespace mm
