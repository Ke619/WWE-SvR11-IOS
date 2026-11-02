// The 3D character view (char_preview.h): the superstar page's preview and
// the Animations page.
//
// Model: ch.pac is EPK8; its EMD group has one PACH per attire and kind
// (entry "%06d%02d", attire*10 + kind, kind 2 = the model). The PACH holds
// JBOY models (child 0 the body with the whole skeleton, hair, eyelash,
// attachments; blood overlays named blood_* are skipped) and texture bundles.
// Every model's vertices are already in the body's model space (bind pose),
// so a vertex is skinned with pose(j) * inverse(bind(j)), its nodes matched
// to the body's by name. -Y is up; the character faces -Z.
//
// Idle: m.pac EPAC group MVMT, entry STAT, PACH child 3 is a YMBs bank;
// motion id 20000 x 10 y 0 is a one second loop. Any YMBs motion plays the
// same way (SetMotion); its track 1 (the victim) goes on the second
// character. A motion has 21 rotation
// channels (the bones in kBones, Euler z*y*x, replacing the node's own
// rotation), a root move, and for each arm and leg an IK target and a pole
// angle: the upper and lower limb bones carry no data and are solved here
// (two bones, the knee or elbow turned by the pole angle). Channels are
// Yamaha style ADPCM. Rotations are keyed 30 a second, moves twice that.
#define NOMINMAX
#include "char_preview.h"
#include "editor.h"
#include "hair_sim.h"

namespace mm {
void Log(const std::string& line);  // (app.h)
bool LoadPicture(const std::wstring& file, svrfmt::Image& out);
}

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <limits>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "imgui.h"
#include "svrfmt/jboy.h"
#include "svrfmt/pac.h"
#include "svrfmt/texture.h"

namespace char_preview {
namespace {

namespace fs = std::filesystem;
using namespace svrfmt;

// ---------------------------------------------------------------- math
// 3x4 affine matrices, column vectors: p' = R p + t.

struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Norm(V3 a) {
  const float l = Len(a);
  return l > 1e-9f ? a * (1.0f / l) : a;
}

struct Xf {
  float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  V3 t;
};
Xf Mul(const Xf& a, const Xf& b) {
  Xf o;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
  o.t = {a.r[0][0] * b.t.x + a.r[0][1] * b.t.y + a.r[0][2] * b.t.z + a.t.x,
         a.r[1][0] * b.t.x + a.r[1][1] * b.t.y + a.r[1][2] * b.t.z + a.t.y,
         a.r[2][0] * b.t.x + a.r[2][1] * b.t.y + a.r[2][2] * b.t.z + a.t.z};
  return o;
}
Xf Inverse(const Xf& a) {  // (rotation only: no scale in a skeleton)
  Xf o;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[j][i];
  o.t = {-(o.r[0][0] * a.t.x + o.r[0][1] * a.t.y + o.r[0][2] * a.t.z),
         -(o.r[1][0] * a.t.x + o.r[1][1] * a.t.y + o.r[1][2] * a.t.z),
         -(o.r[2][0] * a.t.x + o.r[2][1] * a.t.y + o.r[2][2] * a.t.z)};
  return o;
}
V3 Apply(const Xf& a, V3 p) {
  return {a.r[0][0] * p.x + a.r[0][1] * p.y + a.r[0][2] * p.z + a.t.x,
          a.r[1][0] * p.x + a.r[1][1] * p.y + a.r[1][2] * p.z + a.t.y,
          a.r[2][0] * p.x + a.r[2][1] * p.y + a.r[2][2] * p.z + a.t.z};
}
V3 Rotate(const Xf& a, V3 p) {
  return {a.r[0][0] * p.x + a.r[0][1] * p.y + a.r[0][2] * p.z, a.r[1][0] * p.x + a.r[1][1] * p.y + a.r[1][2] * p.z,
          a.r[2][0] * p.x + a.r[2][1] * p.y + a.r[2][2] * p.z};
}
// R = Rz * Ry * Rx (x applied first)
void Euler(const float e[3], float r[3][3]) {
  const float cx = std::cos(e[0]), sx = std::sin(e[0]), cy = std::cos(e[1]), sy = std::sin(e[1]);
  const float cz = std::cos(e[2]), sz = std::sin(e[2]);
  r[0][0] = cz * cy, r[0][1] = cz * sy * sx - sz * cx, r[0][2] = cz * sy * cx + sz * sx;
  r[1][0] = sz * cy, r[1][1] = sz * sy * sx + cz * cx, r[1][2] = sz * sy * cx - cz * sx;
  r[2][0] = -sy, r[2][1] = cy * sx, r[2][2] = cy * cx;
}
// A frame whose x points along `x_dir` and whose z is `d` (sign: arms +1,
// legs -1) with its x part removed.
void Aim(V3 x_dir, V3 d, float sign, float r[3][3]) {
  const V3 x = Norm(x_dir);
  const V3 z = Norm((d - x * Dot(d, x)) * sign), y = Cross(z, x);
  r[0][0] = x.x, r[1][0] = x.y, r[2][0] = x.z;
  r[0][1] = y.x, r[1][1] = y.y, r[2][1] = y.z;
  r[0][2] = z.x, r[1][2] = z.y, r[2][2] = z.z;
}
void MulR(const float a[3][3], const float b[3][3], float o[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}
void TransposeMul(const float a[3][3], const float b[3][3], float o[3][3]) {  // a^T b
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o[i][j] = a[0][i] * b[0][j] + a[1][i] * b[1][j] + a[2][i] * b[2][j];
}

// ---------------------------------------------------------------- the model

struct Influence {
  int node[4] = {0, 0, 0, 0};  // body skeleton
  float w[4] = {1, 0, 0, 0};
};
struct PMesh {
  std::vector<V3> pos, nrm;
  std::vector<std::array<float, 2>> uv;
  std::vector<Influence> inf;
  std::vector<uint32_t> idx;
  std::string tex;  // lower case
  bool cutout = false;  // (hair, lashes: alpha tested)
  bool blood = false;   // a blood overlay (blood_*): drawn only with SetBlood(true)
  // the material, as the game keeps it per mesh: shine g_fSpecularLev / g_iSpecularPow, the colour multiplier
  // g_f4MatDifCol, the render flags (+0x88: 0x100 see-through cut-out, 0x2 culled - else both sides), texNormal
  float spec = 0.5f, spec_pow = 50;
  float tint[3] = {1, 1, 1};
  uint32_t mflags = 0x6;
  std::string ntex;  // the normal map (lower case; "" none)
  bool skin_params = false;  // yBumpMapSS with a tiling map and g_fSweatSPA: culled, the game's skin shader (mm::SkinShine)
  std::vector<V3> tan;  // tangents (from the UVs) for the normal map
  uint32_t model_id = 0;  // its model's PACH child id and its index there (the material panel's key)
  int mesh_index = 0;
};
struct Bone {
  std::string name;
  float t[3], r[3];
  int parent;
};
struct Character {
  std::vector<Bone> bones;
  std::vector<PMesh> meshes;
  std::map<std::string, Bytes> textures;  // lower case name -> DDS
  size_t verts = 0;
  std::string bane60, bane61, bane50;  // the attire's hair chains / collision capsules / body jiggle (PACH 60 / 61 / 50 texts)
};

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

std::atomic<int> g_attire{1};  // the model's attire shown (1-4)
std::atomic<bool> g_gear{false};       // entrance gear shown (kind 4) when the attire has it
std::atomic<bool> g_gear_have{false};  // the shown attire has entrance gear

bool LoadEmd(const Bytes& emd, Character& ch, std::string& err);

bool LoadCharacter(const Bytes& pac, Character& ch, std::string& err, int attire = 1, bool want_gear = false) {
  if (pac.size() < 0x4000 || std::memcmp(pac.data(), "EPK8", 4)) {
    err = "not a character model pac (EPK8)";
    return false;
  }
  // the EMD group, the attire's model (name "......<attire-1>2"), else attire 1's, else any model
  Bytes emd, gear;
  bool attire1 = false, wanted = false;
  const char want = char('0' + std::clamp(attire, 1, 9) - 1);
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!Le32(&pac[p])) break;
    const bool is_emd = !std::memcmp(&pac[p], "EMD ", 4);
    const uint32_t cnt = Le16(&pac[p + 4]);
    p += 12;
    for (uint32_t i = 0; i < cnt && p + 16 <= 0x4000; ++i, p += 16) {
      const std::string name(reinterpret_cast<const char*>(&pac[p]), 8);
      const size_t off = 0x4000 + size_t(Le32(&pac[p + 8])) * 0x800, size = size_t(Le32(&pac[p + 12])) * 0x100;
      if (is_emd && name[0] != 0 && name[7] == '4' && name[6] == want && off + size <= pac.size()) gear.assign(pac.begin() + off, pac.begin() + off + size);
      if (!is_emd || name[0] == 0 || name[7] != '2' || off + size > pac.size()) continue;
      if (!wanted && (emd.empty() || name[6] == want || (name[6] == '0' && !attire1))) {
        emd.assign(pac.begin() + off, pac.begin() + off + size);
        attire1 = name[6] == '0';
        wanted = name[6] == want;
      }
    }
  }
  if (emd.empty()) {
    err = "no model in it (EMD)";
    return false;
  }
  if (want_gear) {
    g_gear_have = !gear.empty();
    if (!gear.empty() && g_gear) emd = std::move(gear);  // (the entrance model instead)
  }
  return LoadEmd(emd, ch, err);
}

// A model's EMD entry (or a prop's evtobj SOBJ entry: the same PACH of JBOYs and texture bundles).
bool LoadEmd(const Bytes& emd, Character& ch, std::string& err) {
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(emd), kids)) {
    err = "the model is not a PACH";
    return false;
  }
  std::vector<Model> models;
  std::vector<uint32_t> model_ids;  // (their PACH child ids)
  for (const auto& k : kids) {
    Bytes raw = Unpack(k.data);
    if (IsJboy(raw)) {
      Model m;
      if (JboyRead(raw, m)) {
        if (k.id == 0) models.insert(models.begin(), std::move(m)), model_ids.insert(model_ids.begin(), k.id);
        else models.push_back(std::move(m)), model_ids.push_back(k.id);
      }
    } else if (k.id == 60 || k.id == 61 || k.id == 50) {  // (secondary motion texts: hair_sim.h)
      (k.id == 60 ? ch.bane60 : k.id == 61 ? ch.bane61 : ch.bane50).assign(raw.begin(), raw.end());
    } else if (IsTextureBundle(raw)) {
      std::vector<BundleTexture> texs;
      if (BundleRead(raw, texs))
        for (auto& t : texs) ch.textures[Lower(t.name)] = std::move(t.data);
    }
  }
  if (models.empty() || models[0].nodes.empty()) {
    err = "no skeleton in the model";
    return false;
  }
  std::map<std::string, int> by_name;
  for (const auto& n : models[0].nodes) {
    Bone b;
    b.name = n.name;
    std::memcpy(b.t, n.t, 12);
    std::memcpy(b.r, n.r, 12);
    b.parent = n.parent >= 0 && n.parent < int(models[0].nodes.size()) ? n.parent : -1;
    by_name.emplace(b.name, int(ch.bones.size()));
    ch.bones.push_back(b);
  }
  // The other models' own bones (the hair's kami_* chains: PACH child 10000 "HAIR" has its own node table, each
  // strand root under atama, each segment under the one before - parent at +48) join the skeleton after the
  // body's, so they follow the head instead of falling back to root (hair left at the hips, stretched). A parent
  // the body has is found by name; one it lacks is added first. Rigid for now: the game's hair physics moves them.
  for (size_t mi = 1; mi < models.size(); ++mi) {
    const Model& m = models[mi];
    std::function<int(int, int)> add = [&](int local, int depth) -> int {
      if (local < 0 || local >= int(m.nodes.size()) || depth > 64) return -1;
      const auto& n = m.nodes[size_t(local)];
      if (auto it = by_name.find(n.name); it != by_name.end()) return it->second;
      Bone b;
      b.name = n.name;
      std::memcpy(b.t, n.t, 12);
      std::memcpy(b.r, n.r, 12);
      b.parent = n.parent != local ? add(n.parent, depth + 1) : -1;  // (before it: parents precede children)
      by_name.emplace(b.name, int(ch.bones.size()));
      ch.bones.push_back(b);
      return int(ch.bones.size()) - 1;
    };
    const size_t before = ch.bones.size();
    for (int i = 0; i < int(m.nodes.size()); ++i) add(i, 0);
    if (ch.bones.size() > before) {  // (the log: how many, and the first one's way up to the body)
      std::string chain;
      for (int b = int(before), hops = 0; b >= 0 && hops < 12; b = ch.bones[size_t(b)].parent, ++hops)
        chain += (chain.empty() ? "" : " < ") + ch.bones[size_t(b)].name;
      mm::Log("model: " + m.name + " adds " + std::to_string(ch.bones.size() - before) + " bones (" + chain + ")");
    }
  }
  for (size_t mi = 0; mi < models.size(); ++mi) {
    const Model& m = models[mi];
    // blood overlays: blood_* / Blood_face (the names' case varies) or PACH children 0x7530-0x7532
    const std::string lname = Lower(m.name);
    const bool blood = lname.rfind("blood", 0) == 0 || (model_ids[mi] >= 0x7530 && model_ids[mi] <= 0x7532);
    const bool name_cutout = lname.find("hair") != std::string::npos || lname.find("eyelash") != std::string::npos;
    for (size_t si_ = 0; si_ < m.meshes.size(); ++si_) {
      const Mesh& s = m.meshes[si_];
      PMesh pm;
      pm.model_id = model_ids[mi], pm.mesh_index = int(si_);
      const int slot = DiffuseSlot(s);
      pm.tex = slot >= 0 && slot < int(m.textures.size()) ? Lower(m.textures[slot]) : "color";
      // the material: the mesh's own parameters and flags (the game's), the cut-out from its flags
      if (s.raw.size() >= 0x8C) pm.mflags = Be32(&s.raw[0x88]);
      bool tiling = false;
      float sweat_spa = 0;
      for (const auto& p : s.params) {
        if (p.name == "texTilingMap" && p.type == 0x0f && p.value.size() >= 4) tiling = int32_t(Be32(p.value.data())) >= 0;
        if (p.name == "g_fSweatSPA" && p.type == 0x0a && p.value.size() >= 4) {
          const uint32_t u = Be32(p.value.data());
          std::memcpy(&sweat_spa, &u, 4);
        }
        if (p.name == "g_fSpecularLev" && p.type == 0x0a && p.value.size() >= 4) {
          const uint32_t u = Be32(p.value.data());
          std::memcpy(&pm.spec, &u, 4);
        } else if (p.name == "g_iSpecularPow" && p.type == 0x05 && p.value.size() >= 4) {
          pm.spec_pow = float(int32_t(Be32(p.value.data())));
        } else if (p.name == "g_f4MatDifCol" && p.type == 0x0d && p.value.size() >= 12) {
          for (int c = 0; c < 3; ++c) {
            const uint32_t u = Be32(&p.value[size_t(4 * c)]);
            std::memcpy(&pm.tint[c], &u, 4);
          }
        } else if (p.name == "texNormal" && p.type == 0x0f && p.value.size() >= 4) {
          const int slot = int(Be32(p.value.data()));
          if (slot >= 0 && slot < int(m.textures.size())) pm.ntex = Lower(m.textures[size_t(slot)]);
        }
      }
      pm.skin_params = mm::SkinShine(s.shader, 0x2, tiling, sweat_spa);  // (culled or not: the flags as drawn)
      pm.cutout = blood || (s.raw.size() >= 0x8C ? (pm.mflags & 0x100) != 0 : name_cutout);
      pm.blood = blood;
      const size_t vc = s.verts.size();
      pm.pos.resize(vc), pm.nrm.resize(vc), pm.uv.resize(vc), pm.inf.resize(vc);
      auto node_of = [&](int palette_slot) {
        if (palette_slot < 0 || palette_slot >= int(s.palette.size())) return 0;
        const int local = s.palette[palette_slot] - 1;  // (1-based)
        if (local < 0 || local >= int(m.nodes.size())) return 0;
        auto it = by_name.find(m.nodes[local].name);
        return it == by_name.end() ? 0 : it->second;
      };
      for (size_t k = 0; k < vc; ++k) {
        pm.pos[k] = {s.verts[k].pos[0], s.verts[k].pos[1], s.verts[k].pos[2]};
        pm.nrm[k] = {s.verts[k].normal[0], s.verts[k].normal[1], s.verts[k].normal[2]};
        pm.uv[k] = k < s.uvs.size() ? s.uvs[k] : std::array<float, 2>{0, 0};
        Influence& f = pm.inf[k];
        f = {};
        f.node[0] = node_of(0);
        int n = 0;
        float sum = 0;
        for (size_t j = 0; j < s.weights.size() && n < 4; ++j) {
          if (k >= s.weights[j].size() || s.weights[j][k].bones[0] == 255) continue;
          f.node[n] = node_of(s.weights[j][k].bones[0]);
          f.w[n] = s.weights[j][k].weight;
          sum += f.w[n++];
        }
        if (n == 0 || sum <= 0) f.w[0] = 1, f.w[1] = f.w[2] = f.w[3] = 0;
        else
          for (int j = 0; j < 4; ++j) f.w[j] = j < n ? f.w[j] / sum : 0;
      }
      for (const auto& st : s.strips)
        for (const auto& t : StripToTriangles(st.indices))
          if (t[0] < vc && t[1] < vc && t[2] < vc) pm.idx.insert(pm.idx.end(), {t[0], t[1], t[2]});
      if (pm.idx.empty()) continue;
      ch.verts += vc;
      if (!pm.ntex.empty()) {  // tangents along +U (per triangle, summed per vertex)
        pm.tan.assign(vc, V3{0, 0, 0});
        for (size_t t = 0; t + 2 < pm.idx.size(); t += 3) {
          const uint32_t a = pm.idx[t], b = pm.idx[t + 1], c = pm.idx[t + 2];
          const V3 e1 = pm.pos[b] - pm.pos[a], e2 = pm.pos[c] - pm.pos[a];
          const float u1 = pm.uv[b][0] - pm.uv[a][0], v1 = pm.uv[b][1] - pm.uv[a][1], u2 = pm.uv[c][0] - pm.uv[a][0],
                      v2 = pm.uv[c][1] - pm.uv[a][1];
          const float d = u1 * v2 - u2 * v1;
          if (std::fabs(d) < 1e-12f) continue;
          const V3 tg = (e1 * v2 - e2 * v1) * (1.0f / d);
          for (uint32_t k : {a, b, c}) pm.tan[k] = pm.tan[k] + tg;
        }
      }
      ch.meshes.push_back(std::move(pm));
    }
  }
  if (ch.meshes.empty()) {
    err = "the model has no meshes";
    return false;
  }
  // (problem reports - "a black shape": what the model holds, and the textures its meshes name that it lacks:
  // those draw black, in here and in the game)
  std::map<std::string, int> missing;
  for (const auto& pm : ch.meshes)
    if (!pm.blood && !ch.textures.count(pm.tex)) ++missing[pm.tex];
  std::string names, lacks;
  for (size_t mi = 0; mi < models.size() && mi < 8; ++mi) names += (names.empty() ? "" : ", ") + models[mi].name;
  for (const auto& [t, n] : missing) lacks += (lacks.empty() ? "" : ", ") + t + " (" + std::to_string(n) + " meshes)";
  mm::Log("model preview: " + std::to_string(models.size()) + " models (" + names + "), " + std::to_string(ch.meshes.size()) +
          " meshes, " + std::to_string(ch.verts) + " vertices, " + std::to_string(ch.textures.size()) + " textures" +
          (lacks.empty() ? "; every mesh's texture is there" : "; textures the meshes name but the model lacks (drawn black): " + lacks));
  return true;
}

// ---------------------------------------------------------------- the idle

const char* const kBones[21] = {"root",      "koshi",     "mune",    "kubi",    "atama",   "l_sakotsu", "l_ninoude",
                                "l_kote",    "l_te",      "r_sakotsu", "r_ninoude", "r_kote", "r_te",    "l_momo",
                                "l_sune",    "l_ashi",    "l_tsumasaki", "r_momo", "r_sune", "r_ashi", "r_tsumasaki"};

struct Channel {
  float scale = 1;
  std::vector<int32_t> v;  // keys
};
struct HandPose;
struct Motion {
  int keys = 0;  // rotation keys (30 a second)
  std::array<Channel, 3> root;
  std::array<std::array<Channel, 3>, 4> ik;  // l_te, r_te, l_ashi, r_ashi
  std::array<Channel, 4> pole;
  std::array<std::array<Channel, 3>, 21> rot;  // empty: that axis is 0
  std::array<bool, 21> has{};
  bool fk = false;  // YMKs: every bone has its rotation (no IK for the limbs)
  struct HandEvent {
    int frame, side, id, blend;  // side 1 left, 2 right; a pose of the hand library
  };
  std::vector<HandEvent> hands;
  std::vector<std::pair<int, int>> attack;  // (frame, group mask): op 40 sets the attack spheres' groups, op 41 clears (0)
  // the motion its person goes on to (the last event op 49 - u16 motion id; docs/MOVE_PACKS.md "The victim's end"):
  // -1 none said, 0 none (the game: back to the stance / the get-up)
  int next = -1;
  std::map<int, int> ops;  // event op -> how many (the move inspector)
  // an entrance's finger tracks (the entrant's y3 = left, y2 = right: one library-style record a frame, docs/HAND_POSES.md),
  // used instead of the op 21 hand poses when there (full_preview.cpp)
  std::array<std::shared_ptr<const HandPose>, 2> fingers;
};

