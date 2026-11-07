#include "arena.h"

#include <algorithm>
#include <string>
#include <sstream>
#include <map>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

#include "fbx_write.h"
#include "png.h"

namespace fs = std::filesystem;

namespace svrfmt {

namespace {
std::string PathStr(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
}  // namespace

const char* ZoneName(Zone z) {
  switch (z) {
    case Zone::kRing: return "ring";
    case Zone::kRingside: return "ringside";
    case Zone::kEntrance: return "entrance";
    default: return "free";
  }
}

Zone ZoneOf(const std::string& n) {
  auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
  if (starts("ar_post") || starts("ar_tb") || n == "ar_ring" || starts("ar_rope") || starts("ar_cover") ||
      starts("bs_ar_epron") || starts("rop_"))
    return Zone::kRing;
  if (starts("ar_fen") || starts("ar_dwnmt") || starts("ar_sak") || starts("ji_") || starts("ar_case") ||
      starts("ar_ring_sita"))
    return Zone::kRingside;
  if (starts("slo_") || starts("sd_titan") || starts("St_stair"))
    return Zone::kEntrance;
  return Zone::kFree;
}

bool Arena::Load(const std::string& path, std::string* error) {
  Bytes d;
  if (!ReadFile(path, d)) {
    if (error) *error = "cannot read " + path;
    return false;
  }
  return LoadData(d, error);
}

bool Arena::LoadData(const Bytes& d, std::string* error) {
  auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
  if (!EpacRead(d, epac)) return fail("not an arena archive (EPAC)");
  bool found = false;
  for (size_t g = 0; g < epac.groups.size() && !found; ++g)
    for (size_t e = 0; e < epac.groups[g].entries.size() && !found; ++e)
      if (IsPach(epac.groups[g].entries[e].data)) { group = g; entry = e; found = true; }
  if (!found || !PachRead(epac.groups[group].entries[entry].data, entries)) return fail("no stage index (PACH)");
  original_file = d.size();
  models.clear();
  bundles.clear();
  original_unpacked = 0;
  for (const auto& e : entries) {
    const Bytes raw = Unpack(e.data);
    original_unpacked += raw.size();
    if (IsJboy(raw)) {
      ArenaModel am;
      am.id = e.id;
      std::string err;
      if (!JboyRead(raw, am.model, &err)) return fail("model " + std::to_string(e.id) + ": " + err);
      am.zone = ZoneOf(am.model.name);
      models.push_back(std::move(am));
    } else if (IsTextureBundle(raw)) {
      ArenaTextureSet ts;
      ts.id = e.id;
      ts.original_raw = raw.size();
      if (BundleRead(raw, ts.textures)) bundles.push_back(std::move(ts));
    }
  }
  return true;
}

Bytes Arena::Save(std::string* error) const {
  // Entries load in place: packed data must stay smaller than unpacked.
  auto pack = [&](const Bytes& raw, const std::string& what) {
    Bytes p = BpeEncode(raw);
    if (p.size() + 64 > raw.size() && error) *error = what + " does not compress enough to load";
    return p;
  };
  std::vector<PachEntry> out = entries;
  for (const auto& m : models)
    if (m.added) {
      // the game finds entries by binary search: keep ids in order
      auto at = std::find_if(out.begin(), out.end(), [&](const PachEntry& e) { return e.id > m.id; });
      PachEntry pe;
      pe.id = m.id;
      pe.data = pack(JboyWrite(m.model), "model " + m.model.name);
      out.insert(at, std::move(pe));
    }
  for (auto& e : out) {
    bool added = false;
    for (const auto& m : models) added |= m.added && m.id == e.id;
    if (added) continue;
    for (const auto& m : models)
      if (m.id == e.id && m.changed) e.data = pack(JboyWrite(m.model), "model " + m.model.name);
    for (const auto& b : bundles)
      if (b.id == e.id && b.changed) e.data = pack(BundleWrite(b.textures), "texture set");
  }
  Epac e = epac;
  e.groups[group].entries[entry].data = PachWrite(out);
  return EpacWrite(e);
}

std::vector<std::string> Arena::FitBudget(const std::vector<std::string>& keep) {
  std::vector<std::string> reduced;
  for (auto& b : bundles) {
    for (int guard = 0; guard < 64; ++guard) {
      size_t total = 16 + 32 * b.textures.size();
      for (const auto& t : b.textures) total += (t.data.size() + 15) & ~size_t(15);
      if (total <= b.original_raw) break;
      // the largest texture that may be reduced
      BundleTexture* big = nullptr;
      for (auto& t : b.textures) {
        if (std::find(keep.begin(), keep.end(), t.name) != keep.end()) continue;
        DdsInfo info;
        if (!DdsInfoOf(t.data, info) || info.w <= 32 || info.h <= 32) continue;
        if (!big || t.data.size() > big->data.size()) big = &t;
      }
      if (!big) break;
      Image img;
      DdsInfo info;
      if (!DdsInfoOf(big->data, info) || !DdsDecode(big->data, img)) break;
      big->data = DdsEncode(Resize(img, info.w / 2, info.h / 2), info.format, info.mips > 1);
      b.changed = true;
      reduced.push_back(big->name);
    }
  }
  return reduced;
}

std::vector<std::string> Arena::FitFile(const std::vector<std::string>& keep) {
  // (each texture set within its shipped size too - only for an arena kept at its shipped size: a bigger one, 2.1.6,
  // loads its sets whole - its file is read at its own size, the textures' memory takes fallback memory)
  std::vector<std::string> reduced = file_max && StageLast() ? std::vector<std::string>{} : FitBudget(keep);
  for (int guard = 0; guard < 64; ++guard) {
    size_t unpacked = 0;
    for (const auto& e : entries) {
      bool done = false;
      for (const auto& m : models)
        if (m.id == e.id && m.changed) { unpacked += JboyWrite(m.model).size(); done = true; }
      for (const auto& b : bundles)
        if (b.id == e.id && b.changed) { unpacked += BundleWrite(b.textures).size(); done = true; }
      if (!done) unpacked += Unpack(e.data).size();
    }
    for (const auto& m : models)
      if (m.added) unpacked += JboyWrite(m.model).size();
    const size_t file = Save().size();
    const size_t file_budget = file_max && StageLast() ? std::max(file_max, original_file) : original_file;
    const size_t unpacked_budget =
        original_file ? size_t(double(original_unpacked) * double(file_budget) / double(original_file)) : original_unpacked;
    if (file <= size_t(file_budget * fit_margin) && unpacked <= size_t(unpacked_budget * fit_margin)) break;
    BundleTexture* big = nullptr;
    ArenaTextureSet* set = nullptr;
    for (auto& b : bundles)
      for (auto& t : b.textures) {
        if (std::find(keep.begin(), keep.end(), t.name) != keep.end()) continue;
        DdsInfo info;
        if (!DdsInfoOf(t.data, info) || info.w <= 32 || info.h <= 32) continue;
        if (!big || t.data.size() > big->data.size()) { big = &t; set = &b; }
      }
    if (!big) break;
    Image img;
    DdsInfo info;
    if (!DdsInfoOf(big->data, info) || !DdsDecode(big->data, img)) break;
    big->data = DdsEncode(Resize(img, info.w / 2, info.h / 2), info.format, info.mips > 1);
    set->changed = true;
    reduced.push_back(big->name);
  }
  return reduced;
}

bool Arena::StageLast() const {
  // (EpacWrite lays the entries out in group / entry order)
  return group + 1 == epac.groups.size() && entry + 1 == epac.groups[group].entries.size();
}

bool ArenaNeedsNew(const Arena& a, size_t saved_size) {
  return (a.original_file && saved_size > a.original_file) || GeometryOf(a).OverStock();
}

ArenaGeometry GeometryOf(const Arena& a) {
  ArenaGeometry g;
  for (const auto& am : a.models)
    for (const auto& m : am.model.meshes) {
      ++g.meshes;
      g.verts += m.verts.size();
      for (const auto& s : m.strips) g.indices += s.indices.size();
    }
  return g;
}

int FitNodeSpheres(Model& m) {
  int fitted = 0;
  for (size_t i = 0; i < m.nodes.size(); ++i) {
    Node& n = m.nodes[i];
    if (n.t[0] || n.t[1] || n.t[2] || n.r[0] || n.r[1] || n.r[2]) continue;  // (in its own space: left as it is)
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    std::vector<const Mesh*> carried;
    for (const auto& me : m.meshes) {
      const bool mine = m.nodes.size() == 1 || std::find(me.palette.begin(), me.palette.end(), int32_t(i + 1)) != me.palette.end();
      if (!mine || me.verts.empty()) continue;
      carried.push_back(&me);
      for (int j = 0; j < 3; ++j) lo[j] = std::min(lo[j], me.sphere[j] - me.sphere[3]), hi[j] = std::max(hi[j], me.sphere[j] + me.sphere[3]);
    }
    if (carried.empty()) continue;
    const float c[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    float r = 0;
    for (const Mesh* me : carried) {
      const float dx = me->sphere[0] - c[0], dy = me->sphere[1] - c[1], dz = me->sphere[2] - c[2];
      r = std::max(r, std::sqrt(dx * dx + dy * dy + dz * dz) + me->sphere[3]);
    }
    n.sphere[0] = c[0], n.sphere[1] = c[1], n.sphere[2] = c[2], n.sphere[3] = r;
    ++fitted;
  }
  return fitted;
}

int GrowVisibility(Arena& a) {
  PachEntry* vis = nullptr;
  for (auto& e : a.entries)
    if (e.id == 0xC364) vis = &e;
  if (!vis) return 0;
  const Bytes raw = Unpack(vis->data);
  std::string t(raw.begin(), raw.end());
  struct Node {
    size_t at = 0, len = 0;  // the box line (without its line end)
    std::string parent;
    float b[6] = {};
  };
  std::map<std::string, Node> nodes;
  const std::string kNode = "NODE MY_NAME = ";
  for (size_t at = t.find(kNode); at != std::string::npos; at = t.find(kNode, at + 1)) {
    const size_t eol = t.find('\n', at);
    if (eol == std::string::npos) break;
    std::istringstream head(t.substr(at + kNode.size(), eol - at - kNode.size()));
    std::string name, key, eq;
    Node n;
    head >> name >> key >> eq >> n.parent;
    const size_t brace = t.find('{', eol);
    if (brace == std::string::npos) break;
    const size_t bs = t.find('\n', brace);
    if (bs == std::string::npos) break;
    const size_t be = t.find('\n', bs + 1);
    if (be == std::string::npos) break;
    n.at = bs + 1, n.len = be - bs - 1;
    if (n.len && t[n.at + n.len - 1] == '\r') --n.len;
    std::istringstream box(t.substr(n.at, n.len));
    for (float& f : n.b) box >> f;
    if (!box) continue;
    nodes[name] = n;
  }
  auto bounds = [](const Model& m, float lo[3], float hi[3]) {
    for (int j = 0; j < 3; ++j) lo[j] = 1e30f, hi[j] = -1e30f;
    for (const auto& me : m.meshes)
      for (const auto& v : me.verts)
        for (int j = 0; j < 3; ++j) lo[j] = std::min(lo[j], v.pos[j]), hi[j] = std::max(hi[j], v.pos[j]);
    return lo[0] <= hi[0];
  };
  // the Y axis's sign: the one that fits the boxes of models that weren't changed
  float fit_plus = 0, fit_minus = 0;
  for (const auto& am : a.models) {
    auto it = nodes.find(am.model.name);
    float lo[3], hi[3];
    if (am.changed || it == nodes.end() || !bounds(am.model, lo, hi)) continue;
    const float* b = it->second.b;
    fit_plus += std::fabs(b[1] - lo[2] / 10) + std::fabs(b[4] - hi[2] / 10);
    fit_minus += std::fabs(b[1] + hi[2] / 10) + std::fabs(b[4] + lo[2] / 10);
  }
  const bool plus = fit_plus <= fit_minus;
  std::map<std::string, std::array<float, 6>> grown;
  auto grow = [&](const std::string& name, const float g[6]) {
    for (std::string n = name; !n.empty();) {
      auto it = nodes.find(n);
      if (it == nodes.end()) break;
      float* b = it->second.b;
      bool more = false;
      for (int j = 0; j < 3; ++j) {
        if (g[j] < b[j]) b[j] = g[j], more = true;
        if (g[j + 3] > b[j + 3]) b[j + 3] = g[j + 3], more = true;
      }
      if (!more) break;  // (it holds it: so do its parents)
      grown[n] = {b[0], b[1], b[2], b[3], b[4], b[5]};
      n = it->second.parent;
    }
  };
  for (const auto& am : a.models) {
    float lo[3], hi[3];
    if (!am.changed || !nodes.count(am.model.name) || !bounds(am.model, lo, hi)) continue;
    const float g[6] = {lo[0] / 10, plus ? lo[2] / 10 : -hi[2] / 10, -hi[1] / 10,
                        hi[0] / 10, plus ? hi[2] / 10 : -lo[2] / 10, -lo[1] / 10};
    grow(am.model.name, g);
  }
  if (grown.empty()) return 0;
  std::vector<std::pair<size_t, std::string>> order;  // (rewritten from the end: earlier offsets stay valid)
  for (const auto& [n, b] : grown) order.push_back({nodes[n].at, n});
  std::sort(order.rbegin(), order.rend());
  for (const auto& [at, n] : order) {
    const auto& b = grown[n];
    char line[200];
    std::snprintf(line, sizeof line, "\t%g %g %g %g %g %g", b[0], b[1], b[2], b[3], b[4], b[5]);
    t.replace(at, nodes[n].len, line);
  }
  vis->data = BpeEncode(Bytes(t.begin(), t.end()));
  return int(grown.size());
}

BundleTexture* Arena::FindTexture(const std::string& name) {
  for (auto& b : bundles)
    for (auto& t : b.textures)
      if (t.name == name) return &t;
  return nullptr;
}

// ------------------------------------------------------------ export

namespace {

std::string JsonStr(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else if (c >= 0x80) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }  // latin-1 byte
    else o += char(c);
  }
  return o + "\"";
}

std::string Hex4(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%04x", v); return b; }

// Object names must be unique and readable: name__id (ids are unique).
std::string ObjectName(const ArenaModel& m) { return m.model.name + "__" + Hex4(m.id); }

const char* FormatName(DxtFormat f) {
  switch (f) {
    case DxtFormat::kDxt1: return "DXT1";
    case DxtFormat::kDxt3: return "DXT3";
    case DxtFormat::kDxt5: return "DXT5";
    case DxtFormat::kArgb: return "ARGB";
    default: return "unknown";
  }
}

}  // namespace

bool ExportArena(const Arena& a, const std::string& out_dir, const std::string& title, ExportReport& rep) {
  std::error_code ec;
  fs::create_directories(fs::u8path(out_dir) / "textures", ec);
  if (ec) { rep.warnings.push_back("cannot create " + out_dir); return false; }

  // textures -> PNG (first bundle wins on duplicate names)
  std::set<std::string> tex_done;
  std::string tex_json;
  for (const auto& b : a.bundles)
    for (const auto& t : b.textures) {
      if (!tex_done.insert(t.name).second) continue;
      DdsInfo info;
      Image img;
      const bool ok = DdsInfoOf(t.data, info) && DdsDecode(t.data, img);
      if (!ok) {
        ++rep.textures_failed;
        rep.warnings.push_back("texture " + t.name + ": unreadable, kept as is");
      } else if (!SavePng(PathStr(fs::u8path(out_dir) / "textures" / (t.name + ".png")), img)) {
        ++rep.textures_failed;
        rep.warnings.push_back("texture " + t.name + ": PNG write failed");
      } else {
        ++rep.textures;
      }
      if (!tex_json.empty()) tex_json += ",\n";
      tex_json += "    " + JsonStr(t.name) + ": {\"bundle\": " + std::to_string(b.id) + ", \"format\": \"" +
                  FormatName(info.format) + "\", \"width\": " + std::to_string(info.w) + ", \"height\": " +
                  std::to_string(info.h) + ", \"mips\": " + std::to_string(info.mips) + "}";
    }

  // models -> FBX. Game units are 10 cm; game Y points down (see below).
  fbx::Document doc(10.0, 1);
  std::string model_json;
  for (const auto& am : a.models) {
    const Model& m = am.model;
    std::vector<double> verts, normals, uvs, colors;
    std::vector<int32_t> poly, uv_index, col_index, mat_index;
    std::vector<std::string> mat_names, mat_textures;
    int base = 0;
    for (size_t si = 0; si < m.meshes.size(); ++si) {
      const Mesh& s = m.meshes[si];
      for (size_t k = 0; k < s.verts.size(); ++k) {
        const Vertex& v = s.verts[k];
        // game axes: Y points down; a 180-degree turn about X makes it Y-up
        // without mirroring (import applies the same turn back)
        verts.insert(verts.end(), {v.pos[0], -v.pos[1], -v.pos[2]});
        uvs.insert(uvs.end(), {double(s.uvs[k][0]), 1.0 - double(s.uvs[k][1])});
        colors.insert(colors.end(), {(v.color >> 16 & 255) / 255.0, (v.color >> 8 & 255) / 255.0,
                                     (v.color & 255) / 255.0, (v.color >> 24 & 255) / 255.0});
      }
      for (const auto& st : s.strips)
        for (const auto& t : StripToTriangles(st.indices)) {
          for (int c = 0; c < 3; ++c) {
            const int vi = base + t[c];
            poly.push_back(c == 2 ? -(vi + 1) : vi);
            const Vertex& v = s.verts[t[c]];
            normals.insert(normals.end(), {v.normal[0], -v.normal[1], -v.normal[2]});
            uv_index.push_back(vi);
            col_index.push_back(vi);
          }
          mat_index.push_back(int32_t(si));
          ++rep.triangles;
        }
      base += int(s.verts.size());
      const int slot = DiffuseSlot(s);
      mat_names.push_back(ObjectName(am) + "_m" + std::to_string(si));
      mat_textures.push_back(slot >= 0 && slot < int(m.textures.size()) ? m.textures[slot] : "");
      ++rep.meshes;
    }
    if (poly.empty()) continue;
    ++rep.models;
    const std::string oname = ObjectName(am);
    const int64_t gid = doc.NewId(), mid = doc.NewId();
    fbx::Node& g = doc.objects->Add("Geometry");
    g.L(gid).S(fbx::ObjName(oname, "Geometry")).S("Mesh");
    g.Add("GeometryVersion").I(124);
    g.Add("Vertices").Ad(verts);
    g.Add("PolygonVertexIndex").Ai(poly);
    {
      fbx::Node& n = g.Add("LayerElementNormal").I(0);
      n.Add("Version").I(101); n.Add("Name").S("");
      n.Add("MappingInformationType").S("ByPolygonVertex");
      n.Add("ReferenceInformationType").S("Direct");
      n.Add("Normals").Ad(normals);
    }
    {
      fbx::Node& n = g.Add("LayerElementUV").I(0);
      n.Add("Version").I(101); n.Add("Name").S("UVMap");
      n.Add("MappingInformationType").S("ByPolygonVertex");
      n.Add("ReferenceInformationType").S("IndexToDirect");
      n.Add("UV").Ad(uvs);
      n.Add("UVIndex").Ai(uv_index);
    }
    {
      fbx::Node& n = g.Add("LayerElementColor").I(0);
      n.Add("Version").I(101); n.Add("Name").S("Col");
      n.Add("MappingInformationType").S("ByPolygonVertex");
      n.Add("ReferenceInformationType").S("IndexToDirect");
      n.Add("Colors").Ad(colors);
      n.Add("ColorIndex").Ai(col_index);
    }
    {
      fbx::Node& n = g.Add("LayerElementMaterial").I(0);
      n.Add("Version").I(101); n.Add("Name").S("");
      n.Add("MappingInformationType").S("ByPolygon");
      n.Add("ReferenceInformationType").S("IndexToDirect");
      n.Add("Materials").Ai(mat_index);
    }
    {
      fbx::Node& l = g.Add("Layer").I(0);
      l.Add("Version").I(100);
      for (const char* t : {"LayerElementNormal", "LayerElementUV", "LayerElementColor", "LayerElementMaterial"}) {
        fbx::Node& le = l.Add("LayerElement");
        le.Add("Type").S(t);
        le.Add("TypedIndex").I(0);
      }
    }
    fbx::Node& mo = doc.objects->Add("Model");
    mo.L(mid).S(fbx::ObjName(oname, "Model")).S("Mesh");
    mo.Add("Version").I(232);
    {
      fbx::Node& p = mo.Add("Properties70");
      fbx::PCustomInt(p, "svr_id", int32_t(am.id));
      fbx::PString(p, "svr_name", m.name, true);
      fbx::PString(p, "svr_zone", ZoneName(am.zone), true);
    }
    mo.Add("Shading").C(true);
    mo.Add("Culling").S("CullingOff");
    doc.Connect(mid, 0);
    doc.Connect(gid, mid);
    for (size_t si = 0; si < mat_names.size(); ++si) {
      const int64_t matid = doc.NewId();
      fbx::Node& mat = doc.objects->Add("Material");
      mat.L(matid).S(fbx::ObjName(mat_names[si], "Material")).S("");
      mat.Add("Version").I(102);
      mat.Add("ShadingModel").S("phong");
      mat.Add("MultiLayer").I(0);
      fbx::Node& p = mat.Add("Properties70");
      fbx::PVec(p, "DiffuseColor", "Color", 1, 1, 1);
      fbx::PString(p, "svr_shader", m.meshes[si].shader, true);
      doc.Connect(matid, mid);
      if (!mat_textures[si].empty() && tex_done.count(mat_textures[si])) {
        const std::string rel = "textures/" + mat_textures[si] + ".png";
        const std::string abs = PathStr(fs::u8path(out_dir) / "textures" / (mat_textures[si] + ".png"));
        const int64_t tid = doc.NewId(), vid = doc.NewId();
        fbx::Node& tx = doc.objects->Add("Texture");
        tx.L(tid).S(fbx::ObjName(mat_textures[si], "Texture")).S("");
        tx.Add("Type").S("TextureVideoClip");
        tx.Add("Version").I(202);
        tx.Add("TextureName").S(fbx::ObjName(mat_textures[si], "Texture"));
        tx.Add("Media").S(fbx::ObjName(mat_textures[si], "Video"));
        tx.Add("FileName").S(abs);
        tx.Add("RelativeFilename").S(rel);
        fbx::Node& vd = doc.objects->Add("Video");
        vd.L(vid).S(fbx::ObjName(mat_textures[si], "Video")).S("Clip");
        vd.Add("Type").S("Clip");
        fbx::Node& vp = vd.Add("Properties70");
        vp.Add("P").S("Path").S("KString").S("XRefUrl").S("").S(abs);
        vd.Add("UseMipMap").I(0);
        vd.Add("Filename").S(abs);
        vd.Add("RelativeFilename").S(rel);
        doc.Connect(vid, tid);
        doc.Connect(tid, matid, "DiffuseColor");
      }
    }
    // arena.json entry
    std::string meshes;
    for (size_t si = 0; si < m.meshes.size(); ++si) {
      const Mesh& s = m.meshes[si];
      if (!meshes.empty()) meshes += ", ";
      meshes += "{\"shader\": " + JsonStr(s.shader) + ", \"format\": " + std::to_string(s.vfmt) +
                ", \"texture\": " + JsonStr(mat_textures[si]) + ", \"skinned\": " +
                (s.palette.size() > 1 ? "true" : "false") + "}";
    }
    if (!model_json.empty()) model_json += ",\n";
    model_json += "    " + JsonStr(oname) + ": {\"id\": " + std::to_string(am.id) + ", \"name\": " + JsonStr(m.name) +
                  ", \"zone\": \"" + ZoneName(am.zone) + "\", \"nodes\": " + std::to_string(m.nodes.size()) +
                  ", \"meshes\": [" + meshes + "]}";
  }
  if (!WriteFile(PathStr(fs::u8path(out_dir) / "arena.fbx"), doc.Write())) {
    rep.warnings.push_back("cannot write arena.fbx");
    return false;
  }
  const std::string json = "{\n  \"format\": \"svr2011-arena-export\",\n  \"version\": 1,\n  \"arena\": " +
                           JsonStr(title) + ",\n  \"units\": \"1 game unit = 10 cm (FBX unit scale 10); FBX = game axes turned 180 degrees "
                           "about X (game Y points down)\",\n" +
                           "  \"models\": {\n" + model_json + "\n  },\n  \"textures\": {\n" + tex_json + "\n  }\n}\n";
  Bytes jb(json.begin(), json.end());
  WriteFile(PathStr(fs::u8path(out_dir) / "arena.json"), jb);
  return true;
}

}  // namespace svrfmt
