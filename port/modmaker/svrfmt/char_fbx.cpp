// A superstar's model to FBX (char_fbx.h). The model as the 3D view reads it
// (char_preview.cpp): an attire's EMD entry is a PACH whose JBOY children are
// the models (child 0 the body, with the whole skeleton; the others' nodes are
// matched to it by name), their vertices already in the body's model space
// (the bind pose); a mesh's weights name palette slots, the palette holds the
// model's node index + 1. A node's local matrix is T(t) R(r), R = Rz Ry Rx.
//
// FBX: the same axes turn as the arena export (180 degrees about X: game Y
// points down), units 10 cm (unit scale 10). Bones are LimbNodes whose
// Lcl Translation / Rotation are the bind pose (so the file's rest pose is the
// game's); each mesh model gets a Skin with a Cluster per bone it uses
// (TransformLink = the bone's bind world matrix, Transform = its inverse
// times the mesh's world, as Blender writes them), and a BindPose.
#include "char_fbx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

#include "fbx_write.h"
#include "jboy.h"
#include "pac.h"
#include "png.h"
#include "texture.h"
#include "ufbx.h"

namespace svrfmt {

namespace fs = std::filesystem;

namespace {

std::string PathStr(const fs::path& p) {  // (UTF-8, as the file helpers take)
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

struct M {  // 3x3 rotation + translation, column vectors: p' = r p + t
  double r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  double t[3] = {0, 0, 0};
};
M Mul(const M& a, const M& b) {
  M o;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
    o.t[i] = a.r[i][0] * b.t[0] + a.r[i][1] * b.t[1] + a.r[i][2] * b.t[2] + a.t[i];
  }
  return o;
}
M Inv(const M& a) {
  M o;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[j][i];
  for (int i = 0; i < 3; ++i) o.t[i] = -(o.r[i][0] * a.t[0] + o.r[i][1] * a.t[1] + o.r[i][2] * a.t[2]);
  return o;
}
M Local(const float t[3], const float e[3]) {  // T(t) Rz Ry Rx
  M m;
  const double cx = std::cos(e[0]), sx = std::sin(e[0]), cy = std::cos(e[1]), sy = std::sin(e[1]), cz = std::cos(e[2]), sz = std::sin(e[2]);
  m.r[0][0] = cz * cy, m.r[0][1] = cz * sy * sx - sz * cx, m.r[0][2] = cz * sy * cx + sz * sx;
  m.r[1][0] = sz * cy, m.r[1][1] = sz * sy * sx + cz * cx, m.r[1][2] = sz * sy * cx - cz * sx;
  m.r[2][0] = -sy, m.r[2][1] = cy * sx, m.r[2][2] = cy * cx;
  for (int k = 0; k < 3; ++k) m.t[k] = t[k];
  return m;
}
// R = Rz Ry Rx -> (x, y, z) in degrees (FBX's eEulerXYZ)
std::array<double, 3> EulerDeg(const double r[3][3]) {
  const double sy = std::clamp(-r[2][0], -1.0, 1.0);
  const double ey = std::asin(sy);
  double ex, ez;
  if (std::fabs(sy) < 0.99999) ex = std::atan2(r[2][1], r[2][2]), ez = std::atan2(r[1][0], r[0][0]);
  else ex = std::atan2(-r[1][2], r[1][1]), ez = 0;
  const double k = 180.0 / 3.14159265358979323846;
  return {ex * k, ey * k, ez * k};
}
// The axes turn (180 degrees about X): C = diag(1, -1, -1)
M Turn(const M& a) {
  M o = a;
  for (int j = 0; j < 3; ++j) o.r[1][j] = -a.r[1][j], o.r[2][j] = -a.r[2][j];
  o.t[1] = -a.t[1], o.t[2] = -a.t[2];
  return o;
}
std::vector<double> FbxMatrix(const M& m) {  // FBX stores the transpose (rows = basis vectors, translation last)
  return {m.r[0][0], m.r[1][0], m.r[2][0], 0, m.r[0][1], m.r[1][1], m.r[2][1], 0,
          m.r[0][2], m.r[1][2], m.r[2][2], 0, m.t[0],    m.t[1],    m.t[2],    1};
}

bool IsBundle(const Bytes& b) { return b.size() > 16 && Le32(&b[4]) == 0x100 && Le32(&b[12]) == 0x10; }
// A texture bundle: count, then 32-byte records {name[16], "dds", size, offset}
void ReadBundle(const Bytes& b, std::map<std::string, Bytes>& out) {
  const uint32_t n = Le32(&b[0]);
  for (uint32_t i = 0; i < n && 16 + 32 * (i + 1) <= b.size(); ++i) {
    const uint8_t* r = &b[16 + 32 * i];
    const std::string name(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 16));
    const uint32_t size = Le32(r + 20), off = Le32(r + 24);
    if (off + size <= b.size() && !out.count(name)) out[name] = Bytes(b.begin() + off, b.begin() + off + size);
  }
}

struct Bone {
  std::string name;
  int parent = -1;
  M local, world;  // (game axes)
};
struct Part {
  std::string name, texture;
  std::string normal_texture;  // (texNormal: its alpha is a skin's shine mask - exported as the material's NormalMap)
  std::vector<std::array<double, 3>> pos, nrm;
  std::vector<std::array<double, 2>> uv;
  std::vector<std::array<std::pair<int, double>, 4>> inf;  // (bone, weight), weight 0 = none
  std::vector<std::array<int, 3>> tris;
};
struct Character {
  std::vector<Bone> bones;
  std::vector<Part> parts;
  std::map<std::string, Bytes> textures;  // name -> DDS
};

bool ReadCharacter(const Bytes& pac, int attire, Character& ch, std::string& err) {
  Epac e;
  if (!EpacRead(pac, e)) return err = "not a model pac", false;
  const char want = char('0' + std::clamp(attire, 1, 9) - 1);
  const EpacEntry* entry = nullptr;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name.size() < 8 || en.name[7] != '2' || en.data.empty()) continue;
      if (en.name[6] == want) entry = &en;
      std::vector<PachEntry> kids;  // (the textures of every attire: attire 2 can use attire 1's sheets)
      if (PachRead(Unpack(en.data), kids))
        for (const auto& k : kids)
          if (const Bytes raw = Unpack(k.data); IsBundle(raw)) ReadBundle(raw, ch.textures);
    }
  if (!entry) return err = "the pac has no attire " + std::to_string(attire), false;
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(entry->data), kids)) return err = "the attire is not a PACH", false;
  std::vector<std::pair<uint32_t, Model>> models;
  for (const auto& k : kids) {
    const Bytes raw = Unpack(k.data);
    Model m;
    if (IsJboy(raw) && JboyRead(raw, m)) models.push_back({k.id, std::move(m)});
  }
  if (models.empty() || models[0].second.nodes.empty()) return err = "no body model (child 0) with a skeleton", false;
  const Model& body = models[0].second;
  std::map<std::string, int> by_name;
  for (const auto& n : body.nodes) {
    Bone b;
    b.name = n.name;
    b.parent = n.parent >= 0 && n.parent < int(body.nodes.size()) ? n.parent : -1;
    b.local = Local(n.t, n.r);
    by_name.emplace(b.name, int(ch.bones.size()));
    ch.bones.push_back(b);
  }
  // the other models' own bones (the hair model's kami_b chains: parent at +48, each strand root under atama)
  // after the body's, so their weights stay on them instead of falling back to root
  for (size_t mi = 1; mi < models.size(); ++mi) {
    const Model& m = models[mi].second;
    std::function<int(int, int)> add = [&](int local, int depth) -> int {
      if (local < 0 || local >= int(m.nodes.size()) || depth > 64) return -1;
      const auto& n = m.nodes[size_t(local)];
      if (const auto it = by_name.find(n.name); it != by_name.end()) return it->second;
      Bone b;
      b.name = n.name;
      b.local = Local(n.t, n.r);
      b.parent = n.parent != local ? add(n.parent, depth + 1) : -1;
      by_name.emplace(b.name, int(ch.bones.size()));
      ch.bones.push_back(b);
      return int(ch.bones.size()) - 1;
    };
    for (int i = 0; i < int(m.nodes.size()); ++i) add(i, 0);
  }
  std::vector<bool> done(ch.bones.size());
  std::function<void(int)> world = [&](int i) {
    if (done[size_t(i)]) return;
    Bone& b = ch.bones[size_t(i)];
    if (b.parent >= 0 && b.parent != i) world(b.parent), b.world = Mul(ch.bones[size_t(b.parent)].world, b.local);
    else b.world = b.local;
    done[size_t(i)] = true;
  };
  for (int i = 0; i < int(ch.bones.size()); ++i) world(i);
  for (const auto& [id, m] : models) {
    if (m.name.rfind("blood", 0) == 0 || (id >= 0x7530 && id <= 0x7532)) continue;  // (blood overlays)
    for (size_t si = 0; si < m.meshes.size(); ++si) {
      const Mesh& s = m.meshes[si];
      Part p;
      p.name = (m.name.empty() ? "model" + std::to_string(id) : m.name) + (m.meshes.size() > 1 ? "_" + std::to_string(si) : "");
      const int slot = DiffuseSlot(s);
      p.texture = slot >= 0 && slot < int(m.textures.size()) ? m.textures[size_t(slot)] : "";
      const int nslot = DiffuseSlot(s, "texNormal");
      p.normal_texture = nslot >= 0 && nslot < int(m.textures.size()) ? m.textures[size_t(nslot)] : "";
      auto bone_of = [&](int palette_slot) {
        if (palette_slot < 0 || palette_slot >= int(s.palette.size())) return 0;
        const int local = s.palette[size_t(palette_slot)] - 1;
        if (local < 0 || local >= int(m.nodes.size())) return 0;
        const auto it = by_name.find(m.nodes[size_t(local)].name);
        return it == by_name.end() ? 0 : it->second;
      };
      const size_t vc = s.verts.size();
      for (size_t k = 0; k < vc; ++k) {
        p.pos.push_back({s.verts[k].pos[0], s.verts[k].pos[1], s.verts[k].pos[2]});
        p.nrm.push_back({s.verts[k].normal[0], s.verts[k].normal[1], s.verts[k].normal[2]});
        p.uv.push_back(k < s.uvs.size() ? std::array<double, 2>{s.uvs[k][0], s.uvs[k][1]} : std::array<double, 2>{0, 0});
        std::array<std::pair<int, double>, 4> f{};
        int n = 0;
        double sum = 0;
        for (size_t j = 0; j < s.weights.size() && n < 4; ++j) {
          if (k >= s.weights[j].size() || s.weights[j][k].bones[0] == 255) continue;
          f[size_t(n)] = {bone_of(s.weights[j][k].bones[0]), s.weights[j][k].weight};
          sum += f[size_t(n++)].second;
        }
        if (n == 0 || sum <= 0) f = {}, f[0] = {bone_of(0), 1.0};
        else
          for (int j = 0; j < n; ++j) f[size_t(j)].second /= sum;
        p.inf.push_back(f);
      }
      for (const auto& st : s.strips)
        for (const auto& t : StripToTriangles(st.indices))
          if (t[0] < vc && t[1] < vc && t[2] < vc) p.tris.push_back({t[0], t[1], t[2]});
      if (!p.tris.empty()) ch.parts.push_back(std::move(p));
    }
  }
  if (ch.parts.empty()) return err = "the attire has no meshes", false;
  return true;
}

}  // namespace

std::vector<std::string> CheckHairSettings(const Bytes& pac) {
  std::vector<std::string> out;
  Epac e;
  if (!EpacRead(pac, e)) return out;
  int attire = 0;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      std::vector<PachEntry> kids;
      if (!PachRead(Unpack(en.data), kids)) continue;
      ++attire;
      // (a chain the models haven't is fine: 23 stock attires name some)
      const PachEntry* text = nullptr;
      for (const auto& k : kids)
        if (k.id == 0x3C) text = &k;
      if (!text) continue;
      const Bytes t = Unpack(text->data);
      std::map<std::string, int> named;
      int lines = 0;
      size_t at = 0;
      while (at < t.size()) {
        size_t nl = at;
        while (nl < t.size() && t[nl] != '\n') ++nl;
        std::vector<std::string> tok;
        std::string cur;
        for (size_t i = at; i < nl; ++i) {
          const char c = char(t[i]);
          if (c == ' ' || c == '\t' || c == '\r') {
            if (!cur.empty()) tok.push_back(cur), cur.clear();
          } else {
            cur += c;
          }
        }
        if (!cur.empty()) tok.push_back(cur);
        at = nl + 1;
        if (tok.size() < 10 || tok[0][0] == ';') continue;  // (a chain line: name + 9 numbers)
        ++lines;
        ++named[tok[0]];
      }
      const std::string who = "attire " + std::to_string(attire) + " (" + en.name + ")";
      for (const auto& [name, n] : named)
        if (n > 1) out.push_back(who + ": its hair settings name " + name + " " + std::to_string(n) + " times");
      if (lines > kHairLinesMax)
        out.push_back(who + ": its hair settings list " + std::to_string(lines) + " chains (the game takes at most " +
                      std::to_string(kHairLinesMax) + ")");
      if (!named.empty()) {  // (moving hair: the models must fit the room the largest stock attire's take)
        size_t bytes = 0;
        for (const auto& k : kids) {
          const Bytes d = Unpack(k.data);
          if (d.size() >= 4 && !std::memcmp(d.data(), "JBOY", 4)) bytes += d.size();
        }
        if (bytes > kChainModelBytesMax)
          out.push_back(who + ": with moving hair its models take " + std::to_string(bytes / 1024) + " KB, more than the game has room for (" +
                        std::to_string(kChainModelBytesMax / 1024) + " KB)");
      }
    }
  return out;
}

CharModelIssues CheckCharacterModel(const Bytes& pac, int attire) {
  CharModelIssues out;
  out.eye_off[0] = out.eye_off[1] = -1;
  out.hair = CheckHairSettings(pac);
  {
    std::vector<EpacRow> rows;
    std::map<char, std::string> first;
    if (pac.size() >= 0x4000 && EpacTable(Bytes(pac.begin(), pac.begin() + 0x4000), 0, rows))
      for (const auto& r : rows) {
        const std::string& n = r.name;
        if (r.group != "EMD " || n.size() != 8 || n[7] != '2' || !std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; }))
          continue;
        auto [it, fresh] = first.emplace(n[6], n);
        if (!fresh)
          out.attire_clash.push_back("entries " + it->second + " and " + n + " are both attire " + std::string(1, char(n[6] + 1)));
      }
  }
  Character ch;
  std::string err;
  if (!ReadCharacter(pac, attire, ch, err)) return out;
  out.read = true;
  for (const auto& p : ch.parts) {
    bool bad = false;
    for (size_t k = 0; k < p.pos.size() && !bad; ++k)
      for (int c = 0; c < 3; ++c) bad |= !std::isfinite(p.pos[k][size_t(c)]);  // (positions: a stock model has NaN normals / UVs on unused vertices)
    if (bad) out.nan_meshes.push_back(p.name);
  }
  const char* eyes[2] = {"l_eye", "r_eye"};
  for (int e = 0; e < 2; ++e) {
    int bone = -1;
    for (size_t i = 0; i < ch.bones.size(); ++i)
      if (ch.bones[i].name == eyes[e]) bone = int(i);
    if (bone < 0) continue;
    double sum[3] = {0, 0, 0};
    int n = 0;
    for (const auto& p : ch.parts)
      for (size_t k = 0; k < p.pos.size(); ++k)
        if (p.inf[k][0].first == bone && p.inf[k][0].second > 0.5 && std::isfinite(p.pos[k][0]))
          for (int c = 0; c < 3; ++c) sum[c] += p.pos[k][size_t(c)], n += c == 0;
    if (!n) continue;
    const M& w = ch.bones[size_t(bone)].world;
    out.eye_off[e] = std::sqrt(std::pow(sum[0] / n - w.t[0], 2) + std::pow(sum[1] / n - w.t[1], 2) + std::pow(sum[2] / n - w.t[2], 2));
  }
  return out;
}