// ---- hands: the finger poses (m.pac MVMT/STAT child 99), picked by YMKs event op 21
struct HandPose {
  int frames = 1;
  std::array<std::vector<std::array<int, 4>>, 30> keys;  // per finger bone: {frame, x, y, z} (s16 angle codes)
};
std::map<int, HandPose> g_hand_lib;
std::mutex g_hand_mutex;
bool g_hand_loaded = false;
std::atomic<bool> g_hand_ready{false};  // (the library is complete: Pose reads it)

const int kStep[8] = {57, 57, 57, 57, 77, 102, 128, 153};

struct Reader {
  const Bytes& d;
  size_t p;
  bool ok = true;
  uint8_t U8(size_t at) {
    if (at >= d.size()) { ok = false; return 0; }
    return d[at];
  }
};

// codes -> init + running sum (key 0 = init)
std::vector<int32_t> Adpcm(int32_t init, int step, const std::vector<int>& codes, bool four_bits) {
  std::vector<int32_t> out{init};
  int32_t v = init;
  for (int c : codes) {
    int mag, d, idx;
    bool neg;
    if (four_bits) mag = c & 7, neg = c & 8, d = ((mag * 2 + 1) * step) >> 3, idx = mag;
    else mag = c & 0x1F, neg = c & 0x20, d = ((mag * 2 + 1) * step) >> 5, idx = std::min(mag >> 2, 7);
    v = neg ? v - d : v + d;
    step = std::clamp((step * kStep[idx]) >> 6, 127, 24576);
    out.push_back(v);
  }
  return out;
}
std::vector<int> Nibbles(Reader& r, size_t at, size_t n) {
  std::vector<int> out(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t b = r.U8(at + (i >> 1));
    out[i] = (i & 1) ? (b & 15) : (b >> 4);
  }
  return out;
}
int32_t Wrap16(int32_t x) { return int32_t(int16_t(uint16_t(x))); }
// translation: s24 init, step byte, 2K nibbles (K + 4 bytes)
void Ch32(Reader& r, int K, Channel& c) {
  const size_t p = r.p;
  int32_t init = r.U8(p) | r.U8(p + 1) << 8 | r.U8(p + 2) << 16;
  if (init & 0x800000) init -= 0x1000000;
  c.v = Adpcm(init, r.U8(p + 3) * 127, Nibbles(r, p + 4, size_t(2 * K)), true);
  r.p += size_t(K) + 4;
}
// rotation: 'r' raw s16 (2K), '6' 6-bit (K + 3), 'n' nibbles ((K+1)/2 + 3)
void Ch16(Reader& r, int K, char mode, Channel& c) {
  const size_t p = r.p;
  if (mode == 'r') {
    c.v.resize(size_t(K));
    for (int i = 0; i < K; ++i) c.v[size_t(i)] = Wrap16(r.U8(p + 2 * i) | r.U8(p + 2 * i + 1) << 8);
    r.p += size_t(2 * K);
    return;
  }
  const int32_t init = Wrap16(r.U8(p) | r.U8(p + 1) << 8);
  const int step = r.U8(p + 2) * 127;
  if (mode == '6') {
    std::vector<int> codes(static_cast<size_t>(K));
    for (int i = 0; i < K; ++i) codes[size_t(i)] = r.U8(p + 3 + i);
    c.v = Adpcm(init, step, codes, false);
    r.p += size_t(K) + 3;
  } else {
    c.v = Adpcm(init, step, Nibbles(r, p + 3, size_t(K)), true);
    r.p += size_t((K + 1) / 2) + 3;
  }
  for (auto& x : c.v) x = Wrap16(x);
}

// ---- YMKs (the match moves: m.pac MOT banks; tools/ymks.py and YMKS_SPEC.md)
// Directory as YMBs; a motion's data is a frame stream (byte < 0x60: skip that
// many frames; 0x60 + u16 size or 0x61..0xFF = size + 0x60: a record taking
// one frame). A record may start with an event block (bit 7: its size), then
// body sub-records: a root (0.01 model units, f32 or s16) and bone rotations
// (s16 angles, Euler z*y*x) for all 21 bones or a masked few, reached in
// count + 1 frames.
bool DecodeYmks(const Bytes& b, int id, int x, int y, Motion& m, std::string& err) {
  if (b.size() < 0x114) return err = "not a motion bank", false;
  m.hands.clear();
  m.attack.clear();
  const uint32_t n = Le32(&b[0x110]);
  const size_t base = 0x114 + 16 * size_t(n);
  if (base > b.size()) return err = "the bank's directory runs past it", false;
  const size_t dl = b.size() - base;
  int found = -1;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = &b[0x114 + 16 * i];
    if (e[0] == y && e[1] == x && Le16(e + 2) == id) { found = int(i); break; }
  }
  if (found < 0) return err = "track " + std::to_string(y) + " isn't in the bank", false;
  const uint32_t off = Le32(&b[0x114 + 16 * size_t(found) + 4]);
  if (off >= dl) return err = "an empty motion", false;
  size_t end = dl;  // (the data runs to the next entry's start)
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t o = Le32(&b[0x114 + 16 * i + 4]);
    if (o > off && o < end) end = o;
  }
  const uint8_t* d = &b[base + off];
  const size_t size = end - off;
  // M1 bone types (ymks.TYPES_M1): which axes, how stored
  static const int kTypes[21] = {7, 7, 7, 6, 0x17, 6, 7, 3, 6, 6, 7, 3, 6, 7, 2, 0x17, 2, 7, 2, 0x17, 2};
  auto axes = [](int t) { return t == 2 ? "y" : t == 3 ? "xy" : t == 4 ? "z" : t == 6 ? "yz" : "xyz"; };
  struct Key1 { int frame; int v; };
  std::array<std::array<std::vector<Key1>, 3>, 21> rk;
  std::array<std::vector<std::pair<int, float>>, 3> root;
  auto rd_bone = [&](const uint8_t* q, size_t left, int bone, int frame) -> size_t {
    const char* ax = axes(kTypes[bone]);
    const size_t need = 2 * std::strlen(ax);
    if (need > left) return size_t(-1);
    for (size_t k = 0; ax[k]; ++k) {
      const int a = ax[k] - 'x';
      rk[size_t(bone)][size_t(a)].push_back({frame, int(int16_t(Le16(q + 2 * k)))});
    }
    return need;
  };
  auto set_root = [&](int frame, float rx, float ry, float rz) {
    root[0].push_back({frame, rx}), root[1].push_back({frame, ry}), root[2].push_back({frame, rz});
  };
  auto f32 = [](const uint8_t* q) { return LeF(q); };
  auto s16r = [](const uint8_t* q) { return float(int16_t(Le16(q))); };
  int frame = 0, last = 0;
  for (size_t p = 0; p < size;) {
    const uint8_t c = d[p];
    if (c < 0x60) { frame += c, ++p; continue; }
    size_t sz;
    if (c == 0x60) {
      if (p + 3 > size) break;
      sz = size_t(d[p + 1]) | size_t(d[p + 2]) << 8, p += 3;
    } else {
      sz = c - 0x60u, ++p;
    }
    if (p + sz > size) break;
    const uint8_t* r = d + p;
    p += sz;
    size_t q = 0;
    if (sz && (r[0] & 0x80)) {  // events: the hand poses (op 21) are used, the rest not shown here
      q = 1 + (r[0] & 0x7Fu);
      for (size_t e = 1; e < q && e < sz;) {
        const uint8_t op = r[e];
        const size_t n = b[0x10 + op];  // (the bank's event-size table, head +0x10)
        if (!n || e + n > q || e + n > sz) break;
        if (op == 21 && n >= 6) m.hands.push_back({frame, r[e + 1], int(Le16(r + e + 2)), int(Le16(r + e + 4))});
        // attack windows (HIT_VOLUMES.md): op 87 / 94 in strikes, op 40 / 41 in weapon motions - set / clear the
        // attack spheres' group mask; a window left open runs to the motion's end
        if ((op == 40 || op == 87) && n >= 2) m.attack.push_back({frame, r[e + 1]});
        if (op == 41 || op == 94) m.attack.push_back({frame, 0});
        if (op == 49 && n >= 3) m.next = int(Le16(r + e + 1));
        ++m.ops[op];
        e += n;
      }
    }
    while (q + 1 < sz) {
      const uint8_t op = r[q];
      const size_t left = sz - q;
      auto bones = [&](size_t at, uint32_t mask) -> size_t {
        for (int k = 0; k < 21; ++k)
          if (mask >> k & 1) {
            const size_t used = rd_bone(r + at, sz - at, k, frame);
            if (used == size_t(-1)) return size_t(-1);
            at += used;
          }
        return at;
      };
      size_t next = size_t(-1);
      if (op == 0x7E && left >= 8) set_root(frame, s16r(r + q + 2), s16r(r + q + 4), s16r(r + q + 6)), next = q + 8;
      else if (op == 0x7F && left >= 14) set_root(frame, f32(r + q + 2), f32(r + q + 6), f32(r + q + 10)), next = q + 14;
      else if (op == 0x7B && left >= 9) set_root(frame, s16r(r + q + 3), s16r(r + q + 5), s16r(r + q + 7)), next = q + 9;
      else if (op == 0x76 && left >= 7) next = q + 7;
      else if (op == 0x74 && left >= 3) {
        size_t t = q + 1;
        if (r[t++]) t += 2;
        if (t < sz && r[t++]) t += 4;
        next = t;
      } else if (op == 0x7D && left >= 14) set_root(frame, f32(r + q + 2), f32(r + q + 6), f32(r + q + 10)), next = bones(q + 14, 0x1FFFFF);
      else if (op == 0x7C && left >= 8) set_root(frame, s16r(r + q + 2), s16r(r + q + 4), s16r(r + q + 6)), next = bones(q + 8, 0x1FFFFF);
      else if (op >= 0x60 && op <= 0x6F && left >= 7) set_root(frame, s16r(r + q + 1), s16r(r + q + 3), s16r(r + q + 5)), next = bones(q + 7, 0x1FFFFF);
      else if ((op == 0x7A && left >= 17) || (op == 0x75 && left >= 11) || (op >= 0x50 && op <= 0x5F && left >= 16)) {
        size_t at;
        if (op == 0x7A) set_root(frame, f32(r + q + 2), f32(r + q + 6), f32(r + q + 10)), at = q + 14;
        else if (op == 0x75) set_root(frame, s16r(r + q + 2), s16r(r + q + 4), s16r(r + q + 6)), at = q + 8;
        else set_root(frame, f32(r + q + 1), f32(r + q + 5), f32(r + q + 9)), at = q + 13;
        const uint32_t mask = r[at] | uint32_t(r[at + 1]) << 8 | uint32_t(r[at + 2]) << 16;
        next = bones(at + 3, mask);
      } else if (op < 21 && left >= 2) {  // one bone: [bone][count][data]
        const size_t used = rd_bone(r + q + 2, sz - q - 2, op, frame);
        if (used != size_t(-1)) next = q + 2 + used;
      }
      if (next == size_t(-1) || next > sz) break;  // (an unknown sub-record, or the stock quirk of a few left-over bytes)
      q = next;
    }
    last = frame;
    ++frame;
  }
  bool any = false;
  for (const auto& b2 : rk)
    for (const auto& a : b2) any |= !a.empty();
  if (!any) return err = "track " + std::to_string(y) + " has no body poses (a camera or a prop)", false;
  // per frame: keys between records blended in a straight line (angles the short way round)
  auto hands = std::move(m.hands);
  auto attack = std::move(m.attack);
  const int next = m.next;
  auto ops = std::move(m.ops);
  m = Motion();
  m.ops = std::move(ops);
  m.hands = std::move(hands);
  m.attack = std::move(attack);
  m.next = next;
  if (!m.attack.empty()) {
    std::string w;
    for (const auto& [f, mask] : m.attack) w += " " + std::to_string(mask) + "@" + std::to_string(f);
    mm::Log("motion " + std::to_string(id) + ": attack windows (mask@key)" + w);
  }
  m.fk = true;
  m.keys = last + 1;
  for (int k = 0; k < 21; ++k)
    for (int a = 0; a < 3; ++a) {
      const auto& ks = rk[size_t(k)][size_t(a)];
      if (ks.empty()) continue;
      m.has[size_t(k)] = true;
      Channel& ch = m.rot[size_t(k)][size_t(a)];
      ch.v.resize(size_t(m.keys));
      size_t j = 0;
      for (int f = 0; f < m.keys; ++f) {
        while (j + 1 < ks.size() && ks[j + 1].frame <= f) ++j;
        int v = ks[j].v;
        if (f > ks[j].frame && j + 1 < ks.size()) {
          const float t = float(f - ks[j].frame) / float(ks[j + 1].frame - ks[j].frame);
          const int dv = int(int16_t(uint16_t(ks[j + 1].v - ks[j].v)));
          v = ks[j].v + int(std::lround(dv * t));
        }
        ch.v[size_t(f)] = v;
      }
    }
  for (int a = 0; a < 3; ++a) {  // root, in the YMBs channel's units (1e-4) at its 60-a-second rate
    const auto& ks = root[size_t(a)];
    Channel& ch = m.root[size_t(a)];
    if (ks.empty()) continue;
    ch.v.resize(size_t(m.keys) * 2);
    std::vector<float> per(size_t(m.keys));
    size_t j = 0;
    for (int f = 0; f < m.keys; ++f) {
      while (j + 1 < ks.size() && ks[j + 1].first <= f) ++j;
      float v = ks[j].second;
      if (f > ks[j].first && j + 1 < ks.size())
        v += (ks[j + 1].second - v) * float(f - ks[j].first) / float(ks[j + 1].first - ks[j].first);
      per[size_t(f)] = v;
    }
    // (the odd 60-a-second samples half way to the next frame's - both the frame's own made the root hold half a key
    // and jump: the whole body judders at 30 a second against a smooth 60 a second camera)
    for (int f = 0; f < m.keys; ++f) {
      const float a = per[size_t(f)], b = per[size_t(std::min(f + 1, m.keys - 1))];
      ch.v[size_t(f) * 2] = int32_t(std::lround(a * 0.01f / 1e-4f));
      ch.v[size_t(f) * 2 + 1] = int32_t(std::lround((a + b) * 0.5f * 0.01f / 1e-4f));
    }
  }
  return true;
}

bool DecodeMotion(const Bytes& b, int id, int x, int y, Motion& m, std::string& err) {
  if (b.size() >= 4 && !std::memcmp(b.data(), "YMKs", 4)) return DecodeYmks(b, id, x, y, m, err);
  if (b.size() < 0x114 || std::memcmp(b.data(), "YMBs", 4)) {
    err = "the bank is not YMBs (match moves are YMKs, which only the game plays)";
    return false;
  }
  const uint8_t* T = &b[0x10];
  const uint32_t n = Le32(&b[0x110]);
  if (0x114 + size_t(n) * 16 + 4 + size_t(n) * 20 > b.size()) {
    err = "the bank is cut short";
    return false;
  }
  const size_t base = 0x114 + size_t(n) * 16;
  int k = -1;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = &b[0x114 + 16 * i];
    if (e[0] == y && e[1] == x && Le16(e + 2) == id) { k = int(i); break; }
  }
  if (k < 0) {
    err = "that track of the motion is not in the bank";
    return false;
  }
  const uint8_t* e = &b[0x114 + 16 * size_t(k)];
  const uint8_t* h = &b[base + 4 + 20 * size_t(k)];
  const int type = h[0], nseg = h[1], interval = h[2] ? h[2] : 1;
  const uint32_t frames = Le32(e + 8);
  if (nseg || type > 1) {
    err = "the motion's layout (segments / type) isn't understood yet";
    return false;
  }
  const int K = int((frames + uint32_t(interval) - 1) / uint32_t(interval));
  m = {};
  m.keys = K;
  Reader r{b, base + Le32(e + 4)};
  const uint8_t* list = T + T[type + 1];
  const uint8_t* desc = T + T[T[0] + 1];
  int rot = 0, ik = 0, pole = 0;
  for (int c = 0; c < list[0]; ++c) {
    const uint8_t* ds = desc + 4 * (list[1 + c] - 1);
    const uint8_t f = ds[0], codec = ds[1];
    const float scale = float(ds[2]);
    if (f & 0x80) {
      if (f & 0x40) {
        for (int a = 0; a < 3; ++a) {
          Channel& ch = m.ik[size_t(std::min(ik, 3))][size_t(a)];
          Ch32(r, K, ch);
          ch.scale = scale;
        }
        ++ik;
      } else {
        Channel& ch = m.pole[size_t(std::min(pole, 3))];
        Ch16(r, K, 'n', ch);
        ch.scale = scale;
        ++pole;
      }
    } else if (f & 0x40) {
      for (int a = 0; a < 3; ++a) {
        Ch32(r, K, m.root[size_t(a)]);
        m.root[size_t(a)].scale = scale;
      }
    } else if (f & 0x20) {
      Channel skip;
      Ch16(r, K, (codec & 15) == 2 ? '6' : 'n', skip);
    } else {
      if (rot < 21) m.has[size_t(rot)] = (f & 7) != 0;
      for (int a = 0; a < 3; ++a) {
        if (!(f & (1 << a))) continue;
        const char mode = (codec & 0xF0) == 0x40 ? 'r' : (codec & 15) == 2 ? '6' : 'n';
        Channel& ch = m.rot[size_t(std::min(rot, 20))][size_t(a)];
        Ch16(r, K, mode, ch);
        ch.scale = scale;
      }
      ++rot;
    }
  }
  if (!r.ok || rot != 21 || ik != 4 || pole != 4) {
    err = "the motion did not decode";
    return false;
  }
  return true;
}

bool ReadWide(const std::wstring& path, Bytes& out) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  const int64_t n = _ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  out.resize(n > 0 ? size_t(n) : 0);
  const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

// One entry of a big EPAC without reading the whole file.
bool EpacEntry(const fs::path& file, const char* group, const char* name, Bytes& out) {
  FILE* f = _wfopen(file.c_str(), L"rb");
  if (!f) return false;
  Bytes idx(0x4000);
  bool ok = std::fread(idx.data(), 1, idx.size(), f) == idx.size() && !std::memcmp(idx.data(), "EPAC", 4);
  bool found = false;
  for (size_t p = 0x800; ok && !found && p + 12 <= 0x4000;) {
    if (!Le32(&idx[p])) break;
    const bool g = !std::memcmp(&idx[p], group, 4);
    const uint32_t cnt = Le32(&idx[p + 4]) / 3;
    p += 12;
    for (uint32_t i = 0; i < cnt && p + 12 <= 0x4000; ++i, p += 12) {
      if (!g || std::memcmp(&idx[p], name, 4)) continue;
      out.resize(size_t(Le32(&idx[p + 8])) * 0x100);
      found = _fseeki64(f, 0x4000 + int64_t(Le32(&idx[p + 4])) * 0x800, SEEK_SET) == 0 &&
              std::fread(out.data(), 1, out.size(), f) == out.size();
      break;
    }
  }
  std::fclose(f);
  return found;
}

bool LoadIdle(const std::wstring& game, Motion& m, std::string& err) {
  Bytes stat;
  if (!EpacEntry(fs::path(game) / L"pac" / L"m.pac", "MVMT", "STAT", stat)) {
    err = "no idle motion (pac/m.pac MVMT/STAT)";
    return false;
  }
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(stat), kids)) {
    err = "the idle motions are not a PACH";
    return false;
  }
  for (const auto& k : kids)
    if (k.id == 3) return DecodeMotion(Unpack(k.data), 20000, 10, 0, m, err);
  err = "no idle bank in m.pac";
  return false;
}

// ---------------------------------------------------------------- posing

float Key(const Channel& c, float at, bool angle, float unit) {
  if (c.v.empty()) return 0;
  const int n = int(c.v.size());
  const int i0 = std::clamp(int(std::floor(at)), 0, n - 1), i1 = std::min(i0 + 1, n - 1);
  const float f = std::clamp(at - float(i0), 0.0f, 1.0f);
  float a = float(c.v[size_t(i0)]), b = float(c.v[size_t(i1)]);
  if (angle) b = a + float(Wrap16(int32_t(b - a)));
  return (a + (b - a) * f) * c.scale * unit;
}

bool EpacEntry(const fs::path& file, const char* group, const char* name, Bytes& out);