bool IsSecondaryBoneName(const std::string& name) {
  std::string l;
  for (char c : name) l += char(std::tolower(uint8_t(c)));
  return l.rfind("j_sec_", 0) == 0 || l.find("_sec_") != std::string::npos;
}

bool IsClothBoneName(const std::string& name) {
  std::string l;
  for (char c : name) l += char(std::tolower(uint8_t(c)));
  for (const char* k : {"jacket", "coat", "skirt", "cape", "cloak", "cloth", "scarf", "ribbon", "sleeve", "hood", "robe",
                        "dress", "apron", "tassel", "parka", "hoodie", "physbone", "spring"})
    if (l.find(k) != std::string::npos) return true;
  return false;
}

FbxChains FindFbxChains(const std::vector<FbxBone>& bones, const std::vector<std::string>& targets, int hair,
                        const std::map<std::string, bool>& cloth) {
  FbxChains r;
  const size_t nb = std::min(bones.size(), targets.size());
  std::vector<std::vector<int>> kids(nb);
  for (size_t i = 0; i < nb; ++i)
    if (bones[i].parent >= 0 && size_t(bones[i].parent) < nb) kids[size_t(bones[i].parent)].push_back(int(i));
  int head = -1;
  for (size_t i = 0; i < nb && head < 0; ++i)
    if (targets[i] == "atama") head = int(i);
  auto under = [&](int i, int a) {
    for (int p = bones[size_t(i)].parent, n = 0; p >= 0 && n <= int(nb); p = bones[size_t(p)].parent, ++n)
      if (p == a) return true;
    return false;
  };
  auto is_cloth = [&](int i) {
    if (!targets[size_t(i)].empty()) return false;
    const auto o = cloth.find(bones[size_t(i)].name);
    return o != cloth.end() ? o->second : IsClothBoneName(bones[size_t(i)].name);
  };
  auto is_hair = [&](int i) {
    return hair != 0 && head >= 0 && targets[size_t(i)].empty() && !is_cloth(i) && IsHairBoneName(bones[size_t(i)].name) && under(i, head);
  };
  // (runs: from where one starts - its parent isn't one, or branches - down its single children)
  auto runs = [&](const std::function<bool(int)>& in, size_t need, std::vector<std::vector<int>>& out, int& short_ones) {
    for (size_t i = 0; i < nb; ++i) {
      const int pa = bones[i].parent;
      if (!in(int(i)) || (pa >= 0 && in(pa) && kids[size_t(pa)].size() == 1)) continue;
      std::vector<int> s{int(i)};
      while (kids[size_t(s.back())].size() == 1 && in(kids[size_t(s.back())][0]) && s.size() < 24) s.push_back(kids[size_t(s.back())][0]);
      if (s.size() >= need) out.push_back(std::move(s));
      else if (s.size() >= 2) ++short_ones;
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
  };
  std::vector<std::vector<int>> hair_runs;
  runs(is_hair, 4, hair_runs, r.short_hair);
  for (auto& h : hair_runs) {
    if (hair < 0 && IsSecondaryBoneName(bones[size_t(h[0])].name)) ++r.secondary;
    else r.hair.push_back(std::move(h));
  }
  runs(is_cloth, 3, r.cloth, r.short_cloth);
  if (r.hair.size() > 16) r.over += int(r.hair.size() - 16), r.hair.resize(16);
  if (r.hair.size() + r.cloth.size() > 16) r.over += int(r.hair.size() + r.cloth.size() - 16), r.cloth.resize(16 - r.hair.size());
  return r;
}

bool IsHairBoneName(const std::string& name) {
  std::string l;
  for (char c : name) l += char(std::tolower(uint8_t(c)));
  for (const char* k : {"hair", "ponytail", "pony_tail", "braid", "bang", "fringe", "pigtail", "twintail", "dread", "kami", "tress"})
    if (l.find(k) != std::string::npos) return true;
  return false;
}

bool ExportCharacterFbx(const Bytes& pac, int attire, const std::string& out_dir, const std::string& name, CharExportReport& rep) {
  Character ch;
  std::string err;
  if (!ReadCharacter(pac, attire, ch, err)) {
    rep.warnings.push_back(err);
    return false;
  }
  std::error_code ec;
  fs::create_directories(fs::u8path(out_dir) / "textures", ec);
  // textures -> PNG
  std::set<std::string> written;
  for (const auto& [tname, dds] : ch.textures) {
    Image img;
    if (DdsDecode(dds, img) && SavePng(PathStr(fs::u8path(out_dir) / "textures" / fs::u8path(tname + ".png")), img)) {
      written.insert(tname);
      ++rep.textures;
    } else {
      rep.warnings.push_back("texture " + tname + ": not written");
    }
  }
  fbx::Document doc(10.0, 1);
  // the skeleton: LimbNodes in the bind pose (roots turned with the axes)
  std::vector<int64_t> bone_id(ch.bones.size());
  std::vector<M> bind(ch.bones.size());
  for (size_t i = 0; i < ch.bones.size(); ++i) {
    const Bone& b = ch.bones[i];
    bind[i] = Turn(b.world);
    const M local = b.parent < 0 ? Turn(b.local) : b.local;
    const int64_t id = doc.NewId(), attr = doc.NewId();
    bone_id[i] = id;
    fbx::Node& na = doc.objects->Add("NodeAttribute");
    na.L(attr).S(fbx::ObjName(b.name, "NodeAttribute")).S("LimbNode");
    na.Add("TypeFlags").S("Skeleton");
    fbx::Node& mo = doc.objects->Add("Model");
    mo.L(id).S(fbx::ObjName(b.name, "Model")).S("LimbNode");
    mo.Add("Version").I(232);
    fbx::Node& p = mo.Add("Properties70");
    const auto e = EulerDeg(local.r);
    fbx::PVec(p, "Lcl Translation", "Lcl Translation", local.t[0], local.t[1], local.t[2]);
    fbx::PVec(p, "Lcl Rotation", "Lcl Rotation", e[0], e[1], e[2]);
    mo.Add("Shading").C(true);
    mo.Add("Culling").S("CullingOff");
    doc.Connect(attr, id);
  }
  for (size_t i = 0; i < ch.bones.size(); ++i) doc.Connect(bone_id[i], ch.bones[i].parent >= 0 ? bone_id[size_t(ch.bones[i].parent)] : 0);
  rep.bones = int(ch.bones.size());
  // the bind pose (every bone and mesh model's world matrix)
  fbx::Node& pose = doc.objects->Add("Pose");
  const int64_t pose_id = doc.NewId();
  pose.L(pose_id).S(fbx::ObjName("BindPose", "Pose")).S("BindPose");
  pose.Add("Type").S("BindPose");
  pose.Add("Version").I(100);
  fbx::Node& npn = pose.Add("NbPoseNodes");
  int pose_nodes = 0;
  for (size_t i = 0; i < ch.bones.size(); ++i) {
    fbx::Node& pn = pose.Add("PoseNode");
    pn.Add("Node").L(bone_id[i]);
    pn.Add("Matrix").Ad(FbxMatrix(bind[i]));
    ++pose_nodes;
  }
  // the meshes, each skinned
  for (const Part& part : ch.parts) {
    std::vector<double> verts, normals, uvs;
    std::vector<int32_t> poly, uv_index;
    for (size_t k = 0; k < part.pos.size(); ++k) {
      verts.insert(verts.end(), {part.pos[k][0], -part.pos[k][1], -part.pos[k][2]});
      uvs.insert(uvs.end(), {part.uv[k][0], 1.0 - part.uv[k][1]});
    }
    // (the game's faces turned over - its normals and winding point in: FbxToBuildInput turns them back)
    for (const auto& t : part.tris)
      for (int k = 0; k < 3; ++k) {
        const int c = k == 0 ? 0 : 3 - k;  // (0, 2, 1)
        poly.push_back(k == 2 ? -(t[size_t(c)] + 1) : t[size_t(c)]);
        const auto& n = part.nrm[size_t(t[size_t(c)])];
        normals.insert(normals.end(), {-n[0], n[1], n[2]});
        uv_index.push_back(t[size_t(c)]);
      }
    const int64_t gid = doc.NewId(), mid = doc.NewId();
    fbx::Node& g = doc.objects->Add("Geometry");
    g.L(gid).S(fbx::ObjName(part.name, "Geometry")).S("Mesh");
    g.Add("GeometryVersion").I(124);
    g.Add("Vertices").Ad(verts);
    g.Add("PolygonVertexIndex").Ai(poly);
    {
      fbx::Node& n = g.Add("LayerElementNormal").I(0);
      n.Add("Version").I(101), n.Add("Name").S("");
      n.Add("MappingInformationType").S("ByPolygonVertex");
      n.Add("ReferenceInformationType").S("Direct");
      n.Add("Normals").Ad(normals);
    }
    {
      fbx::Node& n = g.Add("LayerElementUV").I(0);
      n.Add("Version").I(101), n.Add("Name").S("UVMap");
      n.Add("MappingInformationType").S("ByPolygonVertex");
      n.Add("ReferenceInformationType").S("IndexToDirect");
      n.Add("UV").Ad(uvs);
      n.Add("UVIndex").Ai(uv_index);
    }
    {
      fbx::Node& n = g.Add("LayerElementMaterial").I(0);
      n.Add("Version").I(101), n.Add("Name").S("");
      n.Add("MappingInformationType").S("AllSame");
      n.Add("ReferenceInformationType").S("IndexToDirect");
      n.Add("Materials").Ai({0});
    }
    {
      fbx::Node& l = g.Add("Layer").I(0);
      l.Add("Version").I(100);
      for (const char* t : {"LayerElementNormal", "LayerElementUV", "LayerElementMaterial"}) {
        fbx::Node& le = l.Add("LayerElement");
        le.Add("Type").S(t);
        le.Add("TypedIndex").I(0);
      }
    }
    fbx::Node& mo = doc.objects->Add("Model");
    mo.L(mid).S(fbx::ObjName(part.name, "Model")).S("Mesh");
    mo.Add("Version").I(232);
    mo.Add("Properties70");
    mo.Add("Shading").C(true);
    mo.Add("Culling").S("CullingOff");
    doc.Connect(mid, 0);
    doc.Connect(gid, mid);
    {
      fbx::Node& pn = pose.Add("PoseNode");
      pn.Add("Node").L(mid);
      pn.Add("Matrix").Ad(FbxMatrix(M()));
      ++pose_nodes;
    }
    // material + its texture
    const int64_t matid = doc.NewId();
    fbx::Node& mat = doc.objects->Add("Material");
    mat.L(matid).S(fbx::ObjName(part.name + "_mat", "Material")).S("");
    mat.Add("Version").I(102);
    mat.Add("ShadingModel").S("phong");
    mat.Add("MultiLayer").I(0);
    fbx::PVec(mat.Add("Properties70"), "DiffuseColor", "Color", 1, 1, 1);
    doc.Connect(matid, mid);
    auto written_name = [&](const std::string& want) {  // (as named in the bundles: case may differ)
      std::string t;
      for (const auto& w : written)
        if (!want.empty() && (w == want || std::equal(w.begin(), w.end(), want.begin(), want.end(),
                                                      [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); })))
          t = w;
      return t;
    };
    for (const auto& [tex, slot_name] : {std::make_pair(written_name(part.texture), "DiffuseColor"),
                                         std::make_pair(written_name(part.normal_texture), "NormalMap")}) {
      if (tex.empty()) continue;
      const std::string rel = "textures/" + tex + ".png";
      const std::string abs = PathStr(fs::u8path(out_dir) / "textures" / fs::u8path(tex + ".png"));
      const int64_t tid = doc.NewId(), vid = doc.NewId();
      fbx::Node& tx = doc.objects->Add("Texture");
      tx.L(tid).S(fbx::ObjName(tex, "Texture")).S("");
      tx.Add("Type").S("TextureVideoClip");
      tx.Add("Version").I(202);
      tx.Add("TextureName").S(fbx::ObjName(tex, "Texture"));
      tx.Add("Media").S(fbx::ObjName(tex, "Video"));
      tx.Add("FileName").S(abs);
      tx.Add("RelativeFilename").S(rel);
      fbx::Node& vd = doc.objects->Add("Video");
      vd.L(vid).S(fbx::ObjName(tex, "Video")).S("Clip");
      vd.Add("Type").S("Clip");
      vd.Add("Properties70").Add("P").S("Path").S("KString").S("XRefUrl").S("").S(abs);
      vd.Add("UseMipMap").I(0);
      vd.Add("Filename").S(abs);
      vd.Add("RelativeFilename").S(rel);
      doc.Connect(vid, tid);
      doc.Connect(tid, matid, slot_name);
    }
    // the skin: a cluster per bone the mesh uses
    std::map<int, std::pair<std::vector<int32_t>, std::vector<double>>> by_bone;
    for (size_t k = 0; k < part.inf.size(); ++k)
      for (const auto& [b, w] : part.inf[k])
        if (w > 0) by_bone[b].first.push_back(int32_t(k)), by_bone[b].second.push_back(w);
    const int64_t skin = doc.NewId();
    fbx::Node& sk = doc.objects->Add("Deformer");
    sk.L(skin).S(fbx::ObjName(part.name, "Deformer")).S("Skin");
    sk.Add("Version").I(101);
    sk.Add("Link_DeformAcuracy").D(50);
    doc.Connect(skin, gid);
    for (auto& [b, iw] : by_bone) {
      const int64_t cl = doc.NewId();
      fbx::Node& c = doc.objects->Add("Deformer");
      c.L(cl).S(fbx::ObjName(ch.bones[size_t(b)].name, "SubDeformer")).S("Cluster");
      c.Add("Version").I(100);
      c.Add("UserData").S("").S("");
      c.Add("Indexes").Ai(iw.first);
      c.Add("Weights").Ad(iw.second);
      c.Add("Transform").Ad(FbxMatrix(Inv(bind[size_t(b)])));  // (inverse bind times the mesh's world: identity)
      c.Add("TransformLink").Ad(FbxMatrix(bind[size_t(b)]));
      doc.Connect(cl, skin);
      doc.Connect(bone_id[size_t(b)], cl);
    }
    ++rep.models;
    ++rep.meshes;
    rep.vertices += int(part.pos.size());
    rep.triangles += int(part.tris.size());
  }
  npn.I(pose_nodes);
  doc.Connect(pose_id, 0);
  if (!WriteFile(PathStr(fs::u8path(out_dir) / fs::u8path(name + ".fbx")), doc.Write())) {
    rep.warnings.push_back("could not write the FBX");
    return false;
  }
  return true;
}

bool ReadFbxSkeleton(const std::string& path, std::vector<FbxBone>& out, std::string& err) {
  ufbx_load_opts lo = {};
  lo.target_axes = ufbx_axes_right_handed_y_up;
  lo.target_unit_meters = 0.1f;  // (game units: 10 cm)
  ufbx_error ue;
  ufbx_scene* scene = ufbx_load_file(path.c_str(), &lo, &ue);
  if (!scene) return err = "ufbx: " + std::string(ue.description.data, ue.description.length), false;
  std::set<const ufbx_node*> skinned;
  for (size_t i = 0; i < scene->skin_clusters.count; ++i)
    if (scene->skin_clusters.data[i]->bone_node) skinned.insert(scene->skin_clusters.data[i]->bone_node);
  std::map<const ufbx_node*, int> index;
  for (size_t i = 0; i < scene->nodes.count; ++i) {
    const ufbx_node* n = scene->nodes.data[i];
    if (!n->bone && !skinned.count(n)) continue;
    index[n] = int(out.size());
    FbxBone b;
    b.name.assign(n->name.data, n->name.length);
    b.pos[0] = float(n->node_to_world.m03), b.pos[1] = float(n->node_to_world.m13), b.pos[2] = float(n->node_to_world.m23);
    b.skinned = skinned.count(n) != 0;
    out.push_back(b);
  }
  for (size_t i = 0; i < scene->nodes.count; ++i) {
    const ufbx_node* n = scene->nodes.data[i];
    const auto it = index.find(n);
    if (it == index.end()) continue;
    for (const ufbx_node* p = n->parent; p; p = p->parent)
      if (const auto pi = index.find(p); pi != index.end()) {
        out[size_t(it->second)].parent = pi->second;
        break;
      }
  }
  ufbx_free_scene(scene);
  if (out.empty()) return err = "the FBX has no skeleton (no bones, no skin)", false;
  return true;
}

std::vector<std::string> CheckCharacterFbx(const Bytes& pac, int attire, const std::string& fbx_path) {
  std::vector<std::string> out;
  Character ch;
  std::string err;
  if (!ReadCharacter(pac, attire, ch, err)) return {"FAILED: " + err};
  ufbx_load_opts lo = {};  // (as written: the file's own axes and units)
  ufbx_error ue;
  ufbx_scene* scene = ufbx_load_file(fbx_path.c_str(), &lo, &ue);
  if (!scene) return {"FAILED: ufbx: " + std::string(ue.description.data, ue.description.length)};
  // bones
  std::map<std::string, const ufbx_node*> nodes;
  for (size_t i = 0; i < scene->nodes.count; ++i) nodes[std::string(scene->nodes.data[i]->name.data)] = scene->nodes.data[i];
  int found = 0;
  double worst = 0;
  std::string worst_bone;
  for (const Bone& b : ch.bones) {
    const auto it = nodes.find(b.name);
    if (it == nodes.end()) continue;
    ++found;
    const M want = Turn(b.world);
    const ufbx_matrix& m = it->second->node_to_world;  // (the rest pose: the Lcl transforms)
    const double got[3][4] = {{m.m00, m.m01, m.m02, m.m03}, {m.m10, m.m11, m.m12, m.m13}, {m.m20, m.m21, m.m22, m.m23}};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c)
        if (std::fabs(got[r][c] - want.r[r][c]) > worst) worst = std::fabs(got[r][c] - want.r[r][c]), worst_bone = b.name;
      if (std::fabs(got[r][3] - want.t[r]) * 0.1 > worst) worst = std::fabs(got[r][3] - want.t[r]) * 0.1, worst_bone = b.name;
    }
  }
  out.push_back("bones: " + std::to_string(found) + " of " + std::to_string(ch.bones.size()) + " found; rest pose largest difference " +
                std::to_string(worst) + " (rotation, or translation / 10) at " + worst_bone);
  // clusters' bind matrices against the rest pose (Blender takes the bind pose from them)
  double cworst = 0;
  for (size_t i = 0; i < scene->skin_clusters.count; ++i) {
    const ufbx_skin_cluster* c = scene->skin_clusters.data[i];
    if (!c->bone_node) continue;
    const ufbx_matrix& a = c->bind_to_world;
    const ufbx_matrix& b = c->bone_node->node_to_world;
    const double d[] = {a.m00 - b.m00, a.m11 - b.m11, a.m22 - b.m22, (a.m03 - b.m03) * 0.1, (a.m13 - b.m13) * 0.1, (a.m23 - b.m23) * 0.1};
    for (double x : d) cworst = std::max(cworst, std::fabs(x));
  }
  out.push_back("clusters: " + std::to_string(scene->skin_clusters.count) + "; bind against rest largest difference " + std::to_string(cworst));
  // vertices and weights
  size_t verts = 0, want_verts = 0;
  for (const Part& p : ch.parts) want_verts += p.pos.size();
  for (size_t i = 0; i < scene->meshes.count; ++i) verts += scene->meshes.data[i]->num_vertices;
  double wworst = 0;
  size_t unweighted = 0;
  for (size_t i = 0; i < scene->skin_deformers.count; ++i) {
    const ufbx_skin_deformer* s = scene->skin_deformers.data[i];
    for (size_t v = 0; v < s->vertices.count; ++v) {
      double sum = 0;
      for (uint32_t k = 0; k < s->vertices.data[v].num_weights; ++k) sum += s->weights.data[s->vertices.data[v].weight_begin + k].weight;
      if (s->vertices.data[v].num_weights == 0) ++unweighted;
      else wworst = std::max(wworst, std::fabs(sum - 1));
    }
  }
  out.push_back("vertices: " + std::to_string(verts) + " (the pac: " + std::to_string(want_verts) + "); weight sums off by at most " +
                std::to_string(wworst) + ", " + std::to_string(unweighted) + " vertices without weights");
  out.push_back("meshes: " + std::to_string(scene->meshes.count) + ", materials: " + std::to_string(scene->materials.count) +
                ", textures: " + std::to_string(scene->textures.count));
  ufbx_free_scene(scene);
  return out;
}

// ---------------------------------------------------------------- FBX import (F3): the build input

namespace {

// a general affine inverse (source bind matrices can carry scale)
M InvAffine(const M& a) {
  const double(*r)[3] = a.r;
  const double det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                     r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
  M o;
  if (std::fabs(det) < 1e-12) return o;
  const double k = 1.0 / det;
  o.r[0][0] = (r[1][1] * r[2][2] - r[1][2] * r[2][1]) * k, o.r[0][1] = (r[0][2] * r[2][1] - r[0][1] * r[2][2]) * k;
  o.r[0][2] = (r[0][1] * r[1][2] - r[0][2] * r[1][1]) * k, o.r[1][0] = (r[1][2] * r[2][0] - r[1][0] * r[2][2]) * k;
  o.r[1][1] = (r[0][0] * r[2][2] - r[0][2] * r[2][0]) * k, o.r[1][2] = (r[0][2] * r[1][0] - r[0][0] * r[1][2]) * k;
  o.r[2][0] = (r[1][0] * r[2][1] - r[1][1] * r[2][0]) * k, o.r[2][1] = (r[0][1] * r[2][0] - r[0][0] * r[2][1]) * k;
  o.r[2][2] = (r[0][0] * r[1][1] - r[0][1] * r[1][0]) * k;
  for (int i = 0; i < 3; ++i) o.t[i] = -(o.r[i][0] * a.t[0] + o.r[i][1] * a.t[1] + o.r[i][2] * a.t[2]);
  return o;
}
M FromUfbx(const ufbx_matrix& u) {
  M m;
  m.r[0][0] = u.m00, m.r[0][1] = u.m01, m.r[0][2] = u.m02, m.t[0] = u.m03;
  m.r[1][0] = u.m10, m.r[1][1] = u.m11, m.r[1][2] = u.m12, m.t[1] = u.m13;
  m.r[2][0] = u.m20, m.r[2][1] = u.m21, m.r[2][2] = u.m22, m.t[2] = u.m23;
  return m;
}
using V3 = std::array<double, 3>;
V3 Apply(const M& m, const V3& p) {
  return {m.r[0][0] * p[0] + m.r[0][1] * p[1] + m.r[0][2] * p[2] + m.t[0], m.r[1][0] * p[0] + m.r[1][1] * p[1] + m.r[1][2] * p[2] + m.t[1],
          m.r[2][0] * p[0] + m.r[2][1] * p[1] + m.r[2][2] * p[2] + m.t[2]};
}
V3 ApplyDir(const M& m, const V3& p) {
  return {m.r[0][0] * p[0] + m.r[0][1] * p[1] + m.r[0][2] * p[2], m.r[1][0] * p[0] + m.r[1][1] * p[1] + m.r[1][2] * p[2],
          m.r[2][0] * p[0] + m.r[2][1] * p[1] + m.r[2][2] * p[2]};
}
V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
V3 Norm(const V3& a) {
  const double l = Len(a);
  return l > 1e-12 ? V3{a[0] / l, a[1] / l, a[2] / l} : V3{0, 0, 0};
}
// the rotation (Rodrigues) about a unit axis
M AxisAngle(const V3& k, double a) {
  M m;
  const double c = std::cos(a), s = std::sin(a), t = 1 - c;
  m.r[0][0] = t * k[0] * k[0] + c, m.r[0][1] = t * k[0] * k[1] - s * k[2], m.r[0][2] = t * k[0] * k[2] + s * k[1];
  m.r[1][0] = t * k[0] * k[1] + s * k[2], m.r[1][1] = t * k[1] * k[1] + c, m.r[1][2] = t * k[1] * k[2] - s * k[0];
  m.r[2][0] = t * k[0] * k[2] - s * k[1], m.r[2][1] = t * k[1] * k[2] + s * k[0], m.r[2][2] = t * k[2] * k[2] + c;
  return m;
}
// the shortest rotation taking direction a onto b
M Swing(const V3& a0, const V3& b0) {
  const V3 a = Norm(a0), b = Norm(b0);
  V3 axis = Cross(a, b);
  const double s = Len(axis), c = Dot(a, b);
  if (s < 1e-9) {
    if (c > 0) return M();
    axis = Cross(a, std::fabs(a[0]) < 0.9 ? V3{1, 0, 0} : V3{0, 1, 0});  // (opposite: any perpendicular axis)
  }
  return AxisAngle(Norm(axis), std::atan2(s, c));
}

std::string LowerStr(std::string s) {
  for (auto& ch : s) ch = char(std::tolower(uint8_t(ch)));
  return s;
}

// the texture a material's colour / normal map comes from
const ufbx_texture* MaterialTexture(const ufbx_material* m, bool normal) {
  if (!m) return nullptr;
  if (normal) {
    if (m->pbr.normal_map.texture) return m->pbr.normal_map.texture;
    if (m->fbx.normal_map.texture) return m->fbx.normal_map.texture;
    return m->fbx.bump.texture;
  }
  if (m->pbr.base_color.texture) return m->pbr.base_color.texture;
  return m->fbx.diffuse_color.texture;
}

// A texture's picture: embedded, at its path, beside the FBX, or found by name in the folders around it
// (Unity / Blender projects keep them in a Textures folder next to the model's).
bool LoadFbxTexture(const ufbx_texture* t, const fs::path& fbx_dir, Image& out, std::string& where) {
  if (!t) return false;
  if (t->content.size && LoadImageMemory(static_cast<const uint8_t*>(t->content.data), t->content.size, out)) return where = "embedded", true;
  std::vector<std::string> names;
  for (const ufbx_string* s : {&t->absolute_filename, &t->filename, &t->relative_filename})
    if (s->length) names.emplace_back(s->data, s->length);
  for (const auto& n : names) {
    const fs::path p = fs::u8path(n);
    for (const fs::path& c : {p, fbx_dir / p, fbx_dir / p.filename(), fbx_dir / "textures" / p.filename()}) {
      std::error_code ec;
      if (fs::is_regular_file(c, ec) && LoadImageFile(PathStr(c), out)) return where = PathStr(c), true;
    }
  }
  if (names.empty()) return false;
  const std::string want = LowerStr(PathStr(fs::u8path(names[0]).filename()));
  for (const fs::path& root : {fbx_dir, fbx_dir.parent_path(), fbx_dir.parent_path().parent_path()}) {
    std::error_code ec;
    int seen = 0;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator() && seen < 20000; it.increment(ec), ++seen)
      if (it->is_regular_file(ec) && LowerStr(PathStr(it->path().filename())) == want && LoadImageFile(PathStr(it->path()), out))
        return where = PathStr(it->path()), true;
  }
  return false;
}

std::string JsonStr(const std::string& s) {
  std::string o = "\"";
  for (const char ch : s) {
    if (ch == '"' || ch == '\\') o += '\\', o += ch;
    else if (uint8_t(ch) < 0x20) o += ' ';
    else o += ch;
  }
  return o + "\"";
}
std::string Num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.5g", std::fabs(v) < 1e-9 ? 0.0 : v);
  return b;
}

// the 2011 pairs whose direction the source skeleton takes (bone -> the joint it points at), top down
const char* const kAimPairs[][2] = {
    {"koshi", "mune"},   {"mune", "kubi"},     {"kubi", "atama"},
    {"l_sakotsu", "l_ninoude"}, {"l_ninoude", "l_kote"}, {"l_kote", "l_te"}, {"l_te", "l_yubi20"},
    {"r_sakotsu", "r_ninoude"}, {"r_ninoude", "r_kote"}, {"r_kote", "r_te"}, {"r_te", "r_yubi20"},
    {"l_yubi00", "l_yubi01"}, {"l_yubi01", "l_yubi02"}, {"l_yubi10", "l_yubi11"}, {"l_yubi11", "l_yubi12"},
    {"l_yubi20", "l_yubi21"}, {"l_yubi21", "l_yubi22"}, {"l_yubi30", "l_yubi31"}, {"l_yubi31", "l_yubi32"},
    {"l_yubi40", "l_yubi41"}, {"l_yubi41", "l_yubi42"},
    {"r_yubi00", "r_yubi01"}, {"r_yubi01", "r_yubi02"}, {"r_yubi10", "r_yubi11"}, {"r_yubi11", "r_yubi12"},
    {"r_yubi20", "r_yubi21"}, {"r_yubi21", "r_yubi22"}, {"r_yubi30", "r_yubi31"}, {"r_yubi31", "r_yubi32"},
    {"r_yubi40", "r_yubi41"}, {"r_yubi41", "r_yubi42"},
    {"l_momo", "l_sune"}, {"l_sune", "l_ashi"}, {"l_ashi", "l_tsumasaki"},
    {"r_momo", "r_sune"}, {"r_sune", "r_ashi"}, {"r_ashi", "r_tsumasaki"},
};