// The hand library, once (m.pac MVMT/STAT, PACH child 99: a YMKs bank of poses by id).
// A track of hand-pose records (the library's, and an entrance's finger tracks): per record, after its
// events, 5-byte sub-records [k][count][s8 x][s8 y][s8 z] (k: finger joint 0-14 left, 15-29 right).
HandPose ParseHandRecords(const uint8_t* d, size_t size, int frames) {
  HandPose hp;
  hp.frames = std::max(1, frames);
  int frame = 0;
  for (size_t p = 0; p < size;) {
    const uint8_t c = d[p];
    if (c < 0x60) { frame += c, ++p; continue; }
    size_t sz;
    if (c == 0x60) {
      if (p + 3 > size) break;
      sz = size_t(d[p + 1]) | size_t(d[p + 2]) << 8, p += 3;
    } else {
      sz = c - 0x60u, ++p;
    }
    if (p + sz > size) break;
    const uint8_t* r = d + p;
    p += sz;
    size_t q = sz && (r[0] & 0x80) ? 1 + (r[0] & 0x7Fu) : 0;
    for (; q + 5 <= sz; q += 5) {
      const int k = r[q];
      if (k >= 30) break;
      hp.keys[size_t(k)].push_back({frame, int(int8_t(r[q + 2])) * 256, int(int8_t(r[q + 3])) * 256, int(int8_t(r[q + 4])) * 256});
    }
    ++frame;
  }
  return hp;
}

void LoadHands(const std::wstring& game) {
  std::lock_guard lock(g_hand_mutex);
  if (g_hand_loaded) return;
  g_hand_loaded = true;
  Bytes entry;
  if (!EpacEntry(fs::path(game) / L"pac" / L"m.pac", "MVMT", "STAT", entry)) return;
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(entry), kids)) return;
  Bytes bank;
  for (const auto& k : kids)
    if (k.id == 99) bank = Unpack(k.data);
  if (bank.size() < 0x114 || std::memcmp(bank.data(), "YMKs", 4)) return;
  std::map<int, HandPose> lib;  // (built here, published whole)
  const uint32_t n = Le32(&bank[0x110]);
  const size_t base = 0x114 + 16 * size_t(n);
  if (base > bank.size()) return;
  const size_t dl = bank.size() - base;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = &bank[0x114 + 16 * i];
    const int id = Le16(e + 2);
    const uint32_t off = Le32(e + 4);
    if (id >= 1000 || off >= dl || lib.count(id)) continue;  // (hands only; 1000+ are faces)
    size_t end = dl;
    for (uint32_t j = 0; j < n; ++j) {
      const uint32_t o = Le32(&bank[0x114 + 16 * j + 4]);
      if (o > off && o < end) end = o;
    }
    lib[id] = ParseHandRecords(&bank[base + off], end - off, int(Le32(e + 8)));
  }
  g_hand_lib = std::move(lib);
  g_hand_ready = true;
}

// A hand pose's angles (s16 codes) for finger bone k at time t (looped for animated poses).
std::array<float, 3> PoseAngles(const HandPose& hp, int k, float t, bool loop) {
  const auto& ks = hp.keys[size_t(k)];
  if (ks.empty()) return {0, 0, 0};
  if (loop && hp.frames > 1) t = std::fmod(std::max(0.0f, t), float(hp.frames));
  size_t j = 0;
  while (j + 1 < ks.size() && float(ks[j + 1][0]) <= t) ++j;
  std::array<float, 3> v = {float(ks[j][1]), float(ks[j][2]), float(ks[j][3])};
  if (j + 1 < ks.size() && t > float(ks[j][0])) {
    const float f = (t - float(ks[j][0])) / float(ks[j + 1][0] - ks[j][0]);
    for (int a = 0; a < 3; ++a) v[size_t(a)] += (float(ks[j + 1][size_t(a) + 1]) - v[size_t(a)]) * f;
  }
  return v;
}
std::array<float, 3> HandAngles(int id, int k, float t) {
  const auto it = g_hand_lib.find(id);
  if (it == g_hand_lib.end()) return {0, 0, 0};
  return PoseAngles(it->second, k, t, true);
}

// The skeleton's body: the 21 animated bones and the limbs (not attachments, fingers, face).
struct IkNumbers {
  float d = 0, L = 0, bend = 0;
};
IkNumbers g_ik_last[4];  // (the last Pose's solve, per limb: for the pose-dump comparison)

// The game's limb template: the standard 2011 skeleton's local translations (ch104's, tools/svr11_skeleton.json) of
// the bones the arms' and legs' solve stands on. The game solves every character's limbs on it (scaled with its
// height), not on the model's own bones (pose dump, 21 Oct: the parts' translations the same for a made model, Mickie
// James' ch143, Hornswoggle and the rest; a made model's bends equal to a stock one's).
const float* TemplateT(const std::string& n) {
  static const struct {
    const char* name;
    float t[3];
  } kT[] = {
      // (not the root: its place is the motion's)
      {"koshi", {-1.86908f, 1e-05f, 0}},
      {"mune", {2.23149f, 0, 0}},
      {"l_sakotsu", {1.7637f, -0.00014f, 0}},
      {"l_ninoude", {2.0f, 0.00019f, 0}},
      {"l_ninoude_x", {1.40795f, 0.00014f, 0}},
      {"l_kote", {1.40696f, 0, 0}},
      {"r_sakotsu", {1.7637f, -0.00014f, 0}},
      {"r_ninoude", {1.99998f, 0, 0}},
      {"r_ninoude_x", {1.40794f, 0, 0}},
      {"r_kote", {1.40696f, 0, 0}},
      {"l_momo", {0, -1.142f, -0.00014f}},
      {"l_momo_x", {2.16597f, 0, 0}},
      {"l_sune", {2.18203f, 0, 0}},
      {"r_momo", {0, 1.1418f, -0.00014f}},
      {"r_momo_x", {2.16535f, -8e-05f, 0}},
      {"r_sune", {2.18264f, 0.00012f, 0}},
  };
  for (const auto& b : kT)
    if (n == b.name) return b.t;
  return nullptr;
}

bool BodyBone(const std::string& n) {
  for (const char* b : kBones)
    if (n == b) return true;
  for (const char* b : {"l_ninoude", "r_ninoude", "l_kote", "r_kote", "l_te", "r_te", "l_momo", "r_momo", "l_sune", "r_sune", "l_ashi", "r_ashi"})
    if (n == b) return true;
  return false;
}

// World transforms of every bone for the motion at `key` (rotation keys).
std::vector<Xf> Pose(const Character& ch, const Motion& m, float key, std::vector<Xf>* locals_out = nullptr) {
  const float kAng = 6.2831853f / 65536.0f, kMove = 1e-4f;
  const size_t nb = ch.bones.size();
  std::vector<Xf> local(nb);
  for (size_t i = 0; i < nb; ++i) {
    Euler(ch.bones[i].r, local[i].r);
    local[i].t = {ch.bones[i].t[0], ch.bones[i].t[1], ch.bones[i].t[2]};
  }
  std::map<std::string, int> idx;
  for (size_t i = 0; i < nb; ++i) idx.emplace(ch.bones[i].name, int(i));
  auto find = [&](const char* n) {
    auto it = idx.find(n);
    return it == idx.end() ? -1 : it->second;
  };
  float rot[21][3] = {};
  for (int c = 0; c < 21; ++c)
    for (int a = 0; a < 3; ++a) rot[c][a] = Key(m.rot[size_t(c)][size_t(a)], key, true, kAng);
  for (int c = 0; c < 21; ++c) {
    const int i = find(kBones[c]);
    if (i < 0 || !m.has[size_t(c)]) continue;
    if (m.fk) {  // YMKs, as the game poses them (measured: Big Show 125's bone matrices in his entrance): the key on
      // the bone's rest frame - its rest snapped to quarter turns (root 90, koshi 180, sakotsu +-90 stay) - not on the
      // bind pose's splay (momo -+15, ninoude +-15, kote 20 degrees aren't in the game's poses); the twists below
      const bool kote = c == 7 || c == 11;
      const float e[3] = {kote ? 0.0f : rot[c][0], rot[c][1], rot[c][2]};  // (the kote's x roll: on its twist bones)
      float snapped[3], frame[3][3], key[3][3];
      for (int a = 0; a < 3; ++a) snapped[a] = std::round(ch.bones[size_t(i)].r[a] / 1.5707963f) * 1.5707963f;
      Euler(snapped, frame);
      Euler(e, key);
      MulR(frame, key, local[size_t(i)].r);
    } else {
      // YMBs, as the game builds the pose (sub_823A4420): the IK bones aren't driven, sakotsu / te take y and z,
      // tsumasaki y only; the rest replaces the bone's rotation
      if (c == 6 || c == 7 || c == 10 || c == 11 || c == 13 || c == 14 || c == 17 || c == 18) continue;
      float e[3] = {rot[c][0], rot[c][1], rot[c][2]};
      if (c == 5 || c == 8 || c == 9 || c == 12) e[0] = 0;
      if (c == 16 || c == 20) e[0] = e[2] = 0;
      Euler(e, local[size_t(i)].r);
    }
  }
  if (m.fk)  // YMKs twists, as the game's entrance poses (measured: Big Show 125, the game's bone matrices)
    for (int side = 0; side < 2; ++side) {
      // the upper arm / thigh: their twist about the bone (x) split with the twist bone - ninoude_x half, momo_x 0.435
      // of it (the thigh keeping 0.435 too); the swing stays on the bone
      struct Tw {
        int c;
        const char *bone, *twb;
        float share, keep;
      };
      const Tw tws[2] = {{side ? 10 : 6, side ? "r_ninoude" : "l_ninoude", side ? "r_ninoude_x" : "l_ninoude_x", 0.5f, 0.5f},
                         {side ? 17 : 13, side ? "r_momo" : "l_momo", side ? "r_momo_x" : "l_momo_x", 0.435f, 0.435f}};
      for (const Tw& tw : tws) {
        const int i = find(tw.bone), j = find(tw.twb);
        if (i < 0 || j < 0 || !m.has[size_t(tw.c)]) continue;
        const float (&r)[3][3] = local[size_t(i)].r;
        const float qw = 0.5f * std::sqrt(std::max(0.0f, 1 + r[0][0] + r[1][1] + r[2][2]));
        const float qx = qw > 1e-5f ? (r[2][1] - r[1][2]) / (4 * qw) : 0.0f;
        const float twist = 2 * std::atan2(qx, qw);  // (about x)
        const float off[3] = {-(1 - tw.keep) * twist, 0, 0}, mine[3] = {tw.share * twist, 0, 0};
        float rx[3][3], cur[3][3];
        Euler(off, rx);
        std::memcpy(cur, local[size_t(i)].r, sizeof cur);
        MulR(cur, rx, local[size_t(i)].r);
        Euler(mine, local[size_t(j)].r);
      }
      // the forearm: the kote's x roll, half on each twist bone (all on one when there's one)
      const float roll = rot[side ? 11 : 7][0];
      const char* x1 = side ? "r_kote_x" : "l_kote_x";
      const char* x2 = side ? "r_kote_x2" : "l_kote_x2";
      const bool two = find(x2) >= 0;
      const float e[3] = {two ? roll * 0.5f : roll, 0, 0};
      if (const int i = find(x1); i >= 0) Euler(e, local[size_t(i)].r);
      if (const int i = find(x2); i >= 0) Euler(e, local[size_t(i)].r);
    }
  if (!m.fk)  // the kote's x roll: on the forearm twist bones (half each when there are two)
    for (int side = 0; side < 2; ++side) {
      const float roll = rot[side ? 11 : 7][0];
      const char* x1 = side ? "r_kote_x" : "l_kote_x";
      const char* x2 = side ? "r_kote_x2" : "l_kote_x2";
      const bool two = find(x2) >= 0;
      const float e[3] = {two ? roll * 0.5f : roll, 0, 0};
      if (const int i = find(x1); i >= 0) Euler(e, local[size_t(i)].r);
      if (const int i = find(x2); i >= 0) Euler(e, local[size_t(i)].r);
    }
  const int root = find("root");
  if (root >= 0)
    local[size_t(root)].t = {Key(m.root[0], 2 * key, false, kMove), Key(m.root[1], 2 * key, false, kMove),
                             Key(m.root[2], 2 * key, false, kMove)};
  if (g_hand_ready || m.fingers[0] || m.fingers[1]) {  // fingers: an entrance's tracks, else the hand poses
    // the motion's events pick (else the ring idle 10 / 210)
    for (int side = 1; side <= 2; ++side) {
      const HandPose* track = m.fingers[size_t(side - 1)].get();
      if (!track && !g_hand_ready) continue;
      const Motion::HandEvent* cur = nullptr;
      const Motion::HandEvent* prev = nullptr;
      for (const auto& e : m.hands)
        if (e.side == side && float(e.frame) <= key) prev = cur, cur = &e;
      const int idle = side == 1 ? 10 : 210;
      const int cur_id = cur ? cur->id : idle, prev_id = prev ? prev->id : idle;
      const float since = cur ? key - float(cur->frame) : key;
      const float w = cur && cur->blend > 0 ? std::clamp(since / float(cur->blend), 0.0f, 1.0f) : 1.0f;
      for (int k = 0; k < 15; ++k) {
        const int bone = (side - 1) * 15 + k;
        char name[16];
        std::snprintf(name, sizeof name, "%c_yubi%d%d", side == 1 ? 'l' : 'r', k / 3, k % 3);
        const int i = find(name);
        if (i < 0) continue;
        const auto a = track ? PoseAngles(*track, bone, key, false) : HandAngles(cur_id, bone, since);
        std::array<float, 3> v = a;
        if (w < 1 && !track) {
          const auto b2 = HandAngles(prev_id, bone, prev ? key - float(prev->frame) : key);
          for (int c = 0; c < 3; ++c) v[size_t(c)] = b2[size_t(c)] + (a[size_t(c)] - b2[size_t(c)]) * w;
        }
        const float e[3] = {v[0] * kAng, v[1] * kAng, v[2] * kAng};
        float rr[3][3], rest[3][3];
        Euler(e, rr);
        std::memcpy(rest, local[size_t(i)].r, sizeof rest);
        MulR(rest, rr, local[size_t(i)].r);
      }
    }
  }
  std::vector<Xf> w(nb);
  auto fk = [&] {
    for (size_t i = 0; i < nb; ++i) {
      const int p = ch.bones[i].parent;
      w[i] = p >= 0 && size_t(p) < i ? Mul(w[size_t(p)], local[i]) : local[i];
    }
  };
  fk();
  if (root < 0 || m.fk) {  // (YMKs: the limbs have their own rotations)
    if (locals_out) *locals_out = local;
    return w;
  }
  const V3 root_p = w[size_t(root)].t;
  // The limbs are solved as the game solves them: on its limb template (TemplateT) - the shoulder / hip and the
  // segment lengths the template's, the rotations (the torso's keys) the pose's - and the angles put on the model's
  // own bones. Solved on the model's own lengths, a made model with other proportions bent its arms further and
  // locked its legs straight: the 3D view's hunched idle (user report 21 Oct), where the game stands it up. (A small
  // rig - limbs well under the template's: Hornswoggle - keeps its own: the game retargets its targets,
  // small_rig.cpp.)
  std::vector<Xf> wt(nb);
  {
    std::vector<Xf> lt = local;
    for (size_t i = 0; i < nb; ++i)
      if (const float* t = TemplateT(ch.bones[i].name)) lt[i].t = {t[0], t[1], t[2]};
    for (size_t i = 0; i < nb; ++i) {
      const int p = ch.bones[i].parent;
      wt[i] = p >= 0 && size_t(p) < i ? Mul(wt[size_t(p)], lt[i]) : lt[i];
    }
  }
  // the game's solve (sub_823A4DD8), in the parent frame: arms the sakotsu's, legs the root's
  struct Limb {
    const char *up, *lo, *parent;
    bool arm;
  };
  const Limb limbs[4] = {{"l_ninoude", "l_kote", "l_sakotsu", true},
                         {"r_ninoude", "r_kote", "r_sakotsu", true},
                         {"l_momo", "l_sune", "root", false},
                         {"r_momo", "r_sune", "root", false}};
  auto rot_x = [](float a, float r[3][3]) { const float e[3] = {a, 0, 0}; Euler(e, r); };
  auto rot_y = [](float a, float r[3][3]) { const float e[3] = {0, a, 0}; Euler(e, r); };
  for (int l = 0; l < 4; ++l) {
    const int iu = find(limbs[l].up), il = find(limbs[l].lo), ip = find(limbs[l].parent);
    if (iu < 0 || il < 0 || ip < 0) continue;
    const int su = ch.bones[size_t(iu)].parent;  // (the upper bone's own parent in the skeleton)
    if (su < 0) continue;
    // (the template's limb unless the model's is a small rig's)
    const float Lt = 2 * Len(wt[size_t(il)].t - wt[size_t(iu)].t), Lm = 2 * Len(w[size_t(il)].t - w[size_t(iu)].t);
    const std::vector<Xf>& on = Lt > 1e-4f && Lm >= 0.7f * Lt ? wt : w;
    const Xf& P = on[size_t(ip)];
    const Xf Pi = Inverse(P);
    const V3 S = Apply(Pi, on[size_t(iu)].t);  // the shoulder / hip in the parent frame
    const V3 target = root_p + V3{Key(m.ik[size_t(l)][0], 2 * key, false, kMove), Key(m.ik[size_t(l)][1], 2 * key, false, kMove),
                                  Key(m.ik[size_t(l)][2], 2 * key, false, kMove)};
    V3 v = Apply(Pi, target) - S;
    // L: 2 x the upper segment (the game assumes equal segments) - shoulder / hip to elbow / knee, measured in
    // the world (the lower bone hangs off the twist bone half way down: its own offset is half a segment)
    const float L = &on == &wt ? Lt : Lm;
    float d = Len(v);
    if (L <= 1e-4f || d <= 1e-5f) continue;
    const float lim = (limbs[l].arm ? 0.99f : 0.99999f) * L;
    if (d >= lim) d = limbs[l].arm ? lim : lim + (d - lim) * 0.3f;
    float bend = d >= L ? 0.0f : 2 * std::acos(std::min(d / L, 0.99999f));
    if (!limbs[l].arm) bend = -bend;
    g_ik_last[l] = {Len(v), L, bend};
    const V3 n = Norm(v);
    // Align: the shortest arc +X -> n (axis X x n)
    float A[3][3];
    {
      const V3 axis{0, -n.z, n.y};
      const float al = Len(axis), ang = std::acos(std::clamp(n.x, -1.0f, 1.0f));
      if (al < 1e-6f) {
        float id[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        if (n.x < 0) id[0][0] = -1, id[1][1] = -1;  // (half turn about z)
        std::memcpy(A, id, sizeof A);
      } else {
        const V3 k = axis * (1 / al);
        const float c = std::cos(ang), sn = std::sin(ang), t = 1 - c;
        A[0][0] = c + k.x * k.x * t, A[0][1] = k.x * k.y * t - k.z * sn, A[0][2] = k.x * k.z * t + k.y * sn;
        A[1][0] = k.y * k.x * t + k.z * sn, A[1][1] = c + k.y * k.y * t, A[1][2] = k.y * k.z * t - k.x * sn;
        A[2][0] = k.z * k.x * t - k.y * sn, A[2][1] = k.z * k.y * t + k.x * sn, A[2][2] = c + k.z * k.z * t;
      }
    }
    float pole = Key(m.pole[size_t(l)], key, true, kAng);
    if (limbs[l].arm) pole -= 3.14159265f;
    float Rx[3][3], Ry[3][3], t1[3][3], R[3][3], world[3][3];
    rot_x(pole, Rx);
    rot_y(-bend * 0.5f, Ry);
    MulR(A, Rx, t1);
    MulR(t1, Ry, R);                                     // the upper bone in the parent frame
    MulR(P.r, R, world);                                 // ... in the world
    TransposeMul(w[size_t(su)].r, world, local[size_t(iu)].r);  // ... under its own parent
    // the twist bone (ninoude_x / momo_x, between the upper and the lower bone) takes half the upper bone's roll
    // about its own axis (x): a = swing-twist angle / 2 - fitted to the game's pose dump (0.0 deg off)
    if (const int it = find((std::string(limbs[l].up) + "_x").c_str()); it >= 0 && ch.bones[size_t(it)].parent == iu) {
      const float (&F)[3][3] = local[size_t(iu)].r;
      const float qw = std::sqrt(std::max(0.0f, 1 + F[0][0] + F[1][1] + F[2][2])) * 0.5f;
      const float qx = qw > 1e-6f ? (F[2][1] - F[1][2]) / (4 * qw) : 1.0f;
      const float a = std::atan2(qx, qw);  // (2 atan2(qx, qw) / 2)
      float back[3][3], upper[3][3];
      rot_x(-a, back);
      MulR(local[size_t(iu)].r, back, upper);
      std::memcpy(local[size_t(iu)].r, upper, sizeof upper);
      rot_x(a, local[size_t(it)].r);
    }
    rot_y(bend, local[size_t(il)].r);
    fk();
  }
  if (locals_out) *locals_out = local;
  return w;
}

// ---------------------------------------------------------------- GPU

struct GpuVertex {
  float pos[3], n[3], uv[2];
  float t[3];  // tangent (normal-mapped meshes; else zero)
};
struct Cb {
  float viewproj[16];
  float flags[4];  // x: alpha test, y: the floor, z: the normals (NormalMode)
  float key[4];    // toward the key light (view space, camera relative: upper left)
  float fill[4];   // toward the fill light (lower right)
  float fwd[4];    // the camera's forward
  float shadow[2][4];  // the floor's contact shadows: x, z, radius, strength (0: none)
  float mat[4];        // x shine (g_fSpecularLev), y its power, z a normal map, w the shine's mask (UseMaterial)
  float tint[4];       // the colour multiplier (g_f4MatDifCol)
};

const char* kShader = R"(
cbuffer C : register(b0) { row_major float4x4 viewproj; float4 flags; float4 key; float4 fill; float4 fwd; float4 shadow[2]; float4 mat; float4 tint; };
struct VI { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD; float3 t : TANGENT; };
struct VO { float4 p : SV_Position; float3 n : NORMAL; float2 uv : TEXCOORD0; float3 w : TEXCOORD1; float3 t : TEXCOORD2; };
VO vs(VI i) { VO o; o.p = mul(float4(i.p, 1), viewproj); o.n = i.n; o.uv = i.uv; o.w = i.p; o.t = i.t; return o; }
Texture2D t : register(t0);
Texture2D tn : register(t1);
Texture2D tm : register(t2);
SamplerState s : register(s0);
float4 ps(VO i) : SV_Target {
  float4 tx = t.Sample(s, i.uv);
  if (flags.x > 0.5 && tx.a < 0.3) discard;
  float3 n = normalize(i.n + 1e-5);
  // (a culled part lit as the game lights it - its normals point in, so the game's light comes from -n: a part with
  // its faces the wrong way round shows dark and inside out here too; flipped: turned over. Both sides: the back of
  // cloth and hair cards faces the camera)
  if (flags.z > 0.5) n = -n;
  else if (flags.z > -0.5) n *= dot(n, fwd.xyz) > 0 ? -1 : 1;
  if (mat.z > 0.5 && dot(i.t, i.t) > 1e-8) {  // the normal map (tangent space, +U along the tangent)
    float3 T = normalize(i.t - n * dot(n, i.t)), B = cross(n, T);
    float3 m = tn.Sample(s, i.uv).xyz * 2 - 1;
    n = normalize(T * m.x + B * m.y + n * max(m.z, 0.05));
  }
  float k = saturate(dot(n, key.xyz)), f = saturate(dot(n, fill.xyz));
  if (flags.y > 0.5) {  // the floor, with the people's contact shadows
    float dark = 1;
    for (int j = 0; j < 2; ++j)
      if (shadow[j].w > 0) dark *= 1 - shadow[j].w * 0.55 * smoothstep(shadow[j].z, 0, length(i.w.xz - shadow[j].xy));
    return float4(tx.rgb * (0.62 + 0.38 * k) * dark, 1);
  }
  float amb = lerp(0.30, 0.52, 0.5 + 0.5 * n.y);
  float rim = pow(1 - saturate(dot(n, -fwd.xyz)), 3) * 0.45;
  float3 h = normalize(key.xyz - fwd.xyz);  // the key light's highlight
  float mask = mat.w > 2.5 ? tm.Sample(s, i.uv).a : mat.w > 1.5 ? dot(tx.rgb, float3(0.3, 0.59, 0.11)) : mat.w > 0.5 ? tx.a : 1;
  float3 h2 = normalize(fill.xyz - fwd.xyz);  // (the game lights skin with four lights: the fill shines too)
  float pw = clamp(mat.y, 1, 300);
  float shine = mat.x * 0.45 * mask * (pow(saturate(dot(n, h)), pw) + 0.6 * pow(saturate(dot(n, h2)), pw));  // (linear in g_fSpecularLev, as the game)
  return float4(tx.rgb * tint.rgb * (amb + 0.80 * k + 0.28 * f + rim) + shine * max(k, f), 1);
}
)";

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11RasterizerState* g_raster_cull = nullptr;  // (culled meshes: render flag 0x2)
ID3D11RasterizerState* g_raster_cull_front = nullptr;  // (culled and turned over: Flip faces)
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11ShaderResourceView* g_white = nullptr;
ID3D11ShaderResourceView* g_floor_tex = nullptr;  // the ring canvas with a 1 m grid
ID3D11Buffer* g_floor_vb = nullptr;               // 4 vertices, set each frame (the floor height)
ID3D11Buffer* g_floor_ib = nullptr;
constexpr float kFloorHalf = 40;                  // model units (10 cm): an 8 m square
ID3D11Texture2D* g_rt = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11ShaderResourceView* g_rt_srv = nullptr;
ID3D11DepthStencilView* g_dsv = nullptr;
int g_rt_w = 0, g_rt_h = 0;
bool g_capturing = false;  // (Capture: a clear background, no floor, the model alone)
bool g_capture_both = false;  // (... and the dummy too: a move's frames)