struct OutMesh {
  std::string name, kind, texture_key;
  std::vector<V3> pos, nrm;
  std::vector<std::array<double, 2>> uv;
  std::vector<std::vector<std::pair<int, double>>> w;  // (2011 bone index in the output list, weight)
  std::vector<std::array<int, 3>> tris;
};

// Fewer vertices for the game's budget (192 pages a wrestler; stock bodies ~10,000 vertices): quadric error
// half-edge collapses - a vertex merges into a neighbour, so the kept ones keep their exact UVs, normals and
// weights. Seam vertices (one position, several UV / normal copies) and open edges stay put; a collapse that
// flips or squashes a triangle is refused. keep: the vertices to keep at most. Returns the new vertex count.
// A merge between vertices weighted to different bones moves the kept one's weights onto the gone one's place - a
// finger's tip pulled by the hand, a sleeve by the arm (user report: "stretched fingers"): such a merge costs more
// the more the weights differ (kCrossBone), so it comes after every same-bone one; *cross counts those done (the
// weights more than half apart).
struct Quadric {
  double a[10] = {};  // the symmetric 4x4 (upper triangle)
  void AddPlane(const V3& n, double d, double w) {
    const double p[4] = {n[0], n[1], n[2], d};
    int k = 0;
    for (int i = 0; i < 4; ++i)
      for (int j = i; j < 4; ++j) a[k++] += w * p[i] * p[j];
  }
  void Add(const Quadric& q) {
    for (int k = 0; k < 10; ++k) a[k] += q.a[k];
  }
  double Eval(const V3& v) const {
    const double x = v[0], y = v[1], z = v[2];
    return a[0] * x * x + 2 * a[1] * x * y + 2 * a[2] * x * z + 2 * a[3] * x + a[4] * y * y + 2 * a[5] * y * z + 2 * a[6] * y +
           a[7] * z * z + 2 * a[8] * z + a[9];
  }
};

int Decimate(OutMesh& m, int keep, std::vector<bool>* lock = nullptr, int* cross = nullptr) {
  const int nv = int(m.pos.size());
  if (keep >= nv || m.tris.empty()) return nv;
  // (test aid SVR2011_TEST_REDUCE_OLD=1: the cost without the bones - to compare)
  static const bool by_bones = std::getenv("SVR2011_TEST_REDUCE_OLD") == nullptr;
  constexpr double kCrossBone = 40.0;
  auto wdist = [&](int a, int b) {  // (0: the same weights, 2: no bone in common)
    if (size_t(std::max(a, b)) >= m.w.size()) return 0.0;
    std::map<int, double> d;
    for (const auto& [bn, w] : m.w[size_t(a)]) d[bn] += w;
    for (const auto& [bn, w] : m.w[size_t(b)]) d[bn] -= w;
    double s = 0;
    for (const auto& [bn, v] : d) s += std::fabs(v);
    return s;
  };
  // positions welded: a seam vertex's copies (UV / normal splits) are one position
  std::map<std::array<double, 3>, int> weld;
  std::vector<int> pid(static_cast<size_t>(nv));
  std::vector<std::vector<int>> members;
  for (int i = 0; i < nv; ++i) {
    auto [it, fresh] = weld.emplace(std::array<double, 3>{m.pos[size_t(i)][0], m.pos[size_t(i)][1], m.pos[size_t(i)][2]}, int(members.size()));
    if (fresh) members.emplace_back();
    pid[size_t(i)] = it->second;
    members[size_t(it->second)].push_back(i);
  }
  const int np = int(members.size());
  // (positions to keep: the face - made dear to remove, not locked: a head-heavy model's face held a third of its
  // vertices, and locked, the rest was cut to nothing - the legs gone)
  std::vector<bool> plock(static_cast<size_t>(np));
  if (lock)
    for (int i = 0; i < nv && i < int(lock->size()); ++i)
      if ((*lock)[size_t(i)]) plock[size_t(pid[size_t(i)])] = true;
  std::vector<std::vector<int>> vt(static_cast<size_t>(nv));  // vertex -> its triangles
  for (int t = 0; t < int(m.tris.size()); ++t)
    for (int c = 0; c < 3; ++c) vt[size_t(m.tris[size_t(t)][size_t(c)])].push_back(t);
  // feature edges (positions): open (one triangle) or a seam (the two sides use different copies);
  // a position on exactly two feature edges slides along them, on more (a corner) or one it stays
  auto key = [](int a, int b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); };
  std::map<std::pair<int, int>, std::set<std::pair<int, int>>> variants;
  std::map<std::pair<int, int>, int> uses;
  for (const auto& t : m.tris)
    for (int c = 0; c < 3; ++c) {
      const int a = t[size_t(c)], b = t[size_t((c + 1) % 3)];
      const auto k = key(pid[size_t(a)], pid[size_t(b)]);
      ++uses[k];
      variants[k].insert(pid[size_t(a)] < pid[size_t(b)] ? std::make_pair(a, b) : std::make_pair(b, a));
    }
  std::vector<std::set<int>> fadj(static_cast<size_t>(np));  // position -> its neighbours along feature edges
  for (const auto& [k, n] : uses)
    if (n == 1 || variants[k].size() > 1) fadj[size_t(k.first)].insert(k.second), fadj[size_t(k.second)].insert(k.first);
  auto is_feature = [&](int a, int b) { return fadj[size_t(a)].count(b) > 0; };
  std::vector<Quadric> q(static_cast<size_t>(np));
  for (const auto& t : m.tris) {
    const V3 &p0 = m.pos[size_t(t[0])], &p1 = m.pos[size_t(t[1])], &p2 = m.pos[size_t(t[2])];
    const V3 c = Cross(Sub(p1, p0), Sub(p2, p0));
    const double area = Len(c);
    if (area < 1e-12) continue;
    const V3 n = Norm(c);
    for (int k = 0; k < 3; ++k) q[size_t(pid[size_t(t[size_t(k)])])].AddPlane(n, -Dot(n, p0), area);
    for (int k = 0; k < 3; ++k) {  // a feature edge's own plane (through it, upright on the face): its line stays
      const int a = t[size_t(k)], b = t[size_t((k + 1) % 3)];
      if (!is_feature(pid[size_t(a)], pid[size_t(b)])) continue;
      const V3 e = Sub(m.pos[size_t(b)], m.pos[size_t(a)]);
      const V3 en = Norm(Cross(e, n));
      const double w = Dot(e, e) * 8;
      q[size_t(pid[size_t(a)])].AddPlane(en, -Dot(en, m.pos[size_t(a)]), w);
      q[size_t(pid[size_t(b)])].AddPlane(en, -Dot(en, m.pos[size_t(a)]), w);
    }
  }
  std::vector<bool> tri_dead(m.tris.size()), dead(static_cast<size_t>(nv)), pdead(static_cast<size_t>(np));
  std::vector<int> stamp(static_cast<size_t>(np));
  auto movable = [&](int pu) { return fadj[size_t(pu)].empty() || fadj[size_t(pu)].size() == 2; };
  struct Cand {
    double cost;
    int pu, pv, su;
  };
  auto worse = [](const Cand& a, const Cand& b) { return a.cost > b.cost; };
  std::vector<Cand> heap;
  auto push_from = [&](int pu) {
    if (pdead[size_t(pu)] || !movable(pu)) return;
    std::set<int> seen;
    for (int u : members[size_t(pu)])
      for (int t : vt[size_t(u)]) {
        if (tri_dead[size_t(t)]) continue;
        for (int x : m.tris[size_t(t)]) {
          const int pv = pid[size_t(x)];
          if (pv == pu || !seen.insert(pv).second) continue;
          if (!fadj[size_t(pu)].empty() && !is_feature(pu, pv)) continue;  // (a feature vertex slides along its line only)
          Quadric s = q[size_t(pu)];
          s.Add(q[size_t(pv)]);
          double cost = s.Eval(m.pos[size_t(x)]);
          if (by_bones)
            if (const double wd = wdist(members[size_t(pu)][0], x); wd > 0.05) cost = cost * (1 + kCrossBone * wd) + 1e-4 * wd;
          heap.push_back({plock[size_t(pu)] ? cost * 30 + 2e-4 : cost, pu, pv, stamp[size_t(pu)]});  // (the face: last)
          std::push_heap(heap.begin(), heap.end(), worse);
        }
      }
  };
  for (int p = 0; p < np; ++p) push_from(p);
  int live = nv;
  while (live > keep && !heap.empty()) {
    std::pop_heap(heap.begin(), heap.end(), worse);
    const Cand c = heap.back();
    heap.pop_back();
    if (pdead[size_t(c.pu)] || pdead[size_t(c.pv)] || stamp[size_t(c.pu)] != c.su) continue;
    // each copy of pu goes onto the copy of pv it shares a triangle with
    std::vector<std::pair<int, int>> pairs;
    bool ok = true;
    for (int u : members[size_t(c.pu)]) {
      int to = -1;
      for (int t : vt[size_t(u)]) {
        if (tri_dead[size_t(t)]) continue;
        for (int x : m.tris[size_t(t)])
          if (pid[size_t(x)] == c.pv) to = x;
      }
      if (to < 0) {
        ok = false;
        break;
      }
      pairs.push_back({u, to});
    }
    if (!ok || pairs.empty()) continue;
    // no triangle may flip or turn to a sliver
    for (const auto& [u, v] : pairs) {
      for (int t : vt[size_t(u)]) {
        if (tri_dead[size_t(t)]) continue;
        auto tr = m.tris[size_t(t)];
        if (pid[size_t(tr[0])] == c.pv || pid[size_t(tr[1])] == c.pv || pid[size_t(tr[2])] == c.pv) continue;
        const V3 before = Cross(Sub(m.pos[size_t(tr[1])], m.pos[size_t(tr[0])]), Sub(m.pos[size_t(tr[2])], m.pos[size_t(tr[0])]));
        for (auto& x : tr)
          if (x == u) x = v;
        const V3 after = Cross(Sub(m.pos[size_t(tr[1])], m.pos[size_t(tr[0])]), Sub(m.pos[size_t(tr[2])], m.pos[size_t(tr[0])]));
        if (Len(after) < 1e-10 || Dot(Norm(before), Norm(after)) < 0.3) ok = false;
      }
      if (!ok) break;
    }
    if (!ok) continue;
    if (cross && wdist(pairs[0].first, pairs[0].second) > 1.0) ++*cross;
    for (const auto& [u, v] : pairs) {
      for (int t : vt[size_t(u)]) {
        if (tri_dead[size_t(t)]) continue;
        auto& tr = m.tris[size_t(t)];
        if (pid[size_t(tr[0])] == c.pv || pid[size_t(tr[1])] == c.pv || pid[size_t(tr[2])] == c.pv) {
          tri_dead[size_t(t)] = true;
          continue;
        }
        for (auto& x : tr)
          if (x == u) x = v;
        vt[size_t(v)].push_back(t);
      }
      dead[size_t(u)] = true;
      --live;
    }
    // pu's feature edges are pv's now
    for (int other : fadj[size_t(c.pu)]) {
      fadj[size_t(other)].erase(c.pu);
      if (other != c.pv) fadj[size_t(other)].insert(c.pv), fadj[size_t(c.pv)].insert(other);
    }
    fadj[size_t(c.pu)].clear();
    fadj[size_t(c.pv)].erase(c.pu);
    pdead[size_t(c.pu)] = true;
    q[size_t(c.pv)].Add(q[size_t(c.pu)]);
    // the neighbours' candidates change
    std::set<int> around;
    for (int v : members[size_t(c.pv)])
      for (int t : vt[size_t(v)])
        if (!tri_dead[size_t(t)])
          for (int x : m.tris[size_t(t)]) around.insert(pid[size_t(x)]);
    for (int p : around) ++stamp[size_t(p)], push_from(p);
    if (heap.size() > size_t(nv) * 64) {
      heap.clear();
      for (int p = 0; p < np; ++p) push_from(p);
    }
  }
  // compact: the live, used vertices
  std::vector<bool> used(static_cast<size_t>(nv));
  for (size_t t = 0; t < m.tris.size(); ++t)
    if (!tri_dead[t])
      for (int x : m.tris[t]) used[size_t(x)] = true;
  OutMesh f;
  f.name = m.name, f.kind = m.kind, f.texture_key = m.texture_key;
  std::vector<int> remap(static_cast<size_t>(nv), -1);
  std::vector<bool> kept_lock;
  for (int i = 0; i < nv; ++i)
    if (used[size_t(i)] && !dead[size_t(i)]) {
      remap[size_t(i)] = int(f.pos.size()), f.pos.push_back(m.pos[size_t(i)]), f.nrm.push_back(m.nrm[size_t(i)]),
      f.uv.push_back(m.uv[size_t(i)]), f.w.push_back(m.w[size_t(i)]);
      if (lock) kept_lock.push_back(i < int(lock->size()) && (*lock)[size_t(i)]);
    }
  if (lock) *lock = std::move(kept_lock);  // (renumbered with the vertices: the next pass keeps them too)
  for (size_t t = 0; t < m.tris.size(); ++t) {
    if (tri_dead[t]) continue;
    const auto& tr = m.tris[t];
    if (remap[size_t(tr[0])] < 0 || remap[size_t(tr[1])] < 0 || remap[size_t(tr[2])] < 0) continue;
    f.tris.push_back({remap[size_t(tr[0])], remap[size_t(tr[1])], remap[size_t(tr[2])]});
  }
  m = std::move(f);
  return int(m.pos.size());
}

}  // namespace

namespace {
std::string GuessKind(const std::string& node_name, const std::string& mat_name) {
  const std::string l = LowerStr(node_name + " " + mat_name);
  return l.find("lash") != std::string::npos ? "lash"
         : l.find("hair") != std::string::npos ? "hair"
         : (l.find("eye") != std::string::npos && l.find("brow") == std::string::npos) ? "eye?"
         : "skin";  // ("eye?": an eye only when it's small)
}
}  // namespace

bool FbxListMeshes(const std::string& fbx_path, std::vector<FbxMeshInfo>& out, std::string& err) {
  ufbx_load_opts lo = {};
  lo.target_axes = ufbx_axes_right_handed_y_up;
  lo.target_unit_meters = 0.1f;
  lo.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
  ufbx_error ue;
  ufbx_scene* scene = ufbx_load_file(fbx_path.c_str(), &lo, &ue);
  if (!scene) return err = "ufbx: " + std::string(ue.description.data, ue.description.length), false;
  for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
    const ufbx_node* node = scene->nodes.data[ni];
    const ufbx_mesh* mesh = node->mesh;
    if (!mesh || mesh->num_faces == 0) continue;
    const size_t parts = std::max<size_t>(1, mesh->material_parts.count);
    for (size_t pi = 0; pi < parts; ++pi) {
      const ufbx_mesh_part* part = mesh->material_parts.count ? &mesh->material_parts.data[pi] : nullptr;
      if (part && part->num_triangles == 0) continue;
      const ufbx_material* mat = part && pi < mesh->materials.count ? mesh->materials.data[pi] : nullptr;
      FbxMeshInfo m;
      const std::string node_name(node->name.data, node->name.length);
      const std::string mat_name = mat ? std::string(mat->name.data, mat->name.length) : "";
      m.name = node_name + (mat_name.empty() ? "" : " / " + mat_name);
      m.kind = GuessKind(node_name, mat_name);
      if (m.kind == "eye?") {
        V3 lo3{1e30, 1e30, 1e30}, hi3{-1e30, -1e30, -1e30};
        const M gw = FromUfbx(node->geometry_to_world);
        for (size_t v = 0; v < mesh->vertices.count; ++v) {
          const V3 p = Apply(gw, V3{mesh->vertices.data[v].x, mesh->vertices.data[v].y, mesh->vertices.data[v].z});
          for (int k = 0; k < 3; ++k) lo3[k] = std::min(lo3[k], p[k]), hi3[k] = std::max(hi3[k], p[k]);
        }
        m.kind = Len(Sub(hi3, lo3)) < 0.8 ? "eye" : "skin";
      }
      m.triangles = int(part ? part->num_triangles : mesh->num_triangles);
      m.vertices = int(part ? part->num_faces * 3 : mesh->num_vertices);  // (before sharing: an upper bound)
      m.hidden = !node->visible;
      m.has_texture = MaterialTexture(mat, false) != nullptr;
      out.push_back(m);
    }
  }
  ufbx_free_scene(scene);
  return true;
}

bool FbxMeshPictures(const std::string& fbx_path, int size, std::map<std::string, Image>& out) {
  ufbx_load_opts lo = {};
  ufbx_error ue;
  ufbx_scene* scene = ufbx_load_file(fbx_path.c_str(), &lo, &ue);
  if (!scene) return false;
  const fs::path fbx_dir = fs::u8path(fbx_path).parent_path();
  std::map<const ufbx_texture*, Image> loaded;
  for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
    const ufbx_node* node = scene->nodes.data[ni];
    const ufbx_mesh* mesh = node->mesh;
    if (!mesh || mesh->num_faces == 0) continue;
    const size_t parts = std::max<size_t>(1, mesh->material_parts.count);
    for (size_t pi = 0; pi < parts; ++pi) {
      const ufbx_mesh_part* part = mesh->material_parts.count ? &mesh->material_parts.data[pi] : nullptr;
      if (part && part->num_triangles == 0) continue;
      const ufbx_material* mat = part && pi < mesh->materials.count ? mesh->materials.data[pi] : nullptr;
      const ufbx_texture* t = MaterialTexture(mat, false);
      if (!t) continue;
      auto it = loaded.find(t);
      if (it == loaded.end()) {
        Image img;
        std::string where;
        if (LoadFbxTexture(t, fbx_dir, img, where) && img.w > 0 && img.h > 0) {
          const double f = std::min(1.0, double(size) / std::max(img.w, img.h));
          img = Resize(img, std::max(1, int(img.w * f)), std::max(1, int(img.h * f)));
        }
        it = loaded.emplace(t, std::move(img)).first;
      }
      if (it->second.rgba.empty()) continue;
      const std::string node_name(node->name.data, node->name.length);
      const std::string mat_name = mat ? std::string(mat->name.data, mat->name.length) : "";
      out[node_name + (mat_name.empty() ? "" : " / " + mat_name)] = it->second;
    }
  }
  ufbx_free_scene(scene);
  return true;
}