struct GpuMesh {
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  ID3D11ShaderResourceView* ntex = nullptr;  // the normal map (null: none)
  ID3D11ShaderResourceView* mask = nullptr;  // a skin's shine mask: this map's alpha (the pac's normal map, kept when it's replaced)
  float spec = 0.5f, spec_pow = 50, tint[3] = {1, 1, 1};
  bool culled = true;
  bool flipped = false;      // (the panel's Flip faces: its fronts are its backs)
  bool skin_params = false;  // (PMesh) - culled too: the game's skin shader, shine from the normal map's alpha
  int spec_alpha = -1;  // the shine's mask: -1 the game's (skin: the normal map's alpha, else the colour's), 0 none, 1 the colour's alpha, 2 its brightness
  uint32_t model_id = 0;
  int mesh_index = 0;
  size_t pm = 0;  // (its index in the character's meshes)
  UINT count = 0;
  ID3D11ShaderResourceView* tex = nullptr;
  bool cutout = false;
  bool blood = false;
  bool blood_missing = false;  // a blood overlay whose texture wasn't found: never drawn
  int blood_kind = 0;          // 1 face, 2 body: drawn with the shared blood textures (misc.pac)
};

// state: slot 0 the model (track 0), slot 1 the dummy (track 1)
struct Slot {
  std::wstring file;
  std::atomic<int> generation{0};
  std::shared_ptr<Character> pending;  // loaded, not uploaded yet
  std::shared_ptr<Character> ch;
  std::vector<GpuMesh> gpu;
  std::map<std::string, ID3D11ShaderResourceView*> textures;
  std::vector<Xf> bind_inv;
  std::shared_ptr<Motion> motion;  // its track of the chosen motion (nullptr: the idle / bind pose)
  std::vector<Xf> world;           // the bones as posed this frame (the hit spheres' frame)
  bool hidden = false;             // (the dummy, while a motion with no track for it plays)
  const Motion* shown = nullptr;   // the motion posed, at key
  float key = 0;
  struct Phys {  // hair and jiggle (Physics): built once the character is in, settled on load / a new motion
    bool built = false, settle = true;
    std::vector<hair::Chain> chains;
    int rigid_chains = 0;  // (cloth chains kept as posed: BuildPhysics)
    std::vector<std::pair<int, hair::CapsuleParams>> caps;  // (bone, capsule)
    std::vector<hair::Muscle> muscles;
    std::vector<hair::MuscleRow> rows;
    double acc = 0;
    const void* motion = nullptr;
    float last_key = 0;
  } phys;
};
std::mutex g_mutex;
std::wstring g_game;
Slot g_slots[2];
std::shared_ptr<Motion> g_idle;
std::string g_status, g_motion_status;
Playback g_play;
std::shared_ptr<const Bytes> g_bank;
// A move as the game plays it (SetMove): its phases (x) in turn, each person on its own track - played to its own
// end, then held - and then each person's follow-up (op 49). Not set: the single motion (SetMotion) as before.
struct ChainSeg {
  std::shared_ptr<Motion> m;  // nullptr: no track for this person in this phase - the last pose held
  float start = 0, len = 0;   // keys (30 a second)
  int id = 0, x = 0;
};
struct Chain {
  std::vector<ChainSeg> segs[2];
  float total = 0;
  std::string steps;  // (what plays, for the status line / the move inspector)
};
std::shared_ptr<const Chain> g_chain;
// person s's motion and key at chain time t (nullptr: none yet - the idle)
const Motion* ChainAt(const Chain& c, int s, float t, float& key) {
  const ChainSeg* hold = nullptr;
  for (const ChainSeg& g : c.segs[s]) {
    if (t < g.start) break;
    if (g.m) hold = &g;
    if (t < g.start + g.len && g.m) {
      key = std::min(t - g.start, float(std::max(1, g.m->keys) - 1));
      return g.m.get();
    }
  }
  if (!hold) return nullptr;
  key = float(std::max(1, hold->m->keys) - 1);
  return hold->m.get();
}
float g_yaw = 0.35f, g_pitch = 0.22f, g_zoom = 1;
V3 g_center{0, 0, 0}, g_pan{0, 0, 0};
float g_height = 20;
bool g_blood = false;  // the blood overlays shown
int g_blood_stage = 2;  // which of the shared textures: stage 0 light .. 2 heavy
std::map<std::string, ID3D11ShaderResourceView*> g_blood_srv;  // "bloodface0_2" -> texture (misc.pac BATS/INIT/110)
bool g_blood_loaded = false;
bool g_frame = false;  // (frame the camera on the next posed frame)
ID3D11ShaderResourceView* g_colour_override = nullptr;  // (SetColourOverride: the model's "color" texture swapped)
int g_colour_override_frame = -100;

void Release(IUnknown* p) {
  if (p) p->Release();
}

void FreeCharacter(Slot& sl) {
  for (auto& g : sl.gpu) Release(g.vb), Release(g.ib);
  sl.gpu.clear();
  for (auto& [n, t] : sl.textures) Release(t);
  sl.textures.clear();
  sl.ch.reset();
}

ID3D11ShaderResourceView* MakeTexture(const Bytes& dds) {
  DdsInfo info;
  if (!DdsInfoOf(dds, info)) return nullptr;
  DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
  int block = 0;
  switch (info.format) {
    case DxtFormat::kDxt1: f = DXGI_FORMAT_BC1_UNORM, block = 8; break;
    case DxtFormat::kDxt3: f = DXGI_FORMAT_BC2_UNORM, block = 16; break;
    case DxtFormat::kDxt5: f = DXGI_FORMAT_BC3_UNORM, block = 16; break;
    case DxtFormat::kArgb: f = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default: return nullptr;
  }
  std::vector<D3D11_SUBRESOURCE_DATA> levels;
  size_t off = 128;
  int w = info.w, h = info.h;
  for (int l = 0; l < info.mips; ++l) {
    const size_t pitch = block ? size_t(std::max(1, (w + 3) / 4)) * size_t(block) : size_t(w) * 4;
    const size_t rows = block ? size_t(std::max(1, (h + 3) / 4)) : size_t(h);
    if (off + pitch * rows > dds.size()) break;
    levels.push_back({dds.data() + off, UINT(pitch), 0});
    off += pitch * rows;
    w = std::max(1, w / 2), h = std::max(1, h / 2);
  }
  if (levels.empty()) return nullptr;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = UINT(info.w);
  td.Height = UINT(info.h);
  td.MipLevels = UINT(levels.size());
  td.ArraySize = 1;
  td.Format = f;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  if (SUCCEEDED(g_dev->CreateTexture2D(&td, levels.data(), &tex))) {
    g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
  }
  return srv;
}

// The game's shared blood textures, once: misc.pac BATS/INIT, PACH child 110 (a texture bundle).
void LoadBloodTextures() {
  if (g_blood_loaded) return;
  g_blood_loaded = true;
  Bytes entry;
  if (g_game.empty() || !EpacEntry(fs::path(g_game) / L"pac" / L"misc.pac", "BATS", "INIT", entry)) return;
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(entry), kids)) return;
  for (const auto& k : kids) {
    if (k.id != 110) continue;
    std::vector<BundleTexture> texs;
    if (!BundleRead(Unpack(k.data), texs)) return;
    for (const auto& t : texs) g_blood_srv[Lower(t.name)] = MakeTexture(t.data);
  }
}

void Upload(Slot& sl, std::shared_ptr<Character> ch) {
  FreeCharacter(sl);
  sl.ch = std::move(ch);
  sl.phys = Slot::Phys();
  std::shared_ptr<Character>& g_char = sl.ch;
  std::vector<GpuMesh>& g_gpu = sl.gpu;
  std::map<std::string, ID3D11ShaderResourceView*>& g_textures = sl.textures;
  std::vector<Xf>& g_bind_inv = sl.bind_inv;
  for (const PMesh& pm : g_char->meshes) {
    GpuMesh g;
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = UINT(pm.pos.size() * sizeof(GpuVertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    g_dev->CreateBuffer(&bd, nullptr, &g.vb);
    D3D11_BUFFER_DESC ib = {};
    ib.Usage = D3D11_USAGE_IMMUTABLE;
    ib.ByteWidth = UINT(pm.idx.size() * 4);
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = {pm.idx.data(), 0, 0};
    g_dev->CreateBuffer(&ib, &sd, &g.ib);
    g.count = UINT(pm.idx.size());
    g.cutout = pm.cutout;
    g.blood = pm.blood;
    g.spec = pm.spec, g.spec_pow = pm.spec_pow, std::memcpy(g.tint, pm.tint, sizeof g.tint);
    g.culled = (pm.mflags & 0x2) != 0;
    g.model_id = pm.model_id, g.mesh_index = pm.mesh_index, g.pm = g_gpu.size();
    if (!pm.ntex.empty()) {
      auto nit = g_textures.find(pm.ntex);
      if (nit == g_textures.end()) {
        const auto src = g_char->textures.find(pm.ntex);
        nit = g_textures.emplace(pm.ntex, src == g_char->textures.end() ? nullptr : MakeTexture(src->second)).first;
      }
      g.ntex = g.mask = nit->second;
    }
    g.skin_params = pm.skin_params;
    if (pm.blood && (pm.tex == "bloodface" || pm.tex == "bloodbody")) {  // the game swaps in its shared ones (misc.pac)
      LoadBloodTextures();
      g.blood_kind = pm.tex == "bloodface" ? 1 : 2;
      g.blood_missing = g_blood_srv.empty();
      g_gpu.push_back(g);
      continue;
    }
    // (another blood texture in the model pac may be an 8 x 8 white stand-in: not shown)
    const auto bt = g_char->textures.find(pm.tex);
    if (pm.blood && (bt == g_char->textures.end() || bt->second.size() < 128 || Le32(&bt->second[16]) <= 16)) {
      g.blood_missing = true;
      g_gpu.push_back(g);
      continue;
    }
    auto it = g_textures.find(pm.tex);
    if (it == g_textures.end()) {
      auto src = g_char->textures.find(pm.tex);
      if (src == g_char->textures.end()) src = g_char->textures.find("color");
      it = g_textures.emplace(pm.tex, src == g_char->textures.end() ? nullptr : MakeTexture(src->second)).first;
    }
    g.tex = it->second ? it->second : g_white;
    g_gpu.push_back(g);
  }
  // bind pose (for skinning) and framing
  const size_t nb = g_char->bones.size();
  std::vector<Xf> w(nb);
  for (size_t i = 0; i < nb; ++i) {
    Xf l;
    Euler(g_char->bones[i].r, l.r);
    l.t = {g_char->bones[i].t[0], g_char->bones[i].t[1], g_char->bones[i].t[2]};
    const int p = g_char->bones[i].parent;
    w[i] = p >= 0 && size_t(p) < i ? Mul(w[size_t(p)], l) : l;
  }
  g_bind_inv.resize(nb);
  for (size_t i = 0; i < nb; ++i) g_bind_inv[i] = Inverse(w[i]);
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const PMesh& pm : g_char->meshes)
    for (const V3& p : pm.pos) {
      const float v[3] = {p.x, p.y, p.z};
      for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v[k]), hi[k] = std::max(hi[k], v[k]);
    }
  // (shown y up: y and z negated)
  if (&sl == &g_slots[0]) {
    g_center = {(lo[0] + hi[0]) / 2, -(lo[1] + hi[1]) / 2, -(lo[2] + hi[2]) / 2};
    g_height = std::max(1.0f, hi[1] - lo[1]);
    g_frame = true;
  }
}

bool CreatePipeline() {
  ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
  if (FAILED(D3DCompile(kShader, std::strlen(kShader), "char", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
      FAILED(D3DCompile(kShader, std::strlen(kShader), "char", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
    if (err) g_status = std::string("preview shader: ") + static_cast<const char*>(err->GetBufferPointer());
    return false;
  }
  g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
  g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
  const D3D11_INPUT_ELEMENT_DESC il[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  g_dev->CreateInputLayout(il, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);
  vsb->Release();
  psb->Release();
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(Cb);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  g_dev->CreateBuffer(&bd, nullptr, &g_cb);
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_ANISOTROPIC;
  sd.MaxAnisotropy = 8;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  g_dev->CreateSamplerState(&sd, &g_sampler);
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  g_dev->CreateRasterizerState(&rd, &g_raster);
  rd.CullMode = D3D11_CULL_BACK;  // (a culled mesh, flag 0x2: the game's fronts are clockwise in this view - checked on stock bodies)
  rd.FrontCounterClockwise = FALSE;
  g_dev->CreateRasterizerState(&rd, &g_raster_cull);
  rd.CullMode = D3D11_CULL_FRONT;
  g_dev->CreateRasterizerState(&rd, &g_raster_cull_front);
  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  g_dev->CreateDepthStencilState(&dd, &g_depth);
  const uint32_t white = 0xFFB0B0B0u;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = 1;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA srd = {&white, 4, 0};
  ID3D11Texture2D* tex = nullptr;
  g_dev->CreateTexture2D(&td, &srd, &tex);
  g_dev->CreateShaderResourceView(tex, nullptr, &g_white);
  tex->Release();
  {  // the floor: canvas grey, a line every metre (8 cells), the middle lines darker, darker towards the edge
    constexpr int N = 512, cell = N / 8;
    std::vector<uint32_t> px(size_t(N) * N);
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x) {
        const float dx = (x + 0.5f) / N - 0.5f, dy = (y + 0.5f) / N - 0.5f;
        const float edge = std::clamp(1.0f - (std::max(std::fabs(dx), std::fabs(dy)) - 0.30f) * 3.0f, 0.55f, 1.0f);
        float v = 0.74f;
        const bool line = x % cell < 2 || y % cell < 2, mid = std::abs(x - N / 2) < 2 || std::abs(y - N / 2) < 2;
        if (line) v = 0.60f;
        if (mid) v = 0.50f;
        v *= edge;
        const uint32_t c = uint32_t(std::clamp(v, 0.0f, 1.0f) * 255);
        px[size_t(y) * N + x] = 0xFF000000u | uint32_t(std::min(255u, c + 6)) << 16 | c << 8 | c;
      }
    D3D11_TEXTURE2D_DESC fd = td;
    fd.Width = fd.Height = N;
    D3D11_SUBRESOURCE_DATA fs = {px.data(), N * 4, 0};
    ID3D11Texture2D* ft = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&fd, &fs, &ft))) g_dev->CreateShaderResourceView(ft, nullptr, &g_floor_tex), ft->Release();
    D3D11_BUFFER_DESC vd = {};
    vd.ByteWidth = 4 * sizeof(GpuVertex);
    vd.Usage = D3D11_USAGE_DYNAMIC;
    vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&vd, nullptr, &g_floor_vb);
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    D3D11_BUFFER_DESC id = {};
    id.ByteWidth = sizeof idx;
    id.Usage = D3D11_USAGE_IMMUTABLE;
    id.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA is = {idx, 0, 0};
    g_dev->CreateBuffer(&id, &is, &g_floor_ib);
  }
  return true;
}

void ResizeTarget(int w, int h) {
  if (w == g_rt_w && h == g_rt_h && g_rt) return;
  Release(g_rt_srv), Release(g_rtv), Release(g_rt), Release(g_dsv);
  g_rt_srv = nullptr, g_rtv = nullptr, g_rt = nullptr, g_dsv = nullptr;
  g_rt_w = w, g_rt_h = h;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = UINT(w);
  td.Height = UINT(h);
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  g_dev->CreateTexture2D(&td, nullptr, &g_rt);
  g_dev->CreateRenderTargetView(g_rt, nullptr, &g_rtv);
  g_dev->CreateShaderResourceView(g_rt, nullptr, &g_rt_srv);
  td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D* depth = nullptr;
  g_dev->CreateTexture2D(&td, nullptr, &depth);
  g_dev->CreateDepthStencilView(depth, nullptr, &g_dsv);
  depth->Release();
}