bool FbxToBuildInput(const std::string& fbx_path, const std::vector<std::string>& targets, const Bytes& template_pac,
                     const std::string& template_path, const std::string& out_dir, FbxImportReport& rep,
                     const FbxImportOptions& opts) {
  auto fail = [&](const std::string& e) { return rep.warnings.insert(rep.warnings.begin(), e), false; };
  // the 2011 template skeleton (game axes, bind = rest)
  Character tpl;
  std::string err;
  if (!ReadCharacter(template_pac, 1, tpl, err)) return fail("template: " + err);
  std::map<std::string, int> tpl_by;
  for (size_t i = 0; i < tpl.bones.size(); ++i) tpl_by.emplace(tpl.bones[i].name, int(i));
  auto tpl_pos = [&](const std::string& n, V3& p) {
    const auto it = tpl_by.find(n);
    if (it == tpl_by.end()) return false;
    const M& w = tpl.bones[size_t(it->second)].world;
    p = {w.t[0], w.t[1], w.t[2]};
    return true;
  };

  ufbx_load_opts lo = {};
  lo.target_axes = ufbx_axes_right_handed_y_up;
  lo.target_unit_meters = 0.1f;
  lo.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
  ufbx_error ue;
  ufbx_scene* scene = ufbx_load_file(fbx_path.c_str(), &lo, &ue);
  if (!scene) return fail("ufbx: " + std::string(ue.description.data, ue.description.length));
  struct Free {
    ufbx_scene* s;
    ~Free() { ufbx_free_scene(s); }
  } free_scene{scene};

  // source bones: the same list (and order) as ReadFbxSkeleton
  std::set<const ufbx_node*> skinned;
  for (size_t i = 0; i < scene->skin_clusters.count; ++i)
    if (scene->skin_clusters.data[i]->bone_node) skinned.insert(scene->skin_clusters.data[i]->bone_node);
  std::vector<const ufbx_node*> nodes;
  std::map<const ufbx_node*, int> index;
  for (size_t i = 0; i < scene->nodes.count; ++i) {
    const ufbx_node* n = scene->nodes.data[i];
    if (!n->bone && !skinned.count(n)) continue;
    index[n] = int(nodes.size());
    nodes.push_back(n);
  }
  const size_t nb = nodes.size();
  if (nb == 0) return fail("the FBX has no skeleton");
  if (targets.size() != nb) return fail("the bone mapping doesn't match the FBX (" + std::to_string(targets.size()) + " for " + std::to_string(nb) + " bones)");
  std::vector<int> parent(nb, -1);
  for (size_t i = 0; i < nb; ++i)
    for (const ufbx_node* p = nodes[i]->parent; p; p = p->parent)
      if (const auto it = index.find(p); it != index.end()) {
        parent[i] = it->second;
        break;
      }
  // bind world matrices: a skin cluster's bind, else the parent's bind times the node's local
  std::vector<M> bind(nb);
  std::vector<bool> has_bind(nb);
  for (size_t i = 0; i < scene->skin_clusters.count; ++i) {
    const ufbx_skin_cluster* c = scene->skin_clusters.data[i];
    if (const auto it = index.find(c->bone_node); it != index.end() && !has_bind[size_t(it->second)])
      bind[size_t(it->second)] = FromUfbx(c->bind_to_world), has_bind[size_t(it->second)] = true;
  }
  std::function<void(size_t)> fill = [&](size_t i) {
    if (has_bind[i]) return;
    has_bind[i] = true;  // (no cycles)
    if (parent[i] >= 0) fill(size_t(parent[i])), bind[i] = Mul(bind[size_t(parent[i])], FromUfbx(nodes[i]->node_to_parent));
    else bind[i] = FromUfbx(nodes[i]->node_to_world);
  };
  for (size_t i = 0; i < nb; ++i) fill(i);
  auto origin = [](const M& m) { return V3{m.t[0], m.t[1], m.t[2]}; };

  // each 2011 bone's source bone (the topmost one mapped to it)
  std::vector<int> depth(nb);
  for (size_t i = 0; i < nb; ++i)
    for (int p = parent[i]; p >= 0 && depth[i] <= int(nb); p = parent[size_t(p)]) ++depth[i];
  std::map<std::string, int> src_of;
  for (size_t i = 0; i < nb; ++i)
    if (!targets[i].empty()) {
      auto [it, fresh] = src_of.emplace(targets[i], int(i));
      // (two source bones on one 2011 bone - Eye_L and LeftEye: the one holding weights, else the topmost)
      const bool w_new = skinned.count(nodes[i]) > 0, w_old = skinned.count(nodes[size_t(it->second)]) > 0;
      if (!fresh && (w_new != w_old ? w_new : depth[i] < depth[size_t(it->second)])) it->second = int(i);
    }
  for (const char* need : {"root", "l_ninoude", "r_ninoude", "atama", "l_ashi", "r_ashi"})
    if (!src_of.count(need)) return fail(std::string("the mapping has no ") + need + " (the Skeleton step)");

  // game axes: C = diag(1, -1, -1) (y up -> -Y up), then a turn about the up axis putting the character's left on +X
  M axes;
  axes.r[1][1] = axes.r[2][2] = -1;
  {
    const V3 lat = ApplyDir(axes, Sub(origin(bind[size_t(src_of["l_ninoude"])]), origin(bind[size_t(src_of["r_ninoude"])])));
    const double l = std::hypot(lat[0], lat[2]);
    if (l > 1e-6) {
      const double c = lat[0] / l, s = lat[2] / l;
      M yaw;
      yaw.r[0][0] = c, yaw.r[0][2] = s, yaw.r[2][0] = -s, yaw.r[2][2] = c;
      axes = Mul(yaw, axes);
    }
  }
  const M to_src = Inv(axes);  // (orthonormal)

  // the pose: each pair's source bone turned (with everything under it) to the 2011 bone's direction
  std::vector<M> posed = bind;
  std::vector<std::vector<int>> kids(nb);
  for (size_t i = 0; i < nb; ++i)
    if (parent[i] >= 0) kids[size_t(parent[i])].push_back(int(i));
  std::function<void(int, const M&)> turn = [&](int i, const M& rot) {
    posed[size_t(i)] = Mul(rot, posed[size_t(i)]);
    for (int k : kids[size_t(i)]) turn(k, rot);
  };
  // a hand's roll: the knuckle line (index -> pinky) as the template's, about the hand's axis (before its fingers aim)
  auto roll = [&](char side) {
    const std::string s(1, side);
    const auto te = src_of.find(s + "_te"), mid = src_of.find(s + "_yubi20"), idx = src_of.find(s + "_yubi10"), pk = src_of.find(s + "_yubi40");
    V3 tte, tmid, tidx, tpk;
    if (te == src_of.end() || mid == src_of.end() || idx == src_of.end() || pk == src_of.end() || !tpl_pos(s + "_te", tte) ||
        !tpl_pos(s + "_yubi20", tmid) || !tpl_pos(s + "_yubi10", tidx) || !tpl_pos(s + "_yubi40", tpk))
      return;
    const V3 p_te = origin(posed[size_t(te->second)]);
    const V3 axis = Norm(Sub(origin(posed[size_t(mid->second)]), p_te));
    auto flat = [&](V3 v) {
      const double d = Dot(v, axis);
      return Norm(V3{v[0] - d * axis[0], v[1] - d * axis[1], v[2] - d * axis[2]});
    };
    const V3 have = flat(Sub(origin(posed[size_t(pk->second)]), origin(posed[size_t(idx->second)])));
    const V3 want = flat(ApplyDir(to_src, Sub(tpk, tidx)));
    if (Len(have) < 0.5 || Len(want) < 0.5) return;
    const double ang = std::atan2(Dot(Cross(have, want), axis), Dot(have, want));
    M rot = AxisAngle(axis, ang);
    const V3 r_p = ApplyDir(rot, p_te);
    for (int k = 0; k < 3; ++k) rot.t[k] = p_te[k] - r_p[k];
    turn(te->second, rot);
  };
  int aimed = 0;
  const auto head_of = src_of.find("atama");
  auto holds_head = [&](int i) {
    if (head_of == src_of.end()) return false;
    for (int k = head_of->second; k >= 0; k = parent[size_t(k)])
      if (k == i) return true;
    return false;
  };
  M head_turned;  // (what the aims above the head did to it)
  for (const auto& pr : kAimPairs) {
    const auto a = src_of.find(pr[0]), b = src_of.find(pr[1]);
    V3 ta, tb;
    if (a == src_of.end() || b == src_of.end() || !tpl_pos(pr[0], ta) || !tpl_pos(pr[1], tb)) continue;
    const V3 pa = origin(posed[size_t(a->second)]), pb = origin(posed[size_t(b->second)]);
    if (Len(Sub(pb, pa)) < 1e-6) continue;
    const M sw = Swing(Sub(pb, pa), ApplyDir(to_src, Sub(tb, ta)));
    M rot = sw;
    const V3 r_pa = ApplyDir(sw, pa);
    for (int k = 0; k < 3; ++k) rot.t[k] = pa[k] - r_pa[k];  // (about the bone's own joint)
    turn(a->second, rot);
    if (holds_head(a->second)) head_turned = Mul(rot, head_turned);
    ++aimed;
    if (std::string(pr[1]).ends_with("_yubi20")) roll(pr[0][0]);
  }
  rep.aimed = aimed;
  // The head has no bone to aim at, so it took every turn of the spine and the neck: a model whose neck or spine stands
  // straighter than the template's (report ac68af) looked at the floor. It keeps the way it faces in the FBX: turned
  // back about its own joint. (The neck's vertices blend between the two, as a neck does.)
  if (head_of != src_of.end()) {
    const V3 p = origin(posed[size_t(head_of->second)]);
    M back = Inv(head_turned);
    const V3 r_p = ApplyDir(back, p);
    for (int k = 0; k < 3; ++k) back.t[k] = p[k] - r_p[k];
    turn(head_of->second, back);
  }

  // the output frame: game axes, the hips at 0; scaled (below, once the meshes are posed) so the soles sit as far
  // below the hips as the template's (the game's motions hold the hips at its height: the feet then meet the floor)
  const V3 hips = origin(posed[size_t(src_of["root"])]);
  auto game = [&](const V3& p) { return ApplyDir(axes, Sub(p, hips)); };
  double scale = 1;
  if (game(origin(posed[size_t(src_of["l_ashi"])]))[1] < 1 || game(origin(posed[size_t(src_of["r_ashi"])]))[1] < 1)
    rep.warnings.push_back("the ankles aren't below the hips: the model is upside down or the legs are mapped wrong");
  auto out_pos = [&](const V3& p) {
    const V3 g = game(p);
    return V3{g[0] * scale, g[1] * scale, g[2] * scale};
  };
  {
    const auto toe = src_of.find("l_tsumasaki");
    if (toe != src_of.end() && game(origin(posed[size_t(toe->second)]))[2] > game(origin(posed[size_t(src_of["l_ashi"])]))[2])
      rep.warnings.push_back("the toes point backwards: the FBX may be mirrored (left and right swapped)");
  }

  // the 2011 bones the weights name: the mapped ones, a folded bone's nearest mapped parent, or root
  std::vector<std::string> wbone(nb);
  for (size_t i = 0; i < nb; ++i) {
    for (int p = int(i), n = 0; p >= 0 && n <= int(nb); p = parent[size_t(p)], ++n)
      if (!targets[size_t(p)].empty()) {
        wbone[i] = targets[size_t(p)];
        break;
      }
    if (wbone[i].empty()) wbone[i] = "root";
  }
  // Moving hair and cloth (svr11_build's "hair_chains": the game's kami_b spring chains, docs/HAIR_PHYSICS_PLAN.md
  // B; FindFbxChains): hair first, then cloth - each cloth chain hangs from its nearest mapped bone (the chain's
  // root parent: the solver reads it from the node table) and gets a tip past its last bone, so 3 bones move. The
  // weights go on the builder's names - joint k of chain i "kami_b<i>" (k 0) / "kami_b<i>_<k-1>"; a hair strand's tip
  // on the joint before (the game never moves a tip).
  std::vector<std::vector<int>> strands;
  std::vector<std::string> strand_parent;  // ("" hair: atama)
  {
    std::vector<FbxBone> fb(nb);
    for (size_t i = 0; i < nb; ++i) fb[i].name.assign(nodes[i]->name.data, nodes[i]->name.length), fb[i].parent = parent[i];
    const FbxChains ch = FindFbxChains(fb, targets, opts.hair, opts.cloth);
    for (const auto& h : ch.hair) strands.push_back(h), strand_parent.push_back("");
    for (const auto& c : ch.cloth) strands.push_back(c), strand_parent.push_back(c.front() >= 0 && parent[size_t(c.front())] >= 0 ? wbone[size_t(parent[size_t(c.front())])] : "koshi");
    if (ch.over) rep.warnings.push_back(std::to_string(ch.over) + " moving hair / cloth chain(s) past the 16 the game moves: they ride on their bone");
    if (ch.short_hair) rep.warnings.push_back(std::to_string(ch.short_hair) + " hair strand(s) with under 4 bones ride on the head (the game's chains need 4)");
    if (ch.short_cloth) rep.warnings.push_back(std::to_string(ch.short_cloth) + " cloth chain(s) with under 3 bones ride on their bone");
    if (ch.secondary) rep.mesh_lines.push_back("hair: " + std::to_string(ch.secondary) + " VRChat secondary hair chain(s) (J_Sec_*) left out - their weights on the head");
    for (size_t si = 0; si < strands.size(); ++si)
      for (size_t k = 0; k < strands[si].size(); ++k) {
        const bool cloth_chain = !strand_parent[si].empty();
        const size_t j = !cloth_chain && k + 1 == strands[si].size() ? k - 1 : k;  // (a hair tip's weights on the joint before)
        char n[32];
        if (j == 0) std::snprintf(n, sizeof n, "kami_b%02d", int(si));
        else std::snprintf(n, sizeof n, "kami_b%02d_%02d", int(si), int(j - 1));
        wbone[size_t(strands[si][k])] = n;
      }
  }
  std::vector<std::string> out_bones;
  std::map<std::string, int> out_index;
  auto bone_id = [&](const std::string& n) {
    auto [it, fresh] = out_index.emplace(n, int(out_bones.size()));
    if (fresh) out_bones.push_back(n);
    return it->second;
  };

  // meshes: one per mesh node and material, posed by their weights (linear blend from the bind)
  std::vector<M> skin(nb);
  for (size_t i = 0; i < nb; ++i) skin[i] = Mul(posed[i], InvAffine(bind[i]));
  const fs::path fbx_dir = fs::u8path(fbx_path).parent_path();
  std::vector<OutMesh> meshes;
  std::map<std::string, const ufbx_material*> tex_material;  // texture key -> a material using it
  std::vector<uint32_t> tri(64);
  for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
    const ufbx_node* node = scene->nodes.data[ni];
    const ufbx_mesh* mesh = node->mesh;
    if (!mesh || mesh->num_faces == 0) continue;
    const ufbx_skin_deformer* sk = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
    // a mesh without a skin rides on its nearest bone parent (or root)
    int rigid = -1;
    if (!sk)
      for (const ufbx_node* p = node->parent; p && rigid < 0; p = p->parent)
        if (const auto it = index.find(p); it != index.end()) rigid = it->second;
    if (!sk && rigid < 0) rigid = src_of["root"];
    const M geo_world = FromUfbx(node->geometry_to_world);
    const size_t parts = std::max<size_t>(1, mesh->material_parts.count);
    for (size_t pi = 0; pi < parts; ++pi) {
      const ufbx_mesh_part* part = mesh->material_parts.count ? &mesh->material_parts.data[pi] : nullptr;
      if (part && part->num_triangles == 0) continue;
      const ufbx_material* mat = part && pi < mesh->materials.count ? mesh->materials.data[pi] : nullptr;
      OutMesh om;
      const std::string node_name(node->name.data, node->name.length);
      const std::string mat_name = mat ? std::string(mat->name.data, mat->name.length) : "";
      om.name = node_name + (mat_name.empty() ? "" : " / " + mat_name);
      {
        const auto on = opts.include.find(om.name);
        if (on != opts.include.end() ? !on->second : !node->visible) {
          rep.skipped.push_back(om.name + (node->visible ? "" : " (hidden)"));
          continue;
        }
      }
      om.kind = GuessKind(node_name, mat_name);
      if (const auto k = opts.kind.find(om.name); k != opts.kind.end() && !k->second.empty()) om.kind = k->second;
      const ufbx_texture* ct = MaterialTexture(mat, false);
      if (const auto pic = opts.picture.find(om.name); pic != opts.picture.end() && !pic->second.empty()) {
        om.texture_key = "file|" + pic->second;
        ct = nullptr;
      } else
      om.texture_key = !ct ? "" : ct->filename.length ? LowerStr(std::string(ct->filename.data, ct->filename.length))
                                                    : "embedded|" + std::to_string(ct->element_id);
      if (!om.texture_key.empty() && om.texture_key.rfind("file|", 0) != 0) tex_material.emplace(om.texture_key, mat);
      std::map<std::array<uint32_t, 6>, int> verts;  // (position index, normal and uv values) -> output vertex
      auto vertex = [&](uint32_t ix) {
        const uint32_t vi = mesh->vertex_indices.data[ix];
        const uint32_t nidx = mesh->vertex_normal.exists ? mesh->vertex_normal.indices.data[ix] : 0;
        const uint32_t uidx = mesh->vertex_uv.exists ? mesh->vertex_uv.indices.data[ix] : 0;
        auto bits = [](double v) {
          const float f = float(v);
          uint32_t u;
          std::memcpy(&u, &f, 4);
          return u;
        };
        std::array<uint32_t, 6> key{vi, 0, 0, 0, 0, 0};
        if (mesh->vertex_normal.exists) {
          const ufbx_vec3 q = mesh->vertex_normal.values.data[nidx];
          key[1] = bits(q.x), key[2] = bits(q.y), key[3] = bits(q.z);
        }
        if (mesh->vertex_uv.exists) {
          const ufbx_vec2 q = mesh->vertex_uv.values.data[uidx];
          key[4] = bits(q.x), key[5] = bits(q.y);
        }
        if (const auto it = verts.find(key); it != verts.end()) return it->second;
        const ufbx_vec3 gp = mesh->vertices.data[vi];
        const V3 g{gp.x, gp.y, gp.z};
        V3 n{0, -1, 0};
        if (mesh->vertex_normal.exists) {
          const ufbx_vec3 gn = mesh->vertex_normal.values.data[nidx];
          n = {gn.x, gn.y, gn.z};
        }
        // the influences: (source bone, weight, bind world of the vertex for that bone)
        V3 p{0, 0, 0}, nn{0, 0, 0};
        std::map<int, double> w2011;
        double sum = 0;
        auto add = [&](int b, double w, const V3& wp, const V3& wn) {
          const V3 q = Apply(skin[size_t(b)], wp), qn = ApplyDir(skin[size_t(b)], wn);
          for (int k = 0; k < 3; ++k) p[k] += w * q[k], nn[k] += w * qn[k];
          w2011[bone_id(wbone[size_t(b)])] += w;
          sum += w;
        };
        if (sk && vi < sk->vertices.count) {
          const ufbx_skin_vertex sv = sk->vertices.data[vi];
          for (uint32_t k = 0; k < sv.num_weights; ++k) {
            const ufbx_skin_weight sw = sk->weights.data[sv.weight_begin + k];
            const ufbx_skin_cluster* c = sk->clusters.data[sw.cluster_index];
            const auto it = index.find(c->bone_node);
            if (it == index.end() || sw.weight <= 0) continue;
            const M g2w = Mul(bind[size_t(it->second)], FromUfbx(c->geometry_to_bone));
            add(it->second, sw.weight, Apply(g2w, g), ApplyDir(g2w, n));
          }
        }
        if (sum <= 0) {  // (no weights: rides on its node's bone)
          const int b = rigid >= 0 ? rigid : src_of["root"];
          add(b, 1.0, Apply(geo_world, g), ApplyDir(geo_world, n));
        }
        for (int k = 0; k < 3; ++k) p[k] /= sum;
        om.pos.push_back(out_pos(p));
        om.nrm.push_back(Norm(ApplyDir(axes, nn)));
        std::array<double, 2> uv{0, 0};
        if (mesh->vertex_uv.exists) {
          const ufbx_vec2 u = mesh->vertex_uv.values.data[uidx];
          uv = {u.x, 1.0 - u.y};  // (v down)
        }
        om.uv.push_back(uv);
        std::vector<std::pair<int, double>> wl;
        for (const auto& [b, w] : w2011) wl.push_back({b, w / sum});
        om.w.push_back(std::move(wl));
        const int id = int(om.pos.size()) - 1;
        verts.emplace(key, id);
        return id;
      };
      auto faces = [&](uint32_t f) {
        const ufbx_face face = mesh->faces.data[f];
        if (face.num_indices < 3) return;
        if (tri.size() < size_t(face.num_indices) * 3) tri.resize(size_t(face.num_indices) * 3);
        const uint32_t n = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
        for (uint32_t t = 0; t < n; ++t) {
          // the turn to game axes (det +1) keeps the winding
          om.tris.push_back({vertex(tri[t * 3]), vertex(tri[t * 3 + 1]), vertex(tri[t * 3 + 2])});
        }
      };
      if (part)
        for (size_t k = 0; k < part->face_indices.count; ++k) faces(part->face_indices.data[k]);
      else
        for (uint32_t f = 0; f < mesh->num_faces; ++f) faces(f);
      if (om.tris.empty()) continue;
      // The game's faces are turned over: its models keep their normals, and the winding with them, pointing in
      // (stock bodies: about a quarter point away from the body's centre line, an FBX's own about three quarters),
      // so a model as the FBX has it came out inside out - lit from behind, its fronts culled (user report 21 Oct).
      // The material's "flip" (Flip faces) keeps them as the file has them.
      if (const auto mo = opts.material.find(om.name); mo == opts.material.end() || mo->second.flip != 1) {
        for (auto& n : om.nrm) n = {-n[0], -n[1], -n[2]};
        for (auto& t : om.tris) std::swap(t[1], t[2]);
      }
      if (om.kind == "eye?") {  // (an eye fits in 8 cm; a whole model named EYE doesn't)
        V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
        for (const auto& p : om.pos)
          for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
        om.kind = Len(Sub(hi, lo)) < 0.8 ? "eye" : "skin";
      }
      if (om.pos.size() > 65535) rep.warnings.push_back(om.name + ": " + std::to_string(om.pos.size()) + " vertices (the game takes 65535 per mesh: the builder splits it)");
      rep.vertices += int(om.pos.size()), rep.triangles += int(om.tris.size());
      meshes.push_back(std::move(om));
    }
  }
  if (meshes.empty()) return fail("the FBX has no visible meshes");
  rep.meshes = int(meshes.size());
  {
    double tfloor = -1e30, sole = -1e30;
    for (const auto& p : tpl.parts)
      for (const auto& v : p.pos) tfloor = std::max(tfloor, v[1]);
    for (const auto& m : meshes)
      for (const auto& v : m.pos) sole = std::max(sole, v[1]);
    V3 troot{0, 0, 0};
    tpl_pos("root", troot);
    if (opts.gear_json) {  // (gear pieces have no feet: sized by the skeleton - the hips to the ankles, as the template's)
      V3 tl, tr;
      const double have = 0.5 * (game(origin(posed[size_t(src_of["l_ashi"])]))[1] + game(origin(posed[size_t(src_of["r_ashi"])]))[1]);
      if (tpl_pos("l_ashi", tl) && tpl_pos("r_ashi", tr) && have > 1) scale = 0.5 * ((tl[1] - troot[1]) + (tr[1] - troot[1])) / have;
    } else if (tfloor - troot[1] > 1 && sole > 1) {
      scale = (tfloor - troot[1]) / sole;
    }
    for (auto& m : meshes)
      for (auto& v : m.pos) v = {v[0] * scale, v[1] * scale, v[2] * scale};
  }
  rep.scale = scale;
  // Reduce detail (only when asked: max_vertices > 0 - it used to cut every model over the budget, a face too,
  // crumpled - user report 21 Oct): fewer vertices on the big meshes (eyes and lashes are left as they are);
  // keep_face: vertices that are mostly the head's (atama and the bones under it) stay
  std::vector<std::vector<bool>> locks(meshes.size());
  if (opts.max_vertices > 0 && opts.keep_face) {
    std::set<std::string> head{"atama"};
    for (bool grew = true; grew;) {
      grew = false;
      for (const auto& b : tpl.bones)
        if (b.parent >= 0 && head.count(tpl.bones[size_t(b.parent)].name) && head.insert(b.name).second) grew = true;
    }
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
      const OutMesh& m = meshes[mi];
      locks[mi].resize(m.pos.size());
      for (size_t k = 0; k < m.w.size(); ++k) {
        double hw = 0;
        for (const auto& [b, w] : m.w[k])
          if (b >= 0 && b < int(out_bones.size()) && (head.count(out_bones[size_t(b)]) || out_bones[size_t(b)].rfind("kami_b", 0) == 0)) hw += w;
        locks[mi][k] = hw >= 0.5;
      }
    }
  }
  {
    // (left as they are: eyes, lashes and every mesh under kSmallMesh vertices - a ring, a tooth row, a nail: cut,
    // they lost their shape for almost nothing; test aid SVR2011_TEST_REDUCE_OLD=1: only eyes and lashes, to compare)
    constexpr int kSmallMesh = 300;
    static const bool old_rule = std::getenv("SVR2011_TEST_REDUCE_OLD") != nullptr;
    auto kept = [&](const OutMesh& m) {
      return m.kind == "eye" || m.kind == "lash" || (!old_rule && int(m.pos.size()) < kSmallMesh);
    };
    int total = 0, small = 0;
    for (const auto& m : meshes) total += int(m.pos.size()), small += kept(m) ? int(m.pos.size()) : 0;
    if (opts.max_vertices > 0 && total > opts.max_vertices) {
      rep.reduced_from = total;
      std::vector<int> before(meshes.size()), cross(meshes.size());
      std::vector<bool> keep_mesh(meshes.size());
      for (size_t mi = 0; mi < meshes.size(); ++mi) before[mi] = int(meshes[mi].pos.size()), keep_mesh[mi] = kept(meshes[mi]);
      for (int pass = 0; pass < 3 && total > opts.max_vertices; ++pass) {
        const int big = total - small;
        const double ratio = std::max(0.05, double(opts.max_vertices - small) / std::max(1, big));
        total = small;
        for (size_t mi = 0; mi < meshes.size(); ++mi) {
          OutMesh& m = meshes[mi];
          if (keep_mesh[mi]) continue;
          total += Decimate(m, std::max(24, int(double(m.pos.size()) * ratio)), locks[mi].empty() ? nullptr : &locks[mi], &cross[mi]);
        }
      }
      for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const OutMesh& m = meshes[mi];
        std::string l = m.name + ": " + std::to_string(before[mi]) + " -> " + std::to_string(m.pos.size()) + " vertices";
        if (keep_mesh[mi]) l += " (kept: " + std::string(m.kind == "eye" || m.kind == "lash" ? m.kind : "small") + ")";
        if (cross[mi]) l += ", " + std::to_string(cross[mi]) + " merges across bones";
        rep.reduced_lines.push_back(l);
        rep.cross_merges += cross[mi];
      }
      rep.vertices = 0, rep.triangles = 0;
      for (const auto& m : meshes) rep.vertices += int(m.pos.size()), rep.triangles += int(m.tris.size());
      if (total > opts.max_vertices)
        rep.warnings.push_back("still " + std::to_string(total) + " vertices after reducing (" +
                               (opts.keep_face ? std::string("the face kept, ") : std::string()) +
                               "UV seams and open edges hold the rest): pick a lower level, " +
                               (opts.keep_face ? std::string("untick Keep the face, ") : std::string()) + "or leave out a part");
    }
  }

  // textures: every colour (and normal) map in one sheet (shelf packed, up to 2048), the UVs moved into it
  struct Sheet {
    Image color, normal;
    bool has_normal = false;
    int x = 0, y = 0, w = 0, h = 0;
  };
  std::map<std::string, Sheet> sheets;
  for (const auto& m : meshes)  // (pictures picked for a mesh)
    if (m.texture_key.rfind("file|", 0) == 0 && !sheets.count(m.texture_key)) {
      Sheet s;
      if (LoadImageFile(m.texture_key.substr(5), s.color)) ++rep.textures, sheets.emplace(m.texture_key, std::move(s));
      else rep.warnings.push_back("picture not readable: " + m.texture_key.substr(5));
    }
  for (const auto& [key, mat] : tex_material) {
    Sheet s;
    std::string where;
    if (!LoadFbxTexture(MaterialTexture(mat, false), fbx_dir, s.color, where)) {
      rep.warnings.push_back("texture not found: " + key + " (the mesh gets plain grey)");
      continue;
    }
    std::string nwhere;
    s.has_normal = LoadFbxTexture(MaterialTexture(mat, true), fbx_dir, s.normal, nwhere);
    ++rep.textures;
    sheets.emplace(key, std::move(s));
  }
  {
    std::set<std::string> bare;
    for (const auto& m : meshes)
      if (m.texture_key.empty()) bare.insert(m.name.substr(m.name.find(" / ") == std::string::npos ? 0 : m.name.find(" / ") + 3));
    std::string list;
    for (const auto& b : bare) list += (list.empty() ? "" : ", ") + b;
    if (!list.empty()) rep.warnings.push_back("no colour texture on " + list + " (plain grey: pick its picture)");
  }
  // A mesh's shine mask picture: its colour picture again (its own sheet cell) with the mask's brightness as the
  // alpha - where it shines (svr11_build "specular_alpha": "texture"; a skin part's goes into the normal map's alpha)
  std::set<std::string> own_alpha;  // (sheet cells whose alpha is the picture's: kept)
  for (auto& m : meshes) {
    const auto mt = opts.material.find(m.name);
    if (mt == opts.material.end()) continue;
    const FbxMaterial& f = mt->second;
    if (!f.shine_mask.empty()) {
      Image mask;
      if (!LoadImageFile(f.shine_mask, mask) || mask.w <= 0 || mask.h <= 0) {
        rep.warnings.push_back("shine picture not readable: " + f.shine_mask);
      } else {
        const std::string key = m.texture_key + "|shine|" + f.shine_mask;
        if (!sheets.count(key)) {
          Sheet s;
          if (const auto base = sheets.find(m.texture_key); base != sheets.end() && !m.texture_key.empty()) s = base->second;
          else s.color.w = mask.w, s.color.h = mask.h, s.color.rgba.assign(size_t(mask.w) * mask.h * 4, 160);
          const Image fit = Resize(mask, s.color.w, s.color.h);
          for (size_t q = 0; q + 3 < s.color.rgba.size(); q += 4)
            s.color.rgba[q + 3] = uint8_t((fit.rgba[q] * 77 + fit.rgba[q + 1] * 150 + fit.rgba[q + 2] * 29) >> 8);
          sheets.emplace(key, std::move(s));
        }
        m.texture_key = key;
        own_alpha.insert(key);
        continue;
      }
    }
    if (f.cutout == 1 || f.specular_alpha == "texture") own_alpha.insert(m.texture_key);  // (see-through: the alpha is it)
  }
  // (meshes with no picture share one small grey patch)
  bool any_plain = false;
  for (const auto& m : meshes) any_plain |= !sheets.count(m.texture_key);
  if (any_plain) {
    Sheet s;
    s.color.w = s.color.h = 32;
    s.color.rgba.assign(32 * 32 * 4, 160);
    sheets.emplace("", std::move(s));
    for (auto& m : meshes)
      if (!sheets.count(m.texture_key) || m.texture_key.empty()) m.texture_key = "";
  }
  int atlas = 0;
  {
    double area = 0;
    for (const auto& [k, s] : sheets) area += double(s.color.w) * s.color.h;
    double shrink = std::min(1.0, std::sqrt(2048.0 * 2048.0 * 0.85 / std::max(1.0, area)));
    for (int tries = 0; tries < 40 && !atlas; ++tries, shrink *= 0.92) {
      std::vector<std::pair<int, Sheet*>> order;
      for (auto& [k, s] : sheets) {
        s.w = std::max(8, int(s.color.w * shrink)), s.h = std::max(8, int(s.color.h * shrink));
        order.push_back({s.h, &s});
      }
      std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      for (int size : {256, 512, 1024, 2048}) {
        int x = 0, y = 0, row = 0;
        bool fits = true;
        for (auto& [h, s] : order) {
          const int pw = s->w + 8, ph = s->h + 8;  // (4 px of edge around each: no bleeding between them)
          if (x + pw > size) x = 0, y += row, row = 0;
          if (pw > size || y + ph > size) {
            fits = false;
            break;
          }
          s->x = x + 4, s->y = y + 4, x += pw, row = std::max(row, ph);
        }
        if (fits) {
          atlas = size;
          break;
        }
      }
    }
    if (!atlas) return fail("the textures don't fit one 2048 sheet");
  }
  bool any_normal = false;
  for (const auto& [k, s] : sheets) any_normal |= s.has_normal;
  Image color, normal;
  color.w = color.h = normal.w = normal.h = atlas;
  color.rgba.assign(size_t(atlas) * atlas * 4, 0);
  normal.rgba.assign(size_t(atlas) * atlas * 4, 0);
  for (size_t k = 0; k < normal.rgba.size(); k += 4) normal.rgba[k] = 128, normal.rgba[k + 1] = 128, normal.rgba[k + 2] = 255, normal.rgba[k + 3] = 48;
  // (hair and lash cards, see-through parts: the alpha is their cut-out; a shine picture's; else it's a low shine -
  // a see-through cloth part used to lose its alpha here, cut where it shouldn't be or not at all)
  std::set<std::string> keep_alpha = own_alpha;
  for (const auto& m : meshes)
    if (m.kind == "hair" || m.kind == "lash") keep_alpha.insert(m.texture_key);
  for (auto& [key, s] : sheets) {
    const Image c = Resize(s.color, s.w, s.h);
    const Image n = s.has_normal ? Resize(s.normal, s.w, s.h) : Image();
    for (int y = -4; y < s.h + 4; ++y)
      for (int x = -4; x < s.w + 4; ++x) {
        const int sx = std::clamp(x, 0, s.w - 1), sy = std::clamp(y, 0, s.h - 1);
        const size_t d = (size_t(s.y + y) * atlas + size_t(s.x + x)) * 4, src = (size_t(sy) * s.w + size_t(sx)) * 4;
        for (int ch = 0; ch < 4; ++ch) color.rgba[d + size_t(ch)] = c.rgba[src + size_t(ch)];
        if (!keep_alpha.count(key)) color.rgba[d + 3] = 48;  // (a low shine on skin and cloth)
        if (s.has_normal)
          for (int ch = 0; ch < 4; ++ch) normal.rgba[d + size_t(ch)] = n.rgba[src + size_t(ch)];  // (alpha: the skin's shine mask)
      }
  }
  int wrapped = 0;
  for (auto& m : meshes) {
    const Sheet& s = sheets[m.texture_key];
    for (auto& uv : m.uv) {
      double u = uv[0], v = uv[1];
      if (u < -0.001 || u > 1.001 || v < -0.001 || v > 1.001) ++wrapped, u -= std::floor(u), v -= std::floor(v);
      uv = {(s.x + u * s.w) / atlas, (s.y + v * s.h) / atlas};
    }
  }
  if (wrapped) rep.warnings.push_back(std::to_string(wrapped) + " vertices had UVs outside 0..1 (tiled textures): wrapped, check the look");
  const fs::path dir = fs::u8path(out_dir);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (!SavePng(PathStr(dir / "color.png"), color)) return fail("could not write " + PathStr(dir / "color.png"));
  if (any_normal && !SavePng(PathStr(dir / "normal.png"), normal)) return fail("could not write normal.png");
  rep.atlas = atlas;

  // joints: the 2011 bones at the model's own proportions (the mapped ones; twist bones along their segment)
  std::map<std::string, V3> joints;
  for (const auto& [t, s] : src_of)
    if (tpl_by.count(t)) joints[t] = out_pos(origin(posed[size_t(s)]));
  for (const auto& b : tpl.bones) {
    if (joints.count(b.name) || b.parent < 0) continue;
    const bool twist = b.name.size() > 2 && (b.name.ends_with("_x") || b.name.ends_with("_x2"));
    if (!twist) continue;
    const std::string& pn = tpl.bones[size_t(b.parent)].name;
    std::string next;  // the segment's end: ninoude -> kote, kote -> te, momo -> sune
    for (const auto& pr : kAimPairs)
      if (pn == pr[0]) next = pr[1];
    V3 tp, tn, tx;
    if (next.empty() || !joints.count(pn) || !joints.count(next) || !tpl_pos(pn, tp) || !tpl_pos(next, tn) || !tpl_pos(b.name, tx)) continue;
    const V3 seg = Sub(tn, tp);
    const double f = std::clamp(Dot(Sub(tx, tp), seg) / std::max(1e-9, Dot(seg, seg)), 0.0, 1.0);
    const V3 a = joints[pn], c = joints[next];
    joints[b.name] = {a[0] + f * (c[0] - a[0]), a[1] + f * (c[1] - a[1]), a[2] + f * (c[2] - a[2])};
  }
  // test: how far each aimed bone is from the template's direction (0 when the pose took)
  for (const auto& pr : kAimPairs) {
    V3 ta, tb;
    if (!joints.count(pr[0]) || !joints.count(pr[1]) || !tpl_pos(pr[0], ta) || !tpl_pos(pr[1], tb)) continue;
    const V3 d = Norm(Sub(joints[pr[1]], joints[pr[0]])), t = Norm(Sub(tb, ta));
    const double e = std::acos(std::clamp(Dot(d, t), -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
    if (e > rep.aim_error_deg) rep.aim_error_deg = e, rep.aim_worst = std::string(pr[0]) + " -> " + pr[1];
  }
  // the model's height (feet to the top, cm, before the scale) for the report
  double top = 1e30, bottom = -1e30;
  for (const auto& m : meshes)
    for (const auto& p : m.pos) top = std::min(top, p[1]), bottom = std::max(bottom, p[1]);
  rep.height_cm = (bottom - top) * 10.0 / scale;

  // the builder's input (tools/svr11_build.py)
  std::string hair_json;  // "hair_chains": each strand's joints, root to tip, in the game's space
  if (!strands.empty()) {
    hair_json = "\"hair_chains\": [";
    int cloth_n = 0;
    for (size_t si = 0; si < strands.size(); ++si) {
      const bool cloth_chain = !strand_parent[si].empty();
      cloth_n += cloth_chain;
      hair_json += std::string(si ? ", " : "") + (cloth_chain ? "{\"parent\": " + JsonStr(strand_parent[si]) + ", \"joints\": [" : "[");
      std::vector<V3> pts;
      for (size_t k = 0; k < strands[si].size(); ++k) pts.push_back(out_pos(origin(posed[size_t(strands[si][k])])));
      if (cloth_chain && pts.size() >= 2) {  // (a tip past the last bone, its length again: the last bone moves too)
        const V3& a = pts[pts.size() - 2];
        const V3& b = pts.back();
        pts.push_back({2 * b[0] - a[0], 2 * b[1] - a[1], 2 * b[2] - a[2]});
      }
      for (size_t k = 0; k < pts.size(); ++k) hair_json += std::string(k ? ", " : "") + "[" + Num(pts[k][0]) + ", " + Num(pts[k][1]) + ", " + Num(pts[k][2]) + "]";
      hair_json += cloth_chain ? "]}" : "]";
    }
    hair_json += "],\n ";
    rep.mesh_lines.push_back("moving: " + std::to_string(strands.size() - size_t(cloth_n)) + " hair strand(s), " + std::to_string(cloth_n) +
                             " cloth chain(s) move in the game (kami_b00..)");
  }
  const std::string textures = "{\"color\": \"color.png\"" + std::string(any_normal ? ", \"normal\": \"normal.png\"" : "") + "}";
  std::string j;
  if (opts.gear_json) {  // (gear pieces only: posed and sized onto the template's body; their weights on its bones)
    j = "{\"version\": 1, \"textures\": " + textures + ", \"extra_bones\": [],\n " + hair_json + "\"meshes\": [";
  } else {
    j = "{\"version\": 1, \"template\": " + JsonStr(template_path) + ",\n \"joints\": {";
    bool first = true;
    for (const auto& [n, p] : joints) {
      j += std::string(first ? "" : ", ") + JsonStr(n) + ": [" + Num(p[0]) + ", " + Num(p[1]) + ", " + Num(p[2]) + "]";
      first = false;
    }
    j += "},\n \"attires\": [{\"name\": \"\", \"textures\": " + textures + ",\n  " + hair_json + "\"meshes\": [";
  }
  for (size_t mi = 0; mi < meshes.size(); ++mi) {
    const OutMesh& m = meshes[mi];
    j += std::string(mi ? ",\n   " : "\n   ") + "{\"name\": " + JsonStr(m.name) + ", \"kind\": \"" + m.kind + "\",\n    \"bones\": [";
    for (size_t b = 0; b < out_bones.size(); ++b) j += std::string(b ? ", " : "") + JsonStr(out_bones[b]);
    j += "],\n    \"positions\": [";
    for (size_t k = 0; k < m.pos.size(); ++k) j += std::string(k ? "," : "") + "[" + Num(m.pos[k][0]) + "," + Num(m.pos[k][1]) + "," + Num(m.pos[k][2]) + "]";
    j += "],\n    \"normals\": [";
    for (size_t k = 0; k < m.nrm.size(); ++k) j += std::string(k ? "," : "") + "[" + Num(m.nrm[k][0]) + "," + Num(m.nrm[k][1]) + "," + Num(m.nrm[k][2]) + "]";
    j += "],\n    \"uvs\": [";
    for (size_t k = 0; k < m.uv.size(); ++k) j += std::string(k ? "," : "") + "[" + Num(m.uv[k][0]) + "," + Num(m.uv[k][1]) + "]";
    j += "],\n    \"weights\": [";
    for (size_t k = 0; k < m.w.size(); ++k) {
      j += std::string(k ? "," : "") + "[";
      for (size_t q = 0; q < m.w[k].size(); ++q) j += std::string(q ? "," : "") + "[" + std::to_string(m.w[k][q].first) + "," + Num(m.w[k][q].second) + "]";
      j += "]";
    }
    j += "],\n    \"triangles\": [";
    for (size_t k = 0; k < m.tris.size(); ++k)
      j += std::string(k ? "," : "") + "[" + std::to_string(m.tris[k][0]) + "," + std::to_string(m.tris[k][1]) + "," + std::to_string(m.tris[k][2]) + "]";
    j += "]";
    if (const auto mt = opts.material.find(m.name); mt != opts.material.end()) {  // (the material panel's choices)
      const FbxMaterial& f = mt->second;
      std::string mj;
      auto field = [&](const std::string& kv) { mj += (mj.empty() ? "" : ", ") + kv; };
      if (f.specular >= 0) field("\"specular\": " + Num(f.specular));
      if (!f.normal.empty()) field("\"normal\": " + JsonStr(f.normal));
      if (f.has_tint) field("\"tint\": [" + Num(f.tint[0]) + ", " + Num(f.tint[1]) + ", " + Num(f.tint[2]) + "]");
      if (f.cutout >= 0) field(std::string("\"cutout\": ") + (f.cutout ? "true" : "false"));
      if (f.double_sided >= 0) field(std::string("\"double_sided\": ") + (f.double_sided ? "true" : "false"));
      if (!f.specular_alpha.empty() || !f.shine_mask.empty())  // (a shine picture is the colour cell's alpha)
        field("\"specular_alpha\": " + JsonStr(f.shine_mask.empty() ? f.specular_alpha : "texture"));
      if (!mj.empty()) j += ",\n    \"material\": {" + mj + "}";
    }
    j += "}";
    rep.mesh_lines.push_back(m.name + " (" + m.kind + "): " + std::to_string(m.pos.size()) + " vertices, " + std::to_string(m.tris.size()) + " triangles");
  }
  j += opts.gear_json ? "]}\n" : "]}]}\n";
  if (!WriteFile(PathStr(dir / (opts.gear_json ? "gear.json" : "build.json")), Bytes(j.begin(), j.end()))) return fail("could not write the build input");
  rep.bones_used = int(out_bones.size());
  return true;
}

// ---------------------------------------------------------------- FBX import report (what's wrong with a model)

namespace {
std::string Thousands(long long v) {
  std::string s = std::to_string(v);
  for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
  return s;
}
bool PictureExtOk(const std::string& file) {
  std::string e = LowerStr(PathStr(fs::u8path(file).extension()));
  return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" || e == ".psd" || e.empty();
}
}  // namespace

std::vector<FbxIssue> FbxInspect(const std::string& fbx_path, const std::vector<std::string>& targets, const std::string& rig,
                                 const FbxImportOptions& opts) {
  std::vector<FbxIssue> out;
  auto add = [&](int level, std::string text, std::string fix) { out.push_back({level, std::move(text), std::move(fix)}); };
  ufbx_load_opts lo = {};
  lo.target_axes = ufbx_axes_right_handed_y_up;
  lo.target_unit_meters = 0.1f;
  lo.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
  ufbx_error ue;
  ufbx_scene* scene = ufbx_load_file(fbx_path.c_str(), &lo, &ue);
  if (!scene) {
    add(2, "The file can't be read as an FBX: " + std::string(ue.description.data, ue.description.length) + ".",
        "Export it again as FBX (binary, version 7.x) from your 3D program.");
    return out;
  }
  struct Free {
    ufbx_scene* s;
    ~Free() { ufbx_free_scene(s); }
  } free_scene{scene};

  // the rig
  std::set<std::string> have;
  for (const auto& t : targets)
    if (!t.empty()) have.insert(t);
  if (targets.empty()) {
    add(2, "The model has no skeleton (no bones and no skin).", "Rig it first (Mixamo can rig a model for free), then export the FBX with its armature.");
  } else {
    if (rig.rfind("unknown", 0) == 0) add(1, "The skeleton's bone names aren't from a rig the Mod Maker knows; it matched what it could by name.",
                              "Check Bone mapping: each main bone (hips, spine, head, arms, legs) should have an FBX bone.");
    struct Need {
      const char* what;
      std::vector<const char*> bones;
    };
    const Need needs[] = {{"hips", {"root"}},
                          {"spine", {"koshi", "mune"}},
                          {"neck and head", {"kubi", "atama"}},
                          {"arms", {"l_ninoude", "l_kote", "l_te", "r_ninoude", "r_kote", "r_te"}},
                          {"legs", {"l_momo", "l_sune", "l_ashi", "r_momo", "r_sune", "r_ashi"}}};
    for (const auto& n : needs) {
      std::string miss;
      for (const char* b : n.bones)
        if (!have.count(b)) miss += (miss.empty() ? "" : ", ") + std::string(b);
      if (!miss.empty()) add(2, std::string("The skeleton is missing bones for the ") + n.what + " (" + miss + ").",
                             "Bone mapping: pick the FBX bone for each one. Without them that part can't move.");
    }
    bool fingers = false, eyes = false;
    for (const auto& t : have) fingers |= t.find("yubi") != std::string::npos, eyes |= t == "l_eye" || t == "r_eye";
    if (!fingers) add(0, "No finger bones: the hands keep one pose (fists and grabs won't show).", "Fine for gloves or mitts; a rig with finger bones looks better.");
    if (!eyes) add(0, "No eye bones: the eyes won't move or blink.", "Fine if the eyes are painted on; otherwise add LeftEye / RightEye bones.");
  }

  // the meshes
  std::set<const ufbx_node*> bone_nodes;
  for (size_t i = 0; i < scene->skin_clusters.count; ++i)
    if (scene->skin_clusters.data[i]->bone_node) bone_nodes.insert(scene->skin_clusters.data[i]->bone_node);
  long long verts = 0, tris = 0, unweighted = 0, over4 = 0, degenerate = 0, nan = 0, no_uv_tris = 0;
  std::vector<std::string> rigid, hidden, no_uv;
  double lo_y = 1e30, hi_y = -1e30;
  std::map<const ufbx_material*, std::string> materials;  // (a material's first mesh, for the picture pick)
  for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
    const ufbx_node* node = scene->nodes.data[ni];
    const ufbx_mesh* mesh = node->mesh;
    if (!mesh || mesh->num_faces == 0) continue;
    const std::string name(node->name.data, node->name.length);
    bool on = node->visible;
    for (const auto& [k, v] : opts.include)
      if (k == name || k.rfind(name + " / ", 0) == 0) on = v;
    if (!on) {
      hidden.push_back(name);
      continue;
    }
    for (size_t m = 0; m < mesh->materials.count; ++m) {
      const ufbx_material* mat = mesh->materials.data[m];
      const std::string key = name + " / " + std::string(mat->name.data, mat->name.length);
      if (!opts.picture.count(key)) materials.emplace(mat, key);  // (a picture picked for it: fine)
    }
    // vertices as the game counts them: one per position + normal + UV value (as the build shares them)
    std::set<std::array<uint32_t, 6>> seen;
    auto bits = [](double v) {
      const float f = float(v);
      uint32_t u;
      std::memcpy(&u, &f, 4);
      return u;
    };
    for (size_t ix = 0; ix < mesh->num_indices; ++ix) {
      std::array<uint32_t, 6> k{mesh->vertex_indices.data[ix], 0, 0, 0, 0, 0};
      if (mesh->vertex_normal.exists) {
        const ufbx_vec3 q = mesh->vertex_normal.values.data[mesh->vertex_normal.indices.data[ix]];
        k[1] = bits(q.x), k[2] = bits(q.y), k[3] = bits(q.z);
      }
      if (mesh->vertex_uv.exists) {
        const ufbx_vec2 q = mesh->vertex_uv.values.data[mesh->vertex_uv.indices.data[ix]];
        k[4] = bits(q.x), k[5] = bits(q.y);
      }
      seen.insert(k);
    }
    verts += (long long)seen.size();
    tris += (long long)mesh->num_triangles;
    if (!mesh->vertex_uv.exists) no_uv.push_back(name), no_uv_tris += (long long)mesh->num_triangles;
    const M gw = FromUfbx(node->geometry_to_world);
    for (size_t v = 0; v < mesh->vertices.count; ++v) {
      const ufbx_vec3 p = mesh->vertices.data[v];
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        ++nan;
        continue;
      }
      const V3 w = Apply(gw, V3{p.x, p.y, p.z});
      lo_y = std::min(lo_y, w[1]), hi_y = std::max(hi_y, w[1]);
    }
    std::vector<uint32_t> tri(size_t(mesh->max_face_triangles) * 3 + 3);
    for (size_t f = 0; f < mesh->num_faces; ++f) {
      const uint32_t n = ufbx_triangulate_face(tri.data(), tri.size(), mesh, mesh->faces.data[f]);
      for (uint32_t t = 0; t < n; ++t) {
        const ufbx_vec3 a = mesh->vertices.data[mesh->vertex_indices.data[tri[t * 3]]], b = mesh->vertices.data[mesh->vertex_indices.data[tri[t * 3 + 1]]],
                        c = mesh->vertices.data[mesh->vertex_indices.data[tri[t * 3 + 2]]];
        if (Len(Cross(Sub(V3{b.x, b.y, b.z}, V3{a.x, a.y, a.z}), Sub(V3{c.x, c.y, c.z}, V3{a.x, a.y, a.z}))) < 1e-10) ++degenerate;
      }
    }
    const ufbx_skin_deformer* sk = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
    if (!sk) {
      rigid.push_back(name);
      continue;
    }
    for (size_t v = 0; v < sk->vertices.count; ++v) {
      uint32_t used = 0;
      for (uint32_t k = 0; k < sk->vertices.data[v].num_weights; ++k) used += sk->weights.data[sk->vertices.data[v].weight_begin + k].weight > 1e-4;
      if (used == 0) ++unweighted;
      else if (used > 4) ++over4;
    }
  }
  auto list = [](const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < 4; ++i) s += (i ? ", " : "") + v[i];
    return v.size() > 4 ? s + " and " + std::to_string(v.size() - 4) + " more" : s;
  };
  // polycount: the game gives a wrestler 192 memory pages - about 10,500 vertices. Nothing is cut unless asked
  // (Reduce detail: max_vertices).
  constexpr int kBudget = 10500;
  if (opts.max_vertices > 0 && verts > opts.max_vertices)
    add(0, Thousands(verts) + " vertices: Reduce detail cuts it to about " + Thousands(opts.max_vertices) + (opts.keep_face ? ", the face kept as it is." : "."),
        "Look it over in the 3D view after Make; for full control reduce it in Blender instead (Decimate modifier).");
  else if (verts > kBudget)
    add(2, Thousands(verts) + " vertices, " + Thousands(tris) + " triangles: over what the game holds for a wrestler (about " +
               Thousands(kBudget) + "). It won't build like this.",
        "Turn on Reduce detail below (pick how much), leave parts out in the Outfit step, or reduce it in Blender (Decimate modifier).");
  // skin
  if (!rigid.empty())
    add(1, "No skin on " + list(rigid) + ": " + (rigid.size() == 1 ? "it moves" : "they move") + " stiffly with the bone above, or the hips.",
        "Fine for hard things (a belt buckle, glasses); clothes and hair need skin weights (Blender: parent with automatic weights).");
  if (unweighted)
    add(1, Thousands(unweighted) + " vertices have no skin weights: they stay with the hips and stretch when the body moves.",
        "Blender: select them in Weight Paint and give them weights, or re-parent with automatic weights.");
  if (over4) add(0, Thousands(over4) + " vertices use more than 4 bones: the game keeps the 4 strongest.", "Usually invisible. Blender: Limit Total (4) to see the result first.");
  // mesh
  if (!no_uv.empty())
    add(2, "No UV map on " + list(no_uv) + ": the picture can't be placed on " + (no_uv.size() == 1 ? "it" : "them") + ".",
        "Unwrap it in your 3D program (Blender: Smart UV Project) and export again.");
  if (nan) add(2, Thousands(nan) + " broken points (not-a-number positions): the game would crash.", "Blender: Mesh > Clean Up, then export again.");
  if (degenerate) add(0, Thousands(degenerate) + " triangles have no area (dropped).", "Harmless. Blender: Mesh > Clean Up > Degenerate Dissolve.");
  if (!hidden.empty()) add(0, list(hidden) + (hidden.size() == 1 ? " is" : " are") + " hidden in the file and left out.", "Turn them on in the Outfit step if you want them.");
  // textures
  int missing = 0, big = 0, odd = 0;
  std::set<std::string> pictures;  // (distinct files: materials sharing one count once)
  std::string missing_names, odd_names;
  const fs::path dir = fs::u8path(fbx_path).parent_path();
  for (const auto& [m, key] : materials) {
    const std::string mname(m->name.data, m->name.length);
    const ufbx_texture* t = MaterialTexture(m, false);
    if (!t) {
      ++missing, missing_names += (missing_names.empty() ? "" : ", ") + mname;
      continue;
    }
    const std::string file(t->filename.data, t->filename.length);
    if (!pictures.insert(file.empty() ? "embedded" + std::to_string(t->element_id) : LowerStr(file)).second) continue;
    if (!file.empty() && !PictureExtOk(file)) {
      ++odd, odd_names += (odd_names.empty() ? "" : ", ") + PathStr(fs::u8path(file).filename());
      continue;
    }
    Image img;
    std::string where;
    if (!LoadFbxTexture(t, dir, img, where)) ++missing, missing_names += (missing_names.empty() ? "" : ", ") + mname + " (file not found)";
    else if (img.w > 2048 || img.h > 2048) ++big;
  }
  if (missing)
    add(1, "No picture for " + missing_names + ": " + (missing == 1 ? "it comes" : "they come") + " out plain grey.",
        "Materials step: a Colour picture for each, or export the FBX with its textures (Blender: Path Mode Copy + Embed Textures).");
  if (odd) add(1, "Pictures in a format the Mod Maker can't read: " + odd_names + ".", "Save them as PNG and pick them in the Materials step.");
  if (big) add(0, std::to_string(big) + " picture" + (big == 1 ? " is" : "s are") + " bigger than 2048: everything shares one 2048 sheet, so they get smaller.", "Nothing to do; fine detail softens.");
  if (pictures.size() > 8)
    add(0, std::to_string(pictures.size()) + " pictures share one 2048 sheet: each gets only a small part of it.", "Bake them into one atlas in your 3D program for sharper pictures.");
  // size
  if (hi_y > lo_y) {
    const double cm = (hi_y - lo_y) * 10;
    if (cm < 50 || cm > 300)
      add(1, "The model is " + std::to_string(int(cm + 0.5)) + " cm tall: its units look wrong.",
          "Export again in metres or centimetres (Blender: Apply Scale; Unity: Scale Factor 1). Make sizes it to the game anyway.");
  }
  return out;
}

}  // namespace svrfmt