// row-major, row vectors (as the editor)
void ViewProj(float aspect, float out[16]) {
  const float dist = g_height * 1.85f * g_zoom;
  const V3 at = g_center + g_pan;
  const float cp = std::cos(g_pitch), sp = std::sin(g_pitch);
  const V3 eye = at + V3{std::sin(g_yaw) * dist * cp, dist * sp, std::cos(g_yaw) * dist * cp};
  const V3 z = Norm(at - eye), x = Norm(Cross(V3{0, 1, 0}, z)), y = Cross(z, x);
  const float view[16] = {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -Dot(x, eye), -Dot(y, eye), -Dot(z, eye), 1};
  // (x flipped: the game's view is left-handed - with the plain basis the writing on shirts read mirrored next
  // to the game's own shots; so the camera's x shows to the left on screen)
  const float zn = 0.5f, zf = dist * 4, ys = 1.0f / std::tan(0.30f), xs = -ys / aspect;
  const float proj[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += view[i * 4 + k] * proj[k * 4 + j];
      out[i * 4 + j] = s;
    }
}

float g_frame_lo[3], g_frame_hi[3];  // the bounds gathered over the slots skinned this frame
float g_height_scale = 1;            // the model's (slot 0) size from its feet
bool g_scale_at_root = false;        // (the Full preview: scaled about the root's x / z, not the origin - it walks far)
float g_pivot[2] = {0, 0};           // (that x / z, game coordinates: the last Skin's root)
float g_pivot_y = 0;                 // (and the height it's scaled from: the lowest foot bone - view y, up)
float g_pivot_fixed = std::numeric_limits<float>::quiet_NaN();  // (SetScalePivotY: that height, fixed - game y)
float g_feet = 0, g_feet_next = 1e30f, g_top_next = -1e30f, g_tall = 0;  // slot 0's lowest / highest y (view, unscaled)
// the floor under a motion (view y): its lowest bone over the whole motion, less a sole - in-ring motions
// (finishers) are keyed on the ring canvas, above the arena floor (0) the stances and taunts stand on
float g_motion_floor = 0;
bool g_has_motion_floor = false;

void Skin(Slot& sl, const std::vector<Xf>& pose) {
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  std::vector<Xf> skin(pose.size());
  for (size_t i = 0; i < pose.size(); ++i) skin[i] = Mul(pose[i], sl.bind_inv[i]);
  const bool frame = g_frame;
  const bool model = &sl == &g_slots[0];
  const float s = model ? g_height_scale : 1.0f;
  float cx = 0, cz = 0, fy = g_feet;  // (the scale's pivot: x / z, and the height in view y)
  if (model && g_scale_at_root && !pose.empty()) {
    // (the Full preview: about the root's x / z and the feet as posed now - the last frame's lowest point isn't kept
    // there, so the size went from the entrance frame's 0 and sank a big superstar 1.4 into the floor outside the
    // ring, measured against the game's bones)
    cx = pose[0].t.x, cz = pose[0].t.z, g_pivot[0] = cx, g_pivot[1] = cz;
    float low = -1e30f;
    for (size_t i = 0; i < pose.size() && i < sl.ch->bones.size(); ++i) {
      const std::string& n = sl.ch->bones[i].name;
      if (n.find("ashi") != std::string::npos || n.find("tsumasaki") != std::string::npos) low = std::max(low, pose[i].t.y);
    }
    if (low > -1e29f) fy = -low;
    if (!std::isnan(g_pivot_fixed)) fy = -g_pivot_fixed;  // (the Full preview's: one per part - a pivot that follows
    // the lowest foot each frame made the whole body jump as the feet took turns)
    g_pivot_y = fy;
  }
  for (size_t m = 0; m < sl.gpu.size(); ++m) {
    const PMesh& pm = sl.ch->meshes[m];
    ID3D11Buffer* vb = sl.gpu[m].vb;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (!vb || FAILED(g_ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
    auto* out = static_cast<GpuVertex*>(ms.pData);
    for (size_t k = 0; k < pm.pos.size(); ++k) {
      V3 p, n;
      const Influence& f = pm.inf[k];
      for (int j = 0; j < 4; ++j) {
        if (f.w[j] <= 0) continue;
        const Xf& s = skin[size_t(f.node[j])];
        p = p + Apply(s, pm.pos[k]) * f.w[j];
        n = n + Rotate(s, pm.nrm[k]) * f.w[j];
      }
      // y up for the view: y and z negated (a half turn about x); the model
      // scaled from its feet (last frame's lowest point)
      if (model) g_feet_next = std::min(g_feet_next, -p.y), g_top_next = std::max(g_top_next, -p.y);
      V3 tg{0, 0, 0};
      if (!pm.tan.empty())
        for (int j = 0; j < 4; ++j)
          if (f.w[j] > 0) tg = tg + Rotate(skin[size_t(f.node[j])], pm.tan[k]) * f.w[j];
      out[k] = {{cx + (p.x - cx) * s, fy + (-p.y - fy) * s, -(cz + (p.z - cz) * s)}, {n.x, -n.y, -n.z}, {pm.uv[k][0], pm.uv[k][1]}, {tg.x, -tg.y, -tg.z}};
      if (frame)
        for (int c = 0; c < 3; ++c) lo[c] = std::min(lo[c], out[k].pos[c]), hi[c] = std::max(hi[c], out[k].pos[c]);
    }
    g_ctx->Unmap(vb, 0);
  }
  if (frame && lo[0] <= hi[0])
    for (int c = 0; c < 3; ++c) g_frame_lo[c] = std::min(g_frame_lo[c], lo[c]), g_frame_hi[c] = std::max(g_frame_hi[c], hi[c]);
}

// ---- the basic ring (SetRing): bg29's ring kit placed as the game does (editor::RingPlacements), baked once into
// the view's space with its canvas at y 0; moved to the floor the people stand on (floor_y) when that changes
struct RingDraw {
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  UINT count = 0;
  ID3D11ShaderResourceView* tex = nullptr;
  std::vector<GpuVertex> verts;  // (canvas at y 0)
};
std::vector<RingDraw> g_ring_draws;
std::map<std::string, ID3D11ShaderResourceView*> g_ring_tex;
bool g_ring_on = false, g_ring_built = false;
std::string g_ring_pac;
float g_ring_y = 1e30f;  // (the floor the vertex buffers were last moved to)
constexpr float kRingMatY = -12.0f;  // (the canvas, game y)

}  // namespace

bool RingsideSteps(const std::string& bg_etc, std::vector<uint8_t>& model, std::vector<StepPlace>& places) {
  Bytes d;
  Epac e;
  if (!ReadFile(bg_etc, d) || !EpacRead(d, e)) return false;
  Bytes table;
  model.clear(), places.clear();
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      std::vector<PachEntry> kids;
      if (g.type != "STG " || en.name != "WPON" || !PachRead(Unpack(en.data), kids)) continue;
      for (const auto& k : kids)
        if (k.id == 45010) table = Unpack(k.data);
        else if (k.id == 32) model = Unpack(k.data);
    }
  // the chunk stream: "SubH" {s16 key, s16 records, s32 link}, "Data" {u32 chunk size, record}
  int key = -1;
  for (size_t p = 4; p + 8 <= table.size();) {
    const uint8_t* c = &table[p];
    if (!std::memcmp(c, "SubH", 4) && p + 12 <= table.size()) {
      key = int16_t(Le16(c + 4)), p += 12;
    } else if (!std::memcmp(c, "Data", 4)) {
      const uint32_t size = Le32(c + 4);
      if (size < 36 || p + size > table.size()) break;
      if (key == 1000 && c[8 + 27] == 32) {
        StepPlace s;
        for (int a = 0; a < 3; ++a) s.t[a] = LeF(c + 8 + 4 * a);
        const float yaw = LeF(c + 8 + 16) * 3.14159265f / 180.0f, cy = std::cos(yaw), sy = std::sin(yaw);
        const float r[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
        std::memcpy(s.r, r, sizeof r);
        places.push_back(s);
      }
      p += size;
    } else {
      break;
    }
  }
  return !model.empty() && !places.empty();
}

namespace {

void BuildRing() {
  g_ring_built = true;
  svrfmt::Arena a;
  std::string err;
  if (g_ring_pac.empty() || !a.Load(g_ring_pac, &err)) {
    mm::Log("basic ring: " + g_ring_pac + " didn't load (" + err + ")");
    return;
  }
  for (const auto& b : a.bundles)
    for (const auto& t : b.textures)
      if (!g_ring_tex.count(Lower(t.name))) g_ring_tex[Lower(t.name)] = MakeTexture(t.data);
  size_t parts = 0;
  for (const editor::RingPart& p : editor::RingPlacements(a)) {
    const Model& md = a.models[size_t(p.model)].model;
    const float* m = p.m;
    ++parts;
    for (const Mesh& s : md.meshes) {
      RingDraw d;
      const int slot = DiffuseSlot(s);
      const auto tex = g_ring_tex.find(slot >= 0 && slot < int(md.textures.size()) ? Lower(md.textures[size_t(slot)]) : "");
      d.tex = tex == g_ring_tex.end() ? g_white : tex->second;
      for (size_t k = 0; k < s.verts.size(); ++k) {
        const float* v = s.verts[k].pos;
        const float* n = s.verts[k].normal;
        const float x = v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12], y = v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13],
                    z = v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14];
        const float nx = n[0] * m[0] + n[1] * m[4] + n[2] * m[8], ny = n[0] * m[1] + n[1] * m[5] + n[2] * m[9],
                    nz = n[0] * m[2] + n[1] * m[6] + n[2] * m[10];
        const auto uv = k < s.uvs.size() ? s.uvs[k] : std::array<float, 2>{0, 0};
        d.verts.push_back({{x, -(y - kRingMatY), -z}, {nx, -ny, -nz}, {uv[0], uv[1]}});  // (view space: game x, -y, -z)
      }
      std::vector<uint32_t> idx;
      for (const auto& st : s.strips)
        for (const auto& t : StripToTriangles(st.indices))
          if (t[0] < d.verts.size() && t[1] < d.verts.size() && t[2] < d.verts.size()) idx.insert(idx.end(), {t[0], t[1], t[2]});
      if (idx.empty() || d.verts.empty()) continue;
      D3D11_BUFFER_DESC vd = {UINT(d.verts.size() * sizeof(GpuVertex)), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE};
      D3D11_BUFFER_DESC id = {UINT(idx.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
      D3D11_SUBRESOURCE_DATA is = {idx.data()};
      if (FAILED(g_dev->CreateBuffer(&vd, nullptr, &d.vb)) || FAILED(g_dev->CreateBuffer(&id, &is, &d.ib))) {
        if (d.vb) d.vb->Release();
        continue;
      }
      d.count = UINT(idx.size());
      g_ring_draws.push_back(std::move(d));
    }
  }
  // the steel steps at the corners (rigid: every vertex by its place, R p + t)
  std::vector<uint8_t> steps;
  std::vector<StepPlace> places;
  Character sc;
  const std::u8string etc = (fs::u8path(g_ring_pac).parent_path() / "bgEtc.pac").u8string();  // (UTF-8 paths)
  if (RingsideSteps(std::string(etc.begin(), etc.end()), steps, places) && LoadEmd(steps, sc, err)) {
    for (const auto& [name, dds] : sc.textures)
      if (!g_ring_tex.count(name)) g_ring_tex[name] = MakeTexture(dds);
    for (const StepPlace& pl : places)
      for (const PMesh& pm : sc.meshes) {
        RingDraw d;
        const auto tex = g_ring_tex.find(pm.tex);
        d.tex = tex == g_ring_tex.end() ? g_white : tex->second;
        const float* R = pl.r;
        for (size_t k = 0; k < pm.pos.size(); ++k) {
          const V3 p = pm.pos[k], n = pm.nrm[k];
          const float x = R[0] * p.x + R[1] * p.y + R[2] * p.z + pl.t[0], y = R[3] * p.x + R[4] * p.y + R[5] * p.z + pl.t[1],
                      z = R[6] * p.x + R[7] * p.y + R[8] * p.z + pl.t[2];
          const float nx = R[0] * n.x + R[1] * n.y + R[2] * n.z, ny = R[3] * n.x + R[4] * n.y + R[5] * n.z,
                      nz = R[6] * n.x + R[7] * n.y + R[8] * n.z;
          d.verts.push_back({{x, -(y - kRingMatY), -z}, {nx, -ny, -nz}, {pm.uv[k][0], pm.uv[k][1]}});
        }
        D3D11_BUFFER_DESC vd = {UINT(d.verts.size() * sizeof(GpuVertex)), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE};
        D3D11_BUFFER_DESC id = {UINT(pm.idx.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
        D3D11_SUBRESOURCE_DATA is = {pm.idx.data()};
        if (d.verts.empty() || pm.idx.empty() || FAILED(g_dev->CreateBuffer(&vd, nullptr, &d.vb)) || FAILED(g_dev->CreateBuffer(&id, &is, &d.ib))) {
          if (d.vb) d.vb->Release();
          continue;
        }
        d.count = UINT(pm.idx.size());
        g_ring_draws.push_back(std::move(d));
      }
  } else {
    mm::Log("basic ring: no steel steps (bgEtc.pac STG/WPON)");
  }
  g_ring_y = 1e30f;
  mm::Log("basic ring: " + std::to_string(parts) + " parts, " + std::to_string(places.size()) + " steps, " +
          std::to_string(g_ring_draws.size()) + " meshes from " + g_ring_pac);
}

// ---- secondary motion (hair_sim.h): the game's hair chains and body jiggle on the posed bones, at a fixed 60 Hz
bool g_physics = true;

hair::Rigid ToRigid(const Xf& x) {
  hair::Rigid r;
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b) r.r[a][b] = x.r[a][b];
  r.t = {x.t.x, x.t.y, x.t.z};
  return r;
}
hair::V ToV(V3 v) { return {v.x, v.y, v.z}; }

void BuildPhysics(Slot& sl) {
  Slot::Phys& ph = sl.phys;
  ph.built = true;
  const Character& ch = *sl.ch;
  std::map<std::string, int> idx;
  for (size_t i = 0; i < ch.bones.size(); ++i) idx.emplace(ch.bones[i].name, int(i));
  auto find = [&](const std::string& n) {
    auto it = idx.find(n);
    return it == idx.end() ? -1 : it->second;
  };
  for (const hair::ChainParams& cp : hair::ParseChains(ch.bane60)) {
    if (cp.A != 0) continue;  // (2 of the game's 570 lines: their write-back moves nothing)
    const int root = find(cp.root);
    if (root < 0 || ch.bones[size_t(root)].parent < 0) continue;
    // Only the hair (chains hanging from the head) swings here. A coat's / cloth's chains (hung from the hips or the
    // chest: Undertaker's entrance coat, an FBX jacket) are kept as posed - the game's collision keeps a coat's flare
    // around the legs, this solver doesn't yet: its panels folded in between the legs (user report 21 Oct, the Full
    // preview - "the legs flipped"). The game swings them.
    {
      bool hair = false;
      for (int b = ch.bones[size_t(root)].parent, n = 0; b >= 0 && n < 64 && !hair; b = ch.bones[size_t(b)].parent, ++n)
        hair = ch.bones[size_t(b)].name == "atama";
      if (!hair) {
        ++ph.rigid_chains;
        continue;
      }
    }
    hair::Chain c;
    c.p = cp, c.parent = ch.bones[size_t(root)].parent;
    c.n.push_back({}), c.n.back().bone = root;
    for (int k = 0; k < 64; ++k) {  // kami_bNN_00, _01, ... until one is missing
      char name[48];
      std::snprintf(name, sizeof name, "%s_%02d", cp.root.c_str(), k);
      const int b = find(name);
      if (b < 0) break;
      c.n.push_back({}), c.n.back().bone = b;
    }
    if (c.n.size() < 3) continue;
    c.L.assign(c.n.size() - 1, ch.bones[size_t(c.n.back().bone)].t[0]);  // (every segment the last node's local x)
    ph.chains.push_back(std::move(c));
  }
  for (const hair::CapsuleParams& cap : hair::ParseCapsules(ch.bane61))
    if (const int b = find(cap.bone); b >= 0) ph.caps.push_back({b, cap});
  ph.rows = hair::ParseMuscles(ch.bane50);
  static const std::pair<const char*, int> kMuscles[15] = {
      {"koshi", 0},     {"mune", 1},       {"kubi", 2},          {"l_sakotsu", 3},     {"r_sakotsu", 3},
      {"l_momo", 4},    {"r_momo", 4},     {"l_momo_x", 5},      {"r_momo_x", 5},      {"l_sakotsu_m", 6},
      {"r_sakotsu_m", 6}, {"l_ninoude_m", 7}, {"r_ninoude_m", 7}, {"l_kote_m", 8},     {"r_kote_m", 8}};
  if (ph.rows.size() >= 9)
    for (const auto& [name, row] : kMuscles)
      if (const int b = find(name); b >= 0) {
        hair::Muscle m;
        m.bone = b, m.row = row;
        ph.muscles.push_back(m);
      }
  if (!ph.chains.empty() || !ph.muscles.empty() || ph.rigid_chains)
    mm::Log("physics: " + std::to_string(ph.chains.size()) + " hair chains, " + std::to_string(ph.caps.size()) + " capsules, " +
            std::to_string(ph.muscles.size()) + " jiggle bones" +
            (ph.rigid_chains ? ", " + std::to_string(ph.rigid_chains) + " cloth chains as posed (they swing in the game)" : ""));
}

// The rotation turning +X onto unit u (the shortest arc, 826A8890: identity when nearly parallel).
void ArcFromX(hair::V u, float r[3][3]) {
  const float c = std::clamp(u.x, -1.0f, 1.0f);  // (x . u)
  const float ax = 0, ay = -u.z, az = u.y;        // x cross u
  const float s = std::sqrt(ay * ay + az * az), angle = std::acos(c);
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b) r[a][b] = a == b ? 1.0f : 0.0f;
  if (s < 1e-6f || std::fabs(angle) < 1e-4f) return;
  const float k[3] = {ax / s, ay / s, az / s}, sn = std::sin(angle), cs = std::cos(angle), t = 1 - cs;
  r[0][0] = cs + k[0] * k[0] * t, r[0][1] = k[0] * k[1] * t - k[2] * sn, r[0][2] = k[0] * k[2] * t + k[1] * sn;
  r[1][0] = k[1] * k[0] * t + k[2] * sn, r[1][1] = cs + k[1] * k[1] * t, r[1][2] = k[1] * k[2] * t - k[0] * sn;
  r[2][0] = k[2] * k[0] * t - k[1] * sn, r[2][1] = k[2] * k[1] * t + k[0] * sn, r[2][2] = cs + k[2] * k[2] * t;
}

// The bones' rest locals (for the bind pose, which has no motion).
std::vector<Xf> BindLocals(const Character& ch) {
  std::vector<Xf> l(ch.bones.size());
  for (size_t i = 0; i < l.size(); ++i) Euler(ch.bones[i].r, l[i].r), l[i].t = {ch.bones[i].t[0], ch.bones[i].t[1], ch.bones[i].t[2]};
  return l;
}

// One posed frame through the solvers: the hair bones' world transforms rewritten (8216CAE8), the jiggle bones'
// moved (their skin only: what's below them was posed already). `motion`, `key`: a new motion or a jump back
// (a loop, a restart) settles it again, as the game does at load (30 steps).
void Physics(Slot& sl, std::vector<Xf>& world, const std::vector<Xf>& local, const void* motion, float key) {
  if (!g_physics || !sl.ch || world.size() != sl.ch->bones.size() || local.size() != world.size()) return;
  Slot::Phys& ph = sl.phys;
  if (!ph.built) BuildPhysics(sl);
  if (ph.chains.empty() && ph.muscles.empty()) return;
  const float prev_key = ph.last_key;
  if (motion != ph.motion || key + 0.5f < ph.last_key) ph.settle = true;
  ph.motion = motion, ph.last_key = key;
  auto pos = [&](int b) { return ToV(world[size_t(b)].t); };
  auto negz = [&](int b) { return hair::V{-world[size_t(b)].r[0][2], -world[size_t(b)].r[1][2], -world[size_t(b)].r[2][2]}; };
  const float floor_y = g_has_motion_floor ? -g_motion_floor : 0.0f;  // (game y of what the people stand on)
  std::vector<hair::Sphere> spheres;
  for (const auto& [b, cap] : ph.caps) hair::CapsuleSpheres(cap, ToRigid(world[size_t(b)]), 1.0f, spheres);
  std::vector<hair::V> mpos(ph.muscles.size());
  for (size_t i = 0; i < ph.muscles.size(); ++i) mpos[i] = pos(ph.muscles[i].bone);
  auto step = [&] {
    for (hair::Chain& c : ph.chains) {
      const hair::Rigid rw = ToRigid(world[size_t(c.n[0].bone)]);
      hair::StepA(c, pos(c.n[0].bone), pos(c.n[1].bone), &rw);
    }
    for (hair::Chain& c : ph.chains) hair::StepB(c, negz(c.parent), negz(c.n[0].bone));
    // (no collision while the character is below the floor: the game's tests, relative to its floor -12)
    const bool skip = !ph.chains.empty() && (ph.chains[0].n[0].pos.y > floor_y + 17 || (!spheres.empty() && spheres[0].c.y > floor_y + 12));
    for (hair::Chain& c : ph.chains) hair::StepC(c, spheres, floor_y, skip, false);
    if (!ph.muscles.empty()) hair::MuscleStep(ph.muscles, ph.rows, mpos);
  };
  int steps = 0;
  if (ph.settle) {
    ph.settle = false, ph.acc = 0, steps = 30;
    for (hair::Chain& c : ph.chains) hair::Reset(c, pos(c.parent));
    for (size_t i = 0; i < ph.muscles.size(); ++i) hair::MuscleReset(ph.muscles[i], mpos[i]);
  } else {  // (the motion's clock: 2 steps a key, the game's 60 Hz - not the frame clock, whose 0 / 2 step frames
             // made the hair stutter against the body)
    ph.acc += std::clamp(double(key - prev_key) * 2.0, 0.0, 4.0);
    steps = int(ph.acc + 1e-4);
    ph.acc -= steps;
    steps = std::min(steps, 4);
  }
  for (int s = 0; s < steps; ++s) step();
  // write-back (8216CAE8, A 0): each chain bone turned from +X to the next node, its world rebuilt down the chain
  for (hair::Chain& c : ph.chains) {
    Xf W = world[size_t(c.n[0].bone)];
    float len = local[size_t(c.n[1].bone)].t.x;
    for (size_t j = 1; j + 1 < c.n.size(); ++j) {
      hair::V t = hair::Unapply(ToRigid(W), c.n[j + 1].pos);
      t.x -= len;
      const float tl = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
      Xf L = local[size_t(c.n[j].bone)];
      ArcFromX(tl > 0 ? hair::V{t.x / tl, t.y / tl, t.z / tl} : hair::V{1, 0, 0}, L.r);
      if (j >= 2) L.t = {len, 0, 0};
      const hair::V d{c.n[j + 1].pos.x - c.n[j].pos.x, c.n[j + 1].pos.y - c.n[j].pos.y, c.n[j + 1].pos.z - c.n[j].pos.z};
      W = Mul(W, L);
      world[size_t(c.n[j].bone)] = W;
      len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    }
    for (size_t j = 1; j + 1 < c.n.size(); ++j) c.n[j].pos = ToV(world[size_t(c.n[j].bone)].t);
  }
  // the jiggle: each bone's skin moved by its mass's lag (gains per axis)
  for (const hair::Muscle& m : ph.muscles) {
    const hair::V o = hair::MuscleOffset(m, ph.rows[size_t(m.row)], ToRigid(world[size_t(m.bone)]));
    Xf& w = world[size_t(m.bone)];
    w.t = {w.t.x + o.x, w.t.y + o.y, w.t.z + o.z};
  }
}

// ---- the material panel's live preview (slot 0's meshes)
std::map<std::pair<uint32_t, int>, mm::MatEdit> g_mat_preview;
std::pair<uint32_t, int> g_highlight{~0u, -1};
int g_highlight_frame = -10;  // (it lasts while the panel keeps setting it: a page without it, no glow)
std::map<std::wstring, ID3D11ShaderResourceView*> g_preview_normals;  // picture file -> texture
ID3D11ShaderResourceView* g_flat_normal = nullptr;

std::map<std::wstring, bool> g_preview_normal_alpha;  // (the picture has an alpha of its own: a skin's new shine mask)
ID3D11ShaderResourceView* PreviewNormal(const std::wstring& file) {
  auto it = g_preview_normals.find(file);
  if (it != g_preview_normals.end()) return it->second;
  Image img;
  ID3D11ShaderResourceView* v = mm::LoadPicture(file, img) ? MakeTexture(DdsEncode(img, DxtFormat::kArgb, true)) : nullptr;
  bool own = false;
  for (size_t q = 3; q < img.rgba.size() && !own; q += 4) own = img.rgba[q] != 255;
  g_preview_normal_alpha[file] = own;
  return g_preview_normals[file] = v;
}

// g with the panel's edits on it (slot 0's meshes only).
GpuMesh Previewed(const GpuMesh& g) {
  if (g_highlight == std::make_pair(g.model_id, g.mesh_index) && ImGui::GetFrameCount() - g_highlight_frame <= 2) {  // (the panel's part: a warm glow, pulsing)
    GpuMesh o = g;
    const float t = 1.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 6);
    o.tint[0] *= t * 1.2f, o.tint[1] *= t * 0.9f, o.tint[2] *= t * 0.5f;
    return o;
  }
  const auto it = g_mat_preview.find({g.model_id, g.mesh_index});
  if (it == g_mat_preview.end()) return g;
  const mm::MatEdit& e = it->second;
  GpuMesh o = g;
  if (e.has_spec) o.spec = e.spec;
  if (e.has_tint) std::memcpy(o.tint, e.tint, sizeof o.tint);
  if (e.cutout >= 0) o.cutout = e.cutout != 0;
  if (e.cutout == 1) o.skin_params = false;  // (PatchMaterials: g_fSweatSPA 0 - the colour-alpha shader, which cuts)
  if (e.two_sided >= 0) o.culled = e.two_sided == 0;
  if (e.flip == 1) o.flipped = !o.flipped;
  if (e.normal == 1) {  // (as PatchMaterials: a skin keeps its old shine mask unless the picture has an alpha)
    o.ntex = PreviewNormal(e.normal_file);
    if (g_preview_normal_alpha[e.normal_file]) o.mask = o.ntex;
  }
  if (e.normal == 2) {
    if (!g_flat_normal) {
      Image img;
      img.w = img.h = 4, img.rgba.assign(64, 0);
      for (size_t q = 0; q < 64; q += 4) img.rgba[q] = 128, img.rgba[q + 1] = 128, img.rgba[q + 2] = 255, img.rgba[q + 3] = 255;
      g_flat_normal = MakeTexture(DdsEncode(img, DxtFormat::kArgb, false));
    }
    o.ntex = g_flat_normal;
  }
  if (e.spec_alpha) o.spec_alpha = e.spec_alpha == 2 ? 0 : e.spec_alpha == 3 ? 2 : -1;
  return o;
}

// A mesh's material for the shader (null: neutral - the floor, the ring): constants, normal map at t1, culling.
void UseMaterial(Cb* c, const GpuMesh* g) {
  c->mat[0] = g ? g->spec : 0, c->mat[1] = g ? g->spec_pow : 1, c->mat[2] = g && g->ntex ? 1.0f : 0.0f;
  // the shine's mask, as the game's pixel shaders take it: 3 the mask map's alpha (the skin shaders: a culled
  // yBumpMapSS with tiling and sweat), 1 the colour's alpha (every other one - a cut-out's too), 2 its brightness, 0 none
  float mode = 0;
  if (g) mode = g->spec_alpha >= 0 ? float(g->spec_alpha) : g->skin_params && g->culled && g->mask ? 3.0f : 1.0f;
  c->mat[3] = mode;
  for (int k = 0; k < 3; ++k) c->tint[k] = g ? g->tint[k] : 1;
  c->tint[3] = 1;
}
// The shader's normals (flags.z): 0 both sides (toward the camera), 1 culled (the game's: they point in), -1 culled
// and turned over.
float NormalMode(const GpuMesh* g) { return !g || !g->culled ? 0.0f : g->flipped ? -1.0f : 1.0f; }
void BindMaterial(const GpuMesh* g) {
  ID3D11ShaderResourceView* n[2] = {g && g->ntex ? g->ntex : nullptr, g && g->mask ? g->mask : nullptr};
  g_ctx->PSSetShaderResources(1, 2, n);
  ID3D11RasterizerState* cull = g && g->flipped ? g_raster_cull_front : g_raster_cull;
  g_ctx->RSSetState(g && g->culled && cull ? cull : g_raster);
}

void Render(int w, int h) {
  ResizeTarget(w, h);
  const float clear[4] = {0.11f, 0.12f, 0.14f, g_capturing ? 0.0f : 1.0f};
  g_ctx->ClearRenderTargetView(g_rtv, clear);
  g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 1, 0);
  if (!g_slots[0].ch || !g_vs) return;
  // the clock: the chosen motion's keys (track 0's length), else the idle's
  const Motion* clock = g_slots[0].motion ? g_slots[0].motion.get() : g_slots[1].motion ? g_slots[1].motion.get() : g_idle.get();
  g_play.keys = g_chain ? int(std::ceil(g_chain->total)) : clock ? clock->keys : 0;
  if (g_play.playing && g_play.keys > 0) {
    g_play.key += ImGui::GetIO().DeltaTime * 30.0f * g_play.speed;
    if (g_play.key >= float(g_play.keys)) g_play.key = g_play.loop ? std::fmod(g_play.key, float(g_play.keys)) : float(g_play.keys - 1);
  }
  for (int c = 0; c < 3; ++c) g_frame_lo[c] = 1e30f, g_frame_hi[c] = -1e30f;
  g_feet_next = 1e30f, g_top_next = -1e30f;
  for (Slot& sl : g_slots) {
    if (!sl.ch) continue;
    sl.hidden = &sl == &g_slots[1] && g_bank && !sl.motion && !g_chain;  // (a one-person motion: no dummy in the way)
    if (sl.hidden) {
      sl.world.clear(), sl.shown = nullptr;
      continue;
    }
    const Motion* m = sl.motion ? sl.motion.get() : (&sl == &g_slots[0] || !g_bank) ? g_idle.get() : nullptr;
    float chain_key = 0;
    if (g_chain) {  // (a move: this person's place in its chain - the idle before its first track)
      m = ChainAt(*g_chain, int(&sl - g_slots), g_play.key, chain_key);
      if (!m) m = g_idle.get(), chain_key = std::fmod(g_play.key, float(std::max(1, g_idle ? g_idle->keys : 1)));
    }
    if (m) {
      const float key = g_chain ? chain_key : m == clock ? g_play.key : std::fmod(g_play.key, float(std::max(1, m->keys)));
      std::vector<Xf> locals;
      sl.world = Pose(*sl.ch, *m, key, &locals);
      sl.shown = m, sl.key = key;
      Physics(sl, sl.world, locals, m, key);
      Skin(sl, sl.world);
    } else {
      std::vector<Xf> bind(sl.bind_inv.size());
      for (size_t i = 0; i < bind.size(); ++i) bind[i] = Inverse(sl.bind_inv[i]);
      sl.world = bind, sl.shown = nullptr;
      Physics(sl, sl.world, BindLocals(*sl.ch), nullptr, 0);
      Skin(sl, sl.world);
    }
  }
  if (g_feet_next <= g_top_next) g_feet = g_feet_next, g_tall = g_top_next - g_feet_next;
  const bool fk_frame = g_frame && clock && clock != g_idle.get() && clock->keys > 1;
  if (fk_frame) {  // a motion: framed over all of it (bone positions, every few keys,
    // plus a body's half width) - the people move metres during it
    float lowest = 1e30f;
    for (const Slot& sl : g_slots) {
      if (!sl.ch) continue;
      const Motion* m = sl.motion ? sl.motion.get() : nullptr;
      if (!m) continue;
      const int step = std::max(1, m->keys / 16);
      for (int k = 0; k < m->keys; k += step) {
        const auto pose = Pose(*sl.ch, *m, float(k));
        for (size_t bi = 0; bi < pose.size(); ++bi) {
          const Xf& x = pose[bi];
          const float v[3] = {x.t.x, -x.t.y, -x.t.z};
          if (BodyBone(sl.ch->bones[bi].name)) lowest = std::min(lowest, v[1]);  // (body bones: attachments sit anywhere)
          for (int c = 0; c < 3; ++c) g_frame_lo[c] = std::min(g_frame_lo[c], v[c] - 2), g_frame_hi[c] = std::max(g_frame_hi[c], v[c] + 2);
        }
      }
    }
    if (std::getenv("MM_FEET_LOG"))  // (debug: each person's root and lowest foot, every few keys)
      for (int si = 0; si < 2; ++si) {
        const Slot& sl = g_slots[si];
        if (!sl.ch || !sl.motion) continue;
        std::string line = "feet " + std::to_string(si) + ":";
        for (int k = 0; k < sl.motion->keys; k += 4) {
          const auto w = Pose(*sl.ch, *sl.motion, float(k));
          float foot = 1e30f, root = 0;
          for (size_t i = 0; i < w.size(); ++i) {
            const std::string& n = sl.ch->bones[i].name;
            if (n == "root") root = -w[i].t.y;
            if (n.find("ashi") != std::string::npos || n.find("tsumasaki") != std::string::npos) foot = std::min(foot, -w[i].t.y);
          }
          char b[48];
          std::snprintf(b, sizeof b, " k%d r%.1f f%.1f", k, root, foot);
          line += b;
        }
        mm::Log(line);
      }
    g_has_motion_floor = lowest < 1e29f;
    g_motion_floor = lowest - 0.3f;  // (the lowest bone, a toe, sits a few cm above the sole)
    g_frame_lo[1] = std::min(g_frame_lo[1], g_motion_floor);
  } else if (g_frame) {
    g_has_motion_floor = false;
  }
  if (g_frame && g_frame_lo[0] <= g_frame_hi[0]) {  // framed as posed (the idle stands on the floor)
    const float* lo = g_frame_lo;
    const float* hi = g_frame_hi;
    g_center = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    // the whole bounding sphere in view: the camera sits at 1.85 x g_height,
    // the view's half angle is 0.30 rad (ViewProj) - one idle character
    // (20 tall, ~11 radius) comes out as before
    const V3 ext{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    g_height = std::max(1.0f, Len(ext) * 0.5f * 1.84f);
    if (fk_frame) g_height *= 0.86f;  // (the bounds hold the whole move: most of it happens nearer the middle)
    g_pan = {};
    g_frame = false;
  }
  D3D11_VIEWPORT vp = {0, 0, float(w), float(h), 0, 1};
  g_ctx->RSSetViewports(1, &vp);
  g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
  g_ctx->OMSetDepthStencilState(g_depth, 0);
  g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  g_ctx->RSSetState(g_raster);
  g_ctx->IASetInputLayout(g_layout);
  g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_ctx->VSSetShader(g_vs, nullptr, 0);
  g_ctx->PSSetShader(g_ps, nullptr, 0);
  g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->PSSetSamplers(0, 1, &g_sampler);
  float viewproj[16];
  ViewProj(float(w) / float(h), viewproj);
  // camera relative lights: the key from the camera's upper left, the fill from its lower right
  V3 fwd, right, up;
  {
    const float dist = g_height * 1.85f * g_zoom, cp = std::cos(g_pitch);
    const V3 at = g_center + g_pan, eye = at + V3{std::sin(g_yaw) * dist * cp, dist * std::sin(g_pitch), std::cos(g_yaw) * dist * cp};
    fwd = Norm(at - eye), right = Norm(Cross(V3{0, 1, 0}, fwd)), up = Cross(fwd, right);
    right = right * -1.0f;  // (the screen's right: the view is x flipped, see ViewProj)
  }
  const V3 key = Norm(fwd * -0.55f + up * 0.75f - right * 0.45f), fill = Norm(fwd * -0.6f - up * 0.15f + right * 0.75f);
  // contact shadows: under each person's hips, as strong as they are near the floor
  const float floor_y = (g_has_motion_floor && g_slots[0].motion ? g_motion_floor : g_feet * (1 - g_height_scale)) - 0.02f;
  float shadows[2][4] = {};
  for (int si = 0; si < 2; ++si) {
    const Slot& sl = g_slots[size_t(si)];
    if (!sl.ch || sl.world.size() != sl.ch->bones.size()) continue;
    float lowest = 1e30f, hx = 0, hz = 0;
    bool hips = false;
    for (size_t bi = 0; bi < sl.world.size(); ++bi) {
      const std::string& n = sl.ch->bones[bi].name;
      if (!BodyBone(n)) continue;
      const float vy = -sl.world[bi].t.y;
      lowest = std::min(lowest, vy);
      if (n == "koshi") hx = sl.world[bi].t.x, hz = -sl.world[bi].t.z, hips = true;
    }
    if (!hips) continue;
    const float above = std::max(0.0f, lowest - floor_y);
    shadows[si][0] = hx, shadows[si][1] = hz, shadows[si][2] = 7.0f;
    shadows[si][3] = std::clamp(1.0f - above / 14.0f, 0.15f, 1.0f);
  }
  auto set_cb = [&](float cutout, float floor, const GpuMesh* g = nullptr) {
    BindMaterial(g);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
    Cb* c = static_cast<Cb*>(ms.pData);
    std::memcpy(c->viewproj, viewproj, 64);
    UseMaterial(c, g);
    c->flags[0] = cutout, c->flags[1] = floor, c->flags[2] = NormalMode(g), c->flags[3] = 0;
    const float k4[4] = {key.x, key.y, key.z, 0}, f4[4] = {fill.x, fill.y, fill.z, 0}, w4[4] = {fwd.x, fwd.y, fwd.z, 0};
    std::memcpy(c->key, k4, 16), std::memcpy(c->fill, f4, 16), std::memcpy(c->fwd, w4, 16);
    std::memcpy(c->shadow, shadows, sizeof shadows);
    g_ctx->Unmap(g_cb, 0);
    return true;
  };
  // the floor: the game's (y 0), where the model's height scale puts it
  if (g_floor_vb && g_floor_tex && !g_capturing) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_floor_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
      // (a motion's own floor; else the game's, y 0, where the model's height scale puts it)
      const float y = floor_y + (g_ring_on ? kRingMatY : 0.0f), a = kFloorHalf;  // (the ring on: the arena floor, under its canvas)
      // under the people (the framed centre, on the metre grid: the lines stay put while the camera moves)
      const float cx = std::round(g_center.x / 10) * 10, cz = std::round(g_center.z / 10) * 10;
      GpuVertex* v = static_cast<GpuVertex*>(ms.pData);
      v[0] = {{cx - a, y, cz - a}, {0, 1, 0}, {0, 0}}, v[1] = {{cx + a, y, cz - a}, {0, 1, 0}, {1, 0}};
      v[2] = {{cx + a, y, cz + a}, {0, 1, 0}, {1, 1}}, v[3] = {{cx - a, y, cz + a}, {0, 1, 0}, {0, 1}};
      g_ctx->Unmap(g_floor_vb, 0);
      if (set_cb(0, 1)) {
        const UINT stride = sizeof(GpuVertex), offset = 0;
        g_ctx->IASetVertexBuffers(0, 1, &g_floor_vb, &stride, &offset);
        g_ctx->IASetIndexBuffer(g_floor_ib, DXGI_FORMAT_R32_UINT, 0);
        g_ctx->PSSetShaderResources(0, 1, &g_floor_tex);
        g_ctx->DrawIndexed(6, 0, 0);
      }
    }
  }
  if (g_ring_on && !g_capturing) {
    if (!g_ring_built) BuildRing();
    if (std::fabs(g_ring_y - floor_y) > 1e-3f) {  // (to the floor the people stand on)
      g_ring_y = floor_y;
      for (RingDraw& d : g_ring_draws) {
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(g_ctx->Map(d.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
        GpuVertex* v = static_cast<GpuVertex*>(ms.pData);
        for (size_t k = 0; k < d.verts.size(); ++k) v[k] = d.verts[k], v[k].pos[1] += floor_y;
        g_ctx->Unmap(d.vb, 0);
      }
    }
    for (const RingDraw& d : g_ring_draws) {
      if (!set_cb(0.0f, 0)) break;  // (opaque: the steps' alpha isn't a cut-out)
      const UINT stride = sizeof(GpuVertex), offset = 0;
      g_ctx->IASetVertexBuffers(0, 1, &d.vb, &stride, &offset);
      g_ctx->IASetIndexBuffer(d.ib, DXGI_FORMAT_R32_UINT, 0);
      g_ctx->PSSetShaderResources(0, 1, &d.tex);
      g_ctx->DrawIndexed(d.count, 0, 0);
    }
  }
  for (const Slot& sl : g_slots)
  if (!g_capturing || g_capture_both || &sl == &g_slots[0])
  for (const GpuMesh& g0 : sl.gpu) {
    if (sl.hidden) break;
    const GpuMesh g = &sl == &g_slots[0] && (!g_mat_preview.empty() || g_highlight.second >= 0) ? Previewed(g0) : g0;
    if (g.blood && (!g_blood || g.blood_missing)) continue;
    if (!set_cb(g.cutout && !(g.skin_params && g.culled) ? 1.0f : 0.0f, 0, &g)) continue;  // (the game's skin shaders can't cut)
    const UINT stride = sizeof(GpuVertex), offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, &g.vb, &stride, &offset);
    g_ctx->IASetIndexBuffer(g.ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11ShaderResourceView* srv = g.tex;
    if (g_colour_override && &sl == &g_slots[0] && ImGui::GetFrameCount() - g_colour_override_frame <= 2) {
      const auto ct = sl.textures.find("color");
      if (ct != sl.textures.end() && srv == ct->second) srv = g_colour_override;
    }
    if (g.blood_kind) {
      const auto bit = g_blood_srv.find(std::string(g.blood_kind == 1 ? "bloodface0_" : "bloodbody0_") + std::to_string(g_blood_stage));
      if (bit == g_blood_srv.end() || !bit->second) continue;
      srv = bit->second;
    }
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->DrawIndexed(g.count, 0, 0);
  }
  ID3D11RenderTargetView* none = nullptr;
  g_ctx->OMSetRenderTargets(1, &none, nullptr);
}

}  // namespace

bool Capture(int size, std::vector<uint8_t>& rgba, bool both) {
  if (!g_slots[0].ch || !g_dev) return false;
  g_capture_both = both;
  const bool playing = g_play.playing;
  g_play.playing = false;  // (the pose as it is now)
  g_capturing = true;
  Render(size, size);
  g_capturing = false;
  g_play.playing = playing;
  D3D11_TEXTURE2D_DESC td = {};
  g_rt->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* st = nullptr;
  if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &st))) return false;
  g_ctx->CopyResource(st, g_rt);
  D3D11_MAPPED_SUBRESOURCE ms;
  bool ok = false;
  if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) {
    rgba.resize(size_t(size) * size * 4);
    for (int y = 0; y < size; ++y) std::memcpy(&rgba[size_t(y) * size * 4], static_cast<const uint8_t*>(ms.pData) + size_t(y) * ms.RowPitch, size_t(size) * 4);
    g_ctx->Unmap(st, 0);
    ok = true;
  }
  st->Release();
  return ok;
}

// ---- the Full preview (full_preview.cpp): an entrance's tracks decoded apart from the page's motion, the model
// drawn into another view's target (the arena's) with that view's camera, in game coordinates

struct Clip {
  Motion m;
};

namespace {
// A track's data in a YMKs bank: (start, size, frames), or false.
bool TrackData(const Bytes& b, int id, int x, int y, const uint8_t** d, size_t* size, int* frames) {
  if (b.size() < 0x114 || std::memcmp(b.data(), "YMKs", 4)) return false;
  const uint32_t n = Le32(&b[0x110]);
  const size_t base = 0x114 + 16 * size_t(n);
  if (base > b.size()) return false;
  const size_t dl = b.size() - base;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = &b[0x114 + 16 * i];
    if (e[0] != y || e[1] != x || Le16(e + 2) != id) continue;
    const uint32_t off = Le32(e + 4);
    if (off >= dl) return false;
    size_t end = dl;
    for (uint32_t j = 0; j < n; ++j) {
      const uint32_t o = Le32(&b[0x114 + 16 * j + 4]);
      if (o > off && o < end) end = o;
    }
    *d = &b[base + off], *size = end - off, *frames = int(Le32(e + 8));
    return true;
  }
  return false;
}
}  // namespace

std::shared_ptr<Clip> DecodeClip(const std::vector<uint8_t>& bank, int id, int x, std::string* err) {
  auto c = std::make_shared<Clip>();
  std::string e;
  if (!DecodeYmks(bank, id, x, 0, c->m, e)) {
    if (err) *err = e;
    return nullptr;
  }
  for (int side = 0; side < 2; ++side) {  // (y3 left, y2 right)
    const uint8_t* d = nullptr;
    size_t size = 0;
    int frames = 0;
    if (TrackData(bank, id, x, side == 0 ? 3 : 2, &d, &size, &frames))
      c->m.fingers[size_t(side)] = std::make_shared<const HandPose>(ParseHandRecords(d, size, frames));
  }
  return c;
}

int ClipKeys(const Clip& c) { return c.m.keys; }
bool ClipHasFingers(const Clip& c) { return c.m.fingers[0] || c.m.fingers[1]; }

std::wstring g_test_pose_dump;  // (test aid --pose-dump-motion <YMKs bank file>,<id>,<x>,<out>)

bool ModelReady() {
  {  // (a model loaded in the background is uploaded here too: no page preview may be drawing)
    std::lock_guard lock(g_mutex);
    Slot& sl = g_slots[0];
    if (sl.pending) Upload(sl, std::move(sl.pending)), sl.pending.reset();
  }
  if (!g_test_pose_dump.empty() && g_slots[0].ch) {  // (the motion posed on the model, every key: "K key B name x y z ...")
    std::vector<std::wstring> f;
    for (size_t a = 0, b; a <= g_test_pose_dump.size(); a = b + 1) {
      b = g_test_pose_dump.find(L',', a);
      if (b == std::wstring::npos) b = g_test_pose_dump.size();
      f.push_back(g_test_pose_dump.substr(a, b - a));
    }
    g_test_pose_dump.clear();
    Bytes bank;
    std::string err;
    std::shared_ptr<Clip> clip;
    if (FILE* in = f.size() == 4 ? _wfopen(f[0].c_str(), L"rb") : nullptr) {
      std::fseek(in, 0, SEEK_END);
      bank.resize(size_t(std::ftell(in)));
      std::fseek(in, 0, SEEK_SET);
      bank.resize(std::fread(bank.data(), 1, bank.size(), in));
      std::fclose(in);
      clip = DecodeClip(bank, _wtoi(f[1].c_str()), _wtoi(f[2].c_str()), &err);
    }
    if (!clip) {
      mm::Log("pose dump: the motion didn't decode (" + err + ")");
    } else if (FILE* o = _wfopen(f[3].c_str(), L"w")) {
      const Character& ch = *g_slots[0].ch;
      for (int k = 0; k < std::max(1, clip->m.keys); ++k) {
        const auto w = Pose(ch, clip->m, float(k));
        std::fprintf(o, "K %d B", k);
        for (size_t i = 0; i < w.size(); ++i) std::fprintf(o, " %s %.4f %.4f %.4f", ch.bones[i].name.c_str(), w[i].t.x, w[i].t.y, w[i].t.z);
        std::fputc('\n', o);
      }
      std::fclose(o);
      mm::Log("pose dump: " + std::to_string(clip->m.keys) + " keys written");
    }
  }
  return g_slots[0].ch && !g_slots[0].gpu.empty() && g_vs;
}

std::vector<TraceKey> ClipTrace(const Clip& clip) {
  std::vector<TraceKey> out;
  const Slot& sl = g_slots[0];
  if (!sl.ch) return out;
  for (int k = 0; k < std::max(1, clip.m.keys); ++k) {
    const auto w = Pose(*sl.ch, clip.m, float(k));
    TraceKey t;
    for (size_t i = 0; i < w.size(); ++i) {
      const std::string& n = sl.ch->bones[i].name;
      if (n == "root") t.x = w[i].t.x, t.z = w[i].t.z;
      if (n.find("ashi") != std::string::npos || n.find("tsumasaki") != std::string::npos) t.foot = std::max(t.foot, w[i].t.y);
    }
    out.push_back(t);
  }
  return out;
}

void DrawPosed(Slot& sl, const std::vector<Xf>& pose, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w,
               int h, const float view[16], const float proj[16], const float offset[3]);

void RenderInto(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w, int h, const float view[16],
                const float proj[16], const Clip* clip, float key, const float offset[3]) {
  Slot& sl = g_slots[0];
  if (!sl.ch || !g_vs || !clip || sl.gpu.empty()) return;
  std::vector<Xf> locals;
  sl.world = Pose(*sl.ch, clip->m, key, &locals);
  Physics(sl, sl.world, locals, clip, key);
  DrawPosed(sl, sl.world, rtv, dsv, w, h, view, proj, offset);
}

// ---- props (the Full preview: an entrance's worn / held objects, evtobj.pac SOBJ models)
std::vector<std::unique_ptr<Slot>> g_props;

void SetRing(bool on, const std::string& arena_pac) {
  if (arena_pac != g_ring_pac) {  // (another game folder: built again)
    for (RingDraw& d : g_ring_draws) {
      if (d.vb) d.vb->Release();
      if (d.ib) d.ib->Release();
    }
    g_ring_draws.clear(), g_ring_built = false, g_ring_pac = arena_pac;
  }
  g_ring_on = on;
}
bool Ring() { return g_ring_on; }
void SetMaterialPreview(const std::map<std::pair<uint32_t, int>, mm::MatEdit>& edits) { g_mat_preview = edits; }
void SetColourOverride(const svrfmt::Image* img) {
  if (g_colour_override) g_colour_override->Release(), g_colour_override = nullptr;
  if (img && img->w && img->h) g_colour_override = MakeTexture(svrfmt::DdsEncode(*img, svrfmt::DxtFormat::kArgb, false));
  g_colour_override_frame = ImGui::GetFrameCount();
}
void KeepColourOverride() { g_colour_override_frame = ImGui::GetFrameCount(); }
void SetHighlight(uint32_t model_id, int mesh) { g_highlight = {model_id, mesh}, g_highlight_frame = ImGui::GetFrameCount(); }

bool MeshAtlas(uint32_t model_id, int mesh, bool normal, void** texture, std::vector<std::array<float, 2>>* uvs) {
  const Slot& sl = g_slots[0];
  if (!sl.ch) return false;
  for (const GpuMesh& g0 : sl.gpu) {
    if (g0.model_id != model_id || g0.mesh_index != mesh) continue;
    const GpuMesh g = g_mat_preview.empty() ? g0 : Previewed(g0);
    ID3D11ShaderResourceView* v = normal ? g.ntex : g.tex;
    if (texture) *texture = v;
    if (uvs && g.pm < sl.ch->meshes.size()) {
      const PMesh& pm = sl.ch->meshes[g.pm];
      uvs->clear();
      for (uint32_t i : pm.idx) uvs->push_back(i < pm.uv.size() ? pm.uv[i] : std::array<float, 2>{0, 0});
    }
    return v != nullptr;
  }
  return false;
}

void SetPhysics(bool on) {
  g_physics = on;
  for (Slot& sl : g_slots) sl.phys.settle = true;
}
bool Physics() { return g_physics; }

int AddProp(const std::vector<uint8_t>& sobj, std::string* err) {
  auto ch = std::make_shared<Character>();
  std::string e;
  if (!LoadEmd(sobj, *ch, e)) {
    if (err) *err = e;
    return -1;
  }
  std::lock_guard lock(g_mutex);
  g_props.push_back(std::make_unique<Slot>());
  g_props.back()->pending = std::move(ch);
  return int(g_props.size()) - 1;
}

void ClearProps() {
  std::lock_guard lock(g_mutex);
  for (auto& p : g_props) FreeCharacter(*p);
  g_props.clear();
}

// a posed point of the entrant (game coordinates) where it's drawn: the model's scale applied as Skin does
void ScaledPoint(float x, float y, float z, float out[3]) {
  const float s = g_height_scale, cx = g_scale_at_root ? g_pivot[0] : 0, cz = g_scale_at_root ? g_pivot[1] : 0;
  const float fy = g_scale_at_root ? g_pivot_y : g_feet;
  out[0] = cx + (x - cx) * s, out[1] = -(fy + (-y - fy) * s), out[2] = cz + (z - cz) * s;
}

bool EntrantBone(const char* name, float r[9], float t[3]) {
  const Slot& sl = g_slots[0];
  if (!sl.ch || sl.world.size() != sl.ch->bones.size()) return false;
  for (size_t i = 0; i < sl.world.size(); ++i)
    if (sl.ch->bones[i].name == name) {
      for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) r[a * 3 + b] = sl.world[i].r[a][b];
      ScaledPoint(sl.world[i].t.x, sl.world[i].t.y, sl.world[i].t.z, t);
      return true;
    }
  return false;
}

std::vector<BonePos> EntrantBones() {
  std::vector<BonePos> out;
  const Slot& sl = g_slots[0];
  if (!sl.ch || sl.world.size() != sl.ch->bones.size()) return out;
  for (size_t i = 0; i < sl.world.size(); ++i) {
    BonePos b{sl.ch->bones[i].name, {}};
    ScaledPoint(sl.world[i].t.x, sl.world[i].t.y, sl.world[i].t.z, b.t);
    for (int a = 0; a < 3; ++a)
      for (int c = 0; c < 3; ++c) b.r[a * 3 + c] = sl.world[i].r[a][c];
    out.push_back(b);
  }
  return out;
}

void RenderProp(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w, int h, const float view[16],
                const float proj[16], int prop, const float place_r[9], const float place_t[3], const PropBones& bones,
                const float offset[3]) {
  if (prop < 0 || prop >= int(g_props.size()) || !g_vs) return;
  Slot& sl = *g_props[size_t(prop)];
  if (sl.pending) Upload(sl, std::move(sl.pending)), sl.pending.reset();
  if (!sl.ch || sl.gpu.empty()) return;
  // its bones from their rests, the keyed ones turned (bone k = the key's op k), bone 0 placed by place_r / place_t
  const size_t nb = sl.ch->bones.size();
  std::vector<Xf> local(nb), world(nb);
  for (size_t i = 0; i < nb; ++i) {
    Euler(sl.ch->bones[i].r, local[i].r);
    local[i].t = {sl.ch->bones[i].t[0], sl.ch->bones[i].t[1], sl.ch->bones[i].t[2]};
    if (i < bones.size() && bones[i].on) Euler(bones[i].e, local[i].r);
  }
  Xf place;
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b) place.r[a][b] = place_r[a * 3 + b];
  place.t = {place_t[0], place_t[1], place_t[2]};
  for (size_t i = 0; i < nb; ++i) {
    const int p = sl.ch->bones[i].parent;
    world[i] = p >= 0 && size_t(p) < i ? Mul(world[size_t(p)], local[i]) : Mul(place, local[i]);
  }
  DrawPosed(sl, world, rtv, dsv, w, h, view, proj, offset);
}

void DrawPosed(Slot& sl, const std::vector<Xf>& pose, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w,
               int h, const float view[16], const float proj[16], const float offset[3]) {
  Skin(sl, pose);  // (written y up: a half turn about x - undone in the matrix)
  // the skinned (y up) space -> game space (y, z negated), moved by `offset`, then the view
  float fv[16], viewproj[16];
  std::memcpy(fv, view, sizeof fv);
  for (int j = 0; j < 4; ++j)
    fv[12 + j] += (offset ? offset[0] * view[j] + offset[1] * view[4 + j] + offset[2] * view[8 + j] : 0);
  for (int j = 0; j < 4; ++j) fv[4 + j] = -fv[4 + j], fv[8 + j] = -fv[8 + j];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float sum = 0;
      for (int k = 0; k < 4; ++k) sum += fv[i * 4 + k] * proj[k * 4 + j];
      viewproj[i * 4 + j] = sum;
    }
  // camera relative lights (as Render): the camera's axes in the skinned (y up) space
  const V3 right = Norm(V3{fv[0], fv[4], fv[8]}), up = Norm(V3{fv[1], fv[5], fv[9]}), fwd = Norm(V3{fv[2], fv[6], fv[10]});
  const V3 key_l = Norm(fwd * -0.55f + up * 0.75f - right * 0.45f), fill = Norm(fwd * -0.6f - up * 0.15f + right * 0.75f);
  D3D11_VIEWPORT vp = {0, 0, float(w), float(h), 0, 1};
  g_ctx->RSSetViewports(1, &vp);
  g_ctx->OMSetRenderTargets(1, &rtv, dsv);
  g_ctx->OMSetDepthStencilState(g_depth, 0);
  g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  g_ctx->RSSetState(g_raster);
  g_ctx->IASetInputLayout(g_layout);
  g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_ctx->VSSetShader(g_vs, nullptr, 0);
  g_ctx->PSSetShader(g_ps, nullptr, 0);
  g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->PSSetSamplers(0, 1, &g_sampler);
  for (const GpuMesh& g : sl.gpu) {
    if (g.blood) continue;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
    Cb* c = static_cast<Cb*>(ms.pData);
    std::memset(c, 0, sizeof *c);
    std::memcpy(c->viewproj, viewproj, 64);
    c->flags[0] = g.cutout && !(g.skin_params && g.culled) ? 1.0f : 0.0f;
    c->flags[2] = NormalMode(&g);
    UseMaterial(c, &g);
    const float k4[4] = {key_l.x, key_l.y, key_l.z, 0}, f4[4] = {fill.x, fill.y, fill.z, 0}, w4[4] = {fwd.x, fwd.y, fwd.z, 0};
    std::memcpy(c->key, k4, 16), std::memcpy(c->fill, f4, 16), std::memcpy(c->fwd, w4, 16);
    g_ctx->Unmap(g_cb, 0);
    BindMaterial(&g);
    const UINT stride = sizeof(GpuVertex), offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, &g.vb, &stride, &offset);
    g_ctx->IASetIndexBuffer(g.ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11ShaderResourceView* srv = g.tex;
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->DrawIndexed(g.count, 0, 0);
  }
  BindMaterial(nullptr);
  ID3D11RenderTargetView* none = nullptr;
  g_ctx->OMSetRenderTargets(1, &none, nullptr);
}

void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  g_dev = dev;
  g_ctx = ctx;
  CreatePipeline();
}

void Shutdown() {
  for (Slot& sl : g_slots) FreeCharacter(sl);
  Release(g_rt_srv), Release(g_rtv), Release(g_rt), Release(g_dsv);
  Release(g_vs), Release(g_ps), Release(g_layout), Release(g_cb), Release(g_sampler), Release(g_raster);
  Release(g_depth), Release(g_white), Release(g_floor_tex), Release(g_floor_vb), Release(g_floor_ib);
}

void LoadSlot(int which, const std::wstring& ch_pac) {
  Slot& sl = g_slots[which];
  {
    std::lock_guard lock(g_mutex);
    if (ch_pac == sl.file) return;
    sl.file = ch_pac;
    sl.pending.reset();
    if (which == 0) g_status = ch_pac.empty() ? "" : "Loading the model...";
  }
  const int gen = ++sl.generation;
  if (ch_pac.empty()) return;
  const bool need_idle = !g_idle;
  const std::wstring game = g_game;
  std::thread([which, ch_pac, game, gen, need_idle] {
    const auto t0 = std::chrono::steady_clock::now();
    auto ch = std::make_shared<Character>();
    std::string err;
    Bytes pac;
    bool ok = ReadWide(ch_pac, pac) && LoadCharacter(pac, *ch, err, which == 0 ? g_attire.load() : 1, which == 0);
    if (!ok && err.empty()) err = "could not read the file";
    std::shared_ptr<Motion> idle;
    std::string idle_err;
    if (ok) LoadHands(game);
    if (ok && need_idle) {
      idle = std::make_shared<Motion>();
      if (!LoadIdle(game, *idle, idle_err)) idle.reset();
    }
    std::lock_guard lock(g_mutex);
    Slot& sl = g_slots[which];
    if (gen != sl.generation) return;
    if (!ok) {
      if (which == 0) g_status = "The model did not load: " + err + ".";
      return;
    }
    sl.pending = ch;
    if (idle) g_idle = idle;
    mm::Log("perf: model loaded in " +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()) +
            " ms" + (idle ? " (with the idle)" : ""));
    if (which == 0) {
      char s[160];
      std::snprintf(s, sizeof s, "%zu vertices, %zu bones", ch->verts, ch->bones.size());
      g_status = s;
      if (!g_idle && !idle_err.empty()) g_status += " (no idle: " + idle_err + ")";
    }
  }).detach();
}

void SetModel(const std::wstring& ch_pac, const std::wstring& game) {
  {
    std::lock_guard lock(g_mutex);
    g_game = game;
  }
  LoadSlot(0, ch_pac);
}

void SetDummy(const std::wstring& ch_pac) { LoadSlot(1, ch_pac); }

std::string ComparePoseDump(const std::wstring& dump, std::shared_ptr<const std::vector<uint8_t>> bank, int id, int x) {
  if (!bank || !g_slots[0].ch || !g_slots[1].ch) return "";
  // the dump: per person and frame the last line (pass 1 after pass 0)
  struct Line {
    int x = 0, y = 0, mirrored = 0, players = 1;
    float time = 0;
    float A[3], E[3], R[3], Y;
    std::array<std::array<float, 12>, 33> part;
    std::array<bool, 33> has{};
  };
  std::map<int, std::map<int, Line>> by;  // person -> frame -> line
  if (FILE* f = _wfopen(dump.c_str(), L"rb")) {
    char buf[32768];
    while (std::fgets(buf, sizeof buf, f)) {
      unsigned F = 0, P = 0, M = 0, S = 0, Rq = 0;
      int used = 0;
      // (since the camera dump a line has its tick: "F <frame> T <tick> P ...")
      if (std::sscanf(buf, "F %u T %*u P %u M %u S %u Rq %u Q%n", &F, &P, &M, &S, &Rq, &used) < 5 &&
          std::sscanf(buf, "F %u P %u M %u S %u Rq %u Q%n", &F, &P, &M, &S, &Rq, &used) < 5)
        continue;
      if (int(M) != id) continue;
      Line l{};
      const char* p = buf + used;
      auto num = [&](float& v) { char* e; v = std::strtof(p, &e); p = e; };
      {  // the players: id x y mirrored time weight, the newest first
        int k = 0;
        for (;; ++k) {
          while (*p == ' ') ++p;
          if (*p == 'A' || !*p) break;
          float q[6];
          for (float& v : q) num(v);
          if (k == 0) l.x = int(q[1]), l.y = int(q[2]), l.mirrored = int(q[3]), l.time = q[4];
        }
        l.players = k;
        p = std::strchr(p, 'A');
        if (!p) continue;
        ++p;
      }
      for (float& v : l.A) num(v);
      p = std::strchr(p, 'E') + 1;
      for (float& v : l.E) num(v);
      p = std::strchr(p, 'R') + 1;
      for (float& v : l.R) num(v);
      p = std::strchr(p, 'Y') + 1;
      num(l.Y);
      p = std::strchr(p, 'B') + 1;
      for (int i = 0; i < 33; ++i) {
        while (*p == ' ') ++p;
        if (*p == '-' && (p[1] == ' ' || p[1] == '\n' || p[1] == '\r' || p[1] == 0)) { ++p; continue; }  // (none)
        for (float& v : l.part[size_t(i)]) num(v);
        l.has[size_t(i)] = true;
      }
      by[int(P)][int(F)] = l;
    }
    std::fclose(f);
  }
  if (by.empty()) return "no lines for motion " + std::to_string(id) + " in the dump";
  static const char* kPartNames[33] = {"root", "koshi", "mune", "kubi", "atama", "l_sakotsu", "l_ninoude", "l_kote", "l_te",
                                       "r_sakotsu", "r_ninoude", "r_kote", "r_te", "l_momo", "l_sune", "l_ashi", "l_tsumasaki",
                                       "r_momo", "r_sune", "r_ashi", "r_tsumasaki", "", "", "", "", "", "", "l_kote_x", "r_kote_x",
                                       "", "", "l_kote_x2", "r_kote_x2"};
  std::string out;
  char b[256];
  int track = 0;
  std::map<int, std::vector<std::pair<float, float>>> root_of;  // track -> (frame, game root height / viewer)
  (void)x;
  for (const auto& [person, frames] : by) {
    const Slot& sl = g_slots[person == 0 ? 0 : 1];  // (person 0 on the model, the other on the dummy)
    const Line& first = frames.begin()->second;
    Motion m;
    std::string err;
    if (!DecodeMotion(*bank, id, first.x, first.y, m, err)) {
      out += "person " + std::to_string(person) + " (x " + std::to_string(first.x) + " y " + std::to_string(first.y) + "): " + err + "\n";
      ++track;
      continue;
    }
    int blended = 0, mirrored = 0;
    std::map<std::string, int> idx;
    for (size_t i = 0; i < sl.ch->bones.size(); ++i) idx.emplace(sl.ch->bones[i].name, int(i));
    const int f0 = frames.begin()->first;
    double sum[33] = {}, mx[33] = {};
    int cnt[33] = {};
    double fit_sum[4] = {}, fit_d[4] = {}, fit_bg[4] = {}, fit_bv[4] = {};
    int fit_n[4] = {};
    std::snprintf(b, sizeof b, "person %d = track %d: frames %d..%d (%zu lines), motion keys %d\n", person, track, f0,
                  frames.rbegin()->first, frames.size(), m.keys);
    out += b;
    for (const auto& [F, l] : frames) {
      const float key = std::clamp(l.time * 0.5f, 0.0f, float(m.keys - 1));
      if (l.players > 1) ++blended;  // (layered: other players drive some parts too - counted, kept)
      if (l.mirrored) ++mirrored;
      std::vector<Xf> loc;
      Pose(*sl.ch, m, key, &loc);
      {  // the game's bend from its lower bones (kote 7 / 11, sune 14 / 18: RotY(bend), row form)
        static const int kLower[4] = {7, 11, 14, 18};
        for (int k = 0; k < 4; ++k) {
          if (!l.has[size_t(kLower[k])] || g_ik_last[k].L <= 0) continue;
          const auto& g = l.part[size_t(kLower[k])];
          // row form RotY rows (c,0,-s)(0,1,0)(s,0,c): s = g[6], c = g[0]
          const float bg = std::atan2(g[6], g[0]);
          const float half = std::fabs(bg) * 0.5f;
          if (half < 0.05f) continue;
          fit_sum[k] += g_ik_last[k].d / std::cos(half) / g_ik_last[k].L;
          fit_d[k] += g_ik_last[k].d, fit_bg[k] += bg, fit_bv[k] += g_ik_last[k].bend, ++fit_n[k];
        }
      }
      for (int i = 0; i < 33; ++i) {
        if (!l.has[size_t(i)] || !kPartNames[i][0]) continue;
        const auto it = idx.find(kPartNames[i]);
        if (it == idx.end()) continue;
        const auto& g = l.part[size_t(i)];
        const Xf& v = loc[size_t(it->second)];
        // game rows (row-vector form) -> column form: C[r][c] = G[c][r]; trace(V^T C) = sum V[k][c] * C[k][c]
        double tr = 0;
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 3; ++c) tr += double(v.r[r][c]) * double(g[size_t(c * 3 + r)]);
        const double ang = std::acos(std::clamp((tr - 1) / 2, -1.0, 1.0)) * 57.29578;
        sum[i] += ang, mx[i] = std::max(mx[i], ang), ++cnt[i];
      }
      if ((F - f0) % 20 == 0) {
        std::snprintf(b, sizeof b, "  F%d key %.1f: fighter %.2f %.2f %.2f yaw %.3f | pose root %.2f %.2f %.2f | game root h %.2f view root %.2f %.2f %.2f\n",
                      F, key, l.A[0], l.A[1], l.A[2], l.E[1], l.R[0], l.R[1], l.R[2], l.part[0][10], loc[size_t(idx["root"])].t.x,
                      loc[size_t(idx["root"])].t.y, loc[size_t(idx["root"])].t.z);
        out += b;
      }
    }
    std::snprintf(b, sizeof b, "  x %d y %d; %d frames layered with other players, %d mirrored\n", first.x, first.y, blended, mirrored);
    out += b;
    static const char* kLimb[4] = {"l arm", "r arm", "l leg", "r leg"};
    for (int k = 0; k < 4; ++k)
      if (fit_n[k]) {
        std::snprintf(b, sizeof b, "  IK %s: L needed / this view's L = %.3f (d %.2f, L %.2f; bend game %.1f view %.1f deg, %d frames)\n",
                      kLimb[k], fit_sum[k] / fit_n[k], fit_d[k] / fit_n[k], g_ik_last[k].L, fit_bg[k] / fit_n[k] * 57.2958,
                      fit_bv[k] / fit_n[k] * 57.2958, fit_n[k]);
        out += b;
      }
    for (int i = 0; i < 33; ++i)
      if (cnt[i]) {
        std::snprintf(b, sizeof b, "  %-12s mean %6.1f max %6.1f deg\n", kPartNames[i], sum[i] / cnt[i], mx[i]);
        out += b;
      }
    ++track;
  }
  return out;
}

void SetAttire(int attire) {
  attire = std::clamp(attire, 1, 4);
  if (g_attire.exchange(attire) == attire) return;
  std::wstring file;
  {
    std::lock_guard lock(g_mutex);
    file = g_slots[0].file;
    g_slots[0].file.clear();  // (LoadSlot reloads only a new file)
  }
  if (!file.empty()) LoadSlot(0, file);
}
int Attire() { return g_attire.load(); }
void SetGear(bool on) {
  if (g_gear.exchange(on) == on) return;
  std::wstring file;
  {
    std::lock_guard lock(g_mutex);
    file = g_slots[0].file;
    g_slots[0].file.clear();
  }
  if (!file.empty()) LoadSlot(0, file);
}
bool Gear() { return g_gear.load(); }
bool HasGear() { return g_gear_have.load(); }

bool SetMotion(std::shared_ptr<const Bytes> bank, int id, int x, std::string* err) {
  std::lock_guard lock(g_mutex);
  g_chain.reset();
  g_bank = bank;
  g_slots[0].motion.reset();
  g_slots[1].motion.reset();
  g_play.key = 0;
  g_play.id = bank ? id : -1;
  g_play.x = x;
  if (!bank) return true;
  std::string e0, e1;
  auto m0 = std::make_shared<Motion>(), m1 = std::make_shared<Motion>();
  const bool ok0 = DecodeMotion(*bank, id, x, 0, *m0, e0), ok1 = DecodeMotion(*bank, id, x, 1, *m1, e1);
  if (ok0) g_slots[0].motion = m0;
  if (ok1) g_slots[1].motion = m1;
  if (!ok0 && !ok1) {
    if (err) *err = e0;
    g_bank.reset();
    g_play.id = -1;
    return false;
  }
  g_frame = true;
  return true;
}

void ClearMotion() { SetMotion(nullptr, -1, 0); }

std::vector<int> MovePhases(const Bytes& bank, int id) {
  std::vector<int> xs;
  if (bank.size() < 0x114 || (std::memcmp(bank.data(), "YMKs", 4) && std::memcmp(bank.data(), "YMBs", 4))) return xs;
  const uint32_t n = Le32(&bank[0x110]);
  for (uint32_t i = 0; i < n && 0x114 + 16 * size_t(i + 1) <= bank.size(); ++i) {
    const uint8_t* e = &bank[0x114 + 16 * i];
    if (Le16(e + 2) == id && e[0] <= 1 && std::find(xs.begin(), xs.end(), int(e[1])) == xs.end()) xs.push_back(e[1]);
  }
  std::sort(xs.begin(), xs.end());
  return xs;
}

bool SetMove(std::shared_ptr<const Bytes> bank, int id, int alt, const MotionLoader& load, std::string* err) {
  if (!bank) return SetMotion(nullptr, -1, 0, err);
  auto c = std::make_shared<Chain>();
  std::vector<int> phases;
  for (int x : MovePhases(*bank, id))
    if (x < 100) phases.push_back(x);  // (x 100 and up: other endings - a miss, a reversal - played when picked)
  if (phases.empty()) phases = MovePhases(*bank, id);
  if (alt >= 0 && std::find(phases.begin(), phases.end(), alt) == phases.end()) phases.push_back(alt);
  if (phases.empty()) {
    if (err) *err = "the move has no motions in that bank";
    return false;
  }
  std::string first_err;
  float t = 0;
  for (int x : phases) {
    std::shared_ptr<Motion> m[2];
    float len = 0;
    for (int s = 0; s < 2; ++s) {
      auto mm = std::make_shared<Motion>();
      std::string e;
      if (DecodeMotion(*bank, id, x, s, *mm, e)) m[s] = mm, len = std::max(len, float(mm->keys));
      else if (first_err.empty()) first_err = e;
    }
    if (len <= 0) continue;
    for (int s = 0; s < 2; ++s) c->segs[s].push_back({m[s], t, len, id, x});
    c->steps += (c->steps.empty() ? "" : " > ") + std::string("x ") + std::to_string(x) + (m[1] ? " (both)" : " (one)");
    t += len;
  }
  if (t <= 0) {
    if (err) *err = first_err.empty() ? std::string("the move's tracks didn't read") : first_err;
    return false;
  }
  // each person's follow-up: the motion its last track names (op 49), from this bank or the loader's
  float total = t;
  for (int s = 0; s < 2; ++s) {
    const ChainSeg* last = nullptr;
    for (const ChainSeg& g : c->segs[s])
      if (g.m) last = &g;
    if (!last || last->m->next <= 0) continue;
    const int nid = last->m->next;
    std::shared_ptr<const Bytes> nb = MovePhases(*bank, nid).empty() && load ? load(nid) : bank;
    if (!nb) continue;
    const std::vector<int> nx = MovePhases(*nb, nid);
    auto mm = std::make_shared<Motion>();
    std::string e;
    if (nx.empty() || !DecodeMotion(*nb, nid, nx.front(), 0, *mm, e)) continue;
    const float start = last->start + float(last->m->keys);
    c->segs[s].push_back({mm, start, float(mm->keys), nid, nx.front()});
    total = std::max(total, start + float(mm->keys));
    c->steps += std::string(s ? "; the partner" : "; the superstar") + " then " + std::to_string(nid);
  }
  c->total = total + 15;  // (half a second held at the end before it plays again)
  std::lock_guard lock(g_mutex);
  g_bank = bank;
  g_slots[0].motion = c->segs[0].empty() ? nullptr : c->segs[0].front().m;
  g_slots[1].motion = c->segs[1].empty() ? nullptr : c->segs[1].front().m;
  g_chain = c;
  g_play.key = 0;
  g_play.id = id;
  g_play.x = phases.front();
  g_frame = true;
  return true;
}

TrackFacts InspectTrack(const std::vector<uint8_t>& bank, int id, int x, int y) {
  TrackFacts f;
  Motion m;
  f.ok = DecodeMotion(bank, id, x, y, m, f.err);
  if (!f.ok) return f;
  f.keys = m.keys, f.next = m.next, f.attack = m.attack, f.ops = m.ops;
  return f;
}

std::string MoveSteps() {
  std::lock_guard lock(g_mutex);
  return g_chain ? g_chain->steps : std::string();
}

void SetBlood(bool on) { g_blood = on; }
bool Blood() { return g_blood; }

void SetBloodStage(int stage) { g_blood_stage = std::clamp(stage, 0, 2); }
int BloodStage() { return g_blood_stage; }

bool HasBlood() {
  for (const Slot& sl : g_slots)
    for (const GpuMesh& g : sl.gpu)
      if (g.blood && !g.blood_missing) return true;
  return false;
}

Playback& Play() { return g_play; }

void ResetCamera() {
  g_yaw = 0.35f, g_pitch = 0.22f, g_zoom = 1;
  g_pan = {};
  g_frame = true;
}

// ---- hit volumes (HIT_VOLUMES.md): spheres on bones, the same for every character; centre = the
// bone's world matrix x (offset, 0, 0) - one exception on z; model units
struct HitSphere {
  const char* bone;
  float r, x, z;
  int group;  // attack groups: 1 head, 2 body, 4 / 8 left / right arm, 16 / 32 left / right leg, 64 special
};
const HitSphere kTargets[] = {
    {"atama", 1.7f, 1, 0, 0},    {"mune", 2.5f, 2, 0, 0},     {"koshi", 2.5f, 0, 0, 0},
    {"l_ninoude", 1.2f, 1, 0, 0}, {"l_kote", 1.2f, 0, 0, 0},  {"l_kote", 1.2f, 2, 0, 0},
    {"r_ninoude", 1.2f, 1, 0, 0}, {"r_kote", 1.2f, 0, 0, 0},  {"r_kote", 1.2f, 2, 0, 0},
    {"l_momo", 1.7f, 1.7f, 0, 0}, {"l_sune", 1.7f, 0, 0, 0},  {"l_sune", 1.7f, 2.7f, 0, 0},
    {"r_momo", 1.7f, 1.7f, 0, 0}, {"r_sune", 1.7f, 0, 0, 0},  {"r_sune", 1.7f, 2.7f, 0, 0},
};
const HitSphere kAttacks[] = {
    {"atama", 1.8f, 1, 0, 1},       {"mune", 3.2f, 2, 0, 2},       {"koshi", 3.0f, 0, 0, 2},
    {"l_ninoude", 1.5f, 1.2f, 0, 4}, {"l_kote", 1.5f, 0, 0, 4},    {"l_te", 1.5f, 0, 0, 4},
    {"r_ninoude", 1.5f, 1.2f, 0, 8}, {"r_kote", 1.5f, 0, 0, 8},    {"r_te", 1.5f, 0, 0, 8},
    {"l_momo", 2.0f, 2, 0, 16},      {"l_sune", 2.0f, 0, 0, 16},   {"l_ashi", 1.8f, 0, 0, 16},
    {"r_momo", 2.0f, 2, 0, 32},      {"r_sune", 2.0f, 0, 0, 32},   {"r_ashi", 1.8f, 0, 0, 32},
    {"l_kote", 3.0f, 0, -3, 64},
};
bool g_hits = false;

int AttackMask(const Motion* m, float key) {
  int mask = 0;
  if (m)
    for (const auto& [frame, value] : m->attack)
      if (float(frame) <= key) mask = value;
  return mask;
}

void DrawHits(ImVec2 origin, float w, float h) {
  float vp[16];
  ViewProj(w / h, vp);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(origin, ImVec2(origin.x + w, origin.y + h), true);
  // the camera's right (a sphere's screen radius)
  const float dist = g_height * 1.85f * g_zoom, cp = std::cos(g_pitch);
  const V3 at = g_center + g_pan, eye = at + V3{std::sin(g_yaw) * dist * cp, dist * std::sin(g_pitch), std::cos(g_yaw) * dist * cp};
  const V3 right = Norm(Cross(V3{0, 1, 0}, Norm(at - eye)));
  auto project = [&](V3 v, ImVec2& out) {
    const float cx = v.x * vp[0] + v.y * vp[4] + v.z * vp[8] + vp[12], cy = v.x * vp[1] + v.y * vp[5] + v.z * vp[9] + vp[13];
    const float cw = v.x * vp[3] + v.y * vp[7] + v.z * vp[11] + vp[15];
    if (cw <= 0.01f) return false;
    out = ImVec2(origin.x + (cx / cw * 0.5f + 0.5f) * w, origin.y + (0.5f - cy / cw * 0.5f) * h);
    return true;
  };
  for (int si = 0; si < 2; ++si) {
    const Slot& sl = g_slots[si];
    if (!sl.ch || sl.world.size() != sl.ch->bones.size()) continue;
    const float sc = si == 0 ? g_height_scale : 1.0f;
    auto sphere = [&](const HitSphere& hs, ImU32 col) {
      int bi = -1;
      for (size_t i = 0; i < sl.ch->bones.size(); ++i)
        if (sl.ch->bones[i].name == hs.bone) { bi = int(i); break; }
      if (bi < 0) return;
      const V3 c = Apply(sl.world[size_t(bi)], V3{hs.x, 0, hs.z});
      const V3 v{c.x * sc, g_feet + (-c.y - g_feet) * sc, -c.z * sc};  // (the view's y up, as Skin)
      ImVec2 a, b;
      if (!project(v, a) || !project(v + right * (hs.r * sc), b)) return;
      const float rp = std::max(2.0f, std::hypot(b.x - a.x, b.y - a.y));
      dl->AddCircle(a, rp, col, 20, 1.5f);
    };
    for (const auto& t : kTargets) sphere(t, IM_COL32(60, 220, 110, 200));
    const int mask = AttackMask(sl.shown, sl.key);
    for (const auto& a : kAttacks)
      if (mask & a.group) sphere(a, IM_COL32(255, 70, 70, 230));
  }
  dl->PopClipRect();
}

void SetHits(bool on) { g_hits = on; }
bool Hits() { return g_hits; }

void Draw(float w, float h) {
  {
    std::lock_guard lock(g_mutex);
    for (Slot& sl : g_slots) {
      if (sl.pending) Upload(sl, std::move(sl.pending)), sl.pending.reset();
      if (sl.file.empty() && sl.ch) FreeCharacter(sl);
    }
  }
  if (!g_slots[0].ch) {
    ImGui::Dummy(ImVec2(w, h));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(a, b, IM_COL32(28, 31, 36, 255));
    const std::string s = Status().empty() ? "Pick a model to see it here." : Status();
    const ImVec2 ts = ImGui::CalcTextSize(s.c_str(), nullptr, false, w - 16);
    ImGui::GetWindowDrawList()->AddText(nullptr, 0, ImVec2(a.x + (w - ts.x) / 2, a.y + (h - ts.y) / 2),
                                        IM_COL32(170, 170, 170, 255), s.c_str(), nullptr, w - 16);
    return;
  }
  Render(std::max(16, int(w)), std::max(16, int(h)));
  ImGui::Image(ImTextureID(reinterpret_cast<uintptr_t>(g_rt_srv)), ImVec2(w, h));
  if (g_hits) DrawHits(ImGui::GetItemRectMin(), w, h);
  if (ImGui::IsItemHovered()) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_yaw += io.MouseDelta.x * 0.01f;  // (+: the view is x flipped)
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
      g_yaw += io.MouseDelta.x * 0.01f;
      g_pitch = std::clamp(g_pitch + io.MouseDelta.y * 0.01f, -0.6f, 1.2f);
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      // pan in the view plane
      const float k = g_height * g_zoom * 0.0025f;
      const V3 right{std::cos(g_yaw), 0, -std::sin(g_yaw)};
      g_pan = g_pan + right * (io.MouseDelta.x * k) + V3{0, io.MouseDelta.y * k, 0};
    }
    if (io.MouseWheel != 0) g_zoom = std::clamp(g_zoom * (io.MouseWheel > 0 ? 0.88f : 1.14f), 0.25f, 4.0f);
    const ImVec2 a = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + 8, a.y + 6), IM_COL32(200, 200, 200, 120),
                                        "Left drag: turn   Right drag: tilt   Middle drag: pan   Wheel: zoom");
  }
}

void SetHeight(float scale) { g_height_scale = std::clamp(scale, 0.5f, 2.0f); }
float Height() { return g_height_scale; }
void SetScaleAtRoot(bool on) { g_scale_at_root = on; }
void SetScalePivotY(float game_y) { g_pivot_fixed = game_y; }
float ModelHeight() { return g_slots[0].ch ? g_tall : 0.0f; }

std::string Status() {
  std::lock_guard lock(g_mutex);
  if (!g_slots[0].ch || g_status.find(" bones") == std::string::npos) return g_status;
  // (what plays now, not what played when the model loaded)
  if (g_bank && g_play.id >= 0) return g_status + ", motion " + std::to_string(g_play.id) + (g_play.x ? " variant " + std::to_string(g_play.x) : "");
  return g_status + (g_idle ? ", the idle stance" : ", the bind pose");
}

}  // namespace char_preview
