#include "arena_full.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#include "fbx_write.h"
#include "png.h"

namespace fs = std::filesystem;

namespace svrfmt {

namespace {

std::string P8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

constexpr double kPi = 3.14159265358979323846;

// A key of a YMKs bank: 0x7530's lights (position, rotation, colour), 0x7531's motions (position, rotation, scale).
struct Key {
  float v[9] = {};
};
using Bank = std::map<std::pair<int, int>, Key>;  // (id, channel) -> its first key

float LeF32(const uint8_t* p) {
  float f;
  std::memcpy(&f, p, 4);
  return f;
}
uint32_t LeU32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t LeU16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// The bank's tracks, each track's first key (as the 3D editor reads them: editor.cpp ReadStageBank).
Bank ReadBank(const Bytes& raw, bool motion) {
  Bank out;
  size_t p = raw.size() >= 16 && !std::memcmp(raw.data(), "YMKs", 4) ? 16 : 0;
  if (raw.size() < p + 0x104) return out;
  const uint32_t n = LeU32(raw.data() + p + 0x100);
  const size_t dir = p + 0x104, base = dir + 16 * size_t(n);
  if (n > 4096 || base > raw.size()) return out;
  const size_t pose = motion ? 29 : 25;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* d = raw.data() + dir + 16 * i;
    const int y = d[0], id = LeU16(d + 2);
    const int frames = int(std::max<uint32_t>(1, LeU32(d + 8)));
    size_t q = base + LeU32(d + 4);
    for (int f = 0; q < raw.size() && f < frames;) {
      const uint8_t b = raw[q];
      if (b < 0x60) {
        f += b, ++q;
        continue;
      }
      size_t sz = b - 0x60;
      if (b == 0x60) {
        if (q + 3 > raw.size()) break;
        sz = LeU16(raw.data() + q + 1), q += 3;
      } else {
        ++q;
      }
      if (q + sz > raw.size()) break;
      const uint8_t* r = raw.data() + q;
      size_t rs = sz;
      if (rs && (r[0] & 0x80)) {  // (an event block first)
        const size_t ev = 1 + (r[0] & 0x7f);
        r += std::min(ev, rs), rs -= std::min(ev, rs);
      }
      if (rs >= pose) {
        Key k;
        for (int a = 0; a < 3; ++a) k.v[a] = LeF32(r + 1 + 4 * a) * 0.01f;
        if (motion) {
          for (int a = 0; a < 3; ++a) k.v[3 + a] = LeF32(r + 13 + 4 * a);
          k.v[6] = LeF32(r + 25);
        } else {
          for (int a = 0; a < 3; ++a) k.v[3 + a] = float(int16_t(LeU16(r + 13 + 2 * a)) * (2 * kPi / 65536.0));
          for (int a = 0; a < 3; ++a) k.v[6 + a] = LeU16(r + 19 + 2 * a) / 4096.0f;
        }
        out[{id, y}] = k;
        break;
      }
      q += sz, ++f;
    }
  }
  return out;
}

// The flag table (entry 50001: "<model id> <flags>"), each line " <flags> ".
std::map<uint32_t, std::string> FlagsOf(const Arena& a) {
  std::map<uint32_t, std::string> flags;
  for (const auto& e : a.entries) {
    if (e.id != 50001) continue;
    const Bytes t = Unpack(e.data);
    const std::string text(t.begin(), t.end());
    for (size_t pos = 0; pos < text.size();) {
      size_t nl = text.find('\n', pos);
      const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      pos = nl == std::string::npos ? text.size() : nl + 1;
      if (line.empty() || !std::isdigit(uint8_t(line[0]))) continue;
      const size_t f = line.find_first_not_of("0123456789");
      flags[uint32_t(std::atoi(line.c_str()))] = " " + (f == std::string::npos ? std::string() : line.substr(f)) + " ";
    }
  }
  return flags;
}

bool Word(const std::string& flags, const char* w) {
  for (size_t at = flags.find(w); at != std::string::npos; at = flags.find(w, at + 1))
    if (at > 0 && std::isspace(uint8_t(flags[at - 1])) && at + 1 < flags.size() && std::isspace(uint8_t(flags[at + 1]))) return true;
  return false;
}

// A place: row vectors, p' = p * r + t (scale, rotation x then y then z, the move).
struct Place {
  double r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  double t[3] = {0, 0, 0};
};
Place PlaceOf(const Key& k) {
  const double s = k.v[6] != 0 ? k.v[6] : 1.0;
  const double cx = std::cos(k.v[3]), sx = std::sin(k.v[3]), cy = std::cos(k.v[4]), sy = std::sin(k.v[4]), cz = std::cos(k.v[5]),
               sz = std::sin(k.v[5]);
  const double rx[3][3] = {{1, 0, 0}, {0, cx, sx}, {0, -sx, cx}}, ry[3][3] = {{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}},
               rz[3][3] = {{cz, sz, 0}, {-sz, cz, 0}, {0, 0, 1}};
  double a[3][3], b[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      a[i][j] = 0;
      for (int m = 0; m < 3; ++m) a[i][j] += rx[i][m] * ry[m][j];
    }
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      b[i][j] = 0;
      for (int m = 0; m < 3; ++m) b[i][j] += a[i][m] * rz[m][j];
    }
  Place p;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) p.r[i][j] = b[i][j] * s;
    p.t[i] = k.v[i];
  }
  return p;
}

// Yaw about the vertical, then a move (as the 3D editor's Place).
Place YawAt(double x, double y, double z, double yaw) {
  Place p;
  const double c = std::cos(yaw), s = std::sin(yaw);
  p.r[0][0] = c, p.r[0][2] = -s, p.r[2][0] = s, p.r[2][2] = c;
  p.t[0] = x, p.t[1] = y, p.t[2] = z;
  return p;
}

// The ring's parts: the game draws them from one stored model each, by code; the same rules as the 3D editor's
// (editor.cpp RoleOf and its draw): ropes three times a side, a pad at each rope's height, each corner's three
// turnbuckle links, the apron's panels on two sides each. out: the places, empty when the game doesn't draw the model.
// Returns false for a model that isn't a ring part.
bool RingPlaces(uint32_t id, bool per_rope, std::vector<Place>& out, std::vector<std::string>& names) {
  constexpr double kMatY = -12, kBase = 3.4, kGap = 4.2, kRopeOut = 28.5, kApronOut = 31.9, kHalfPi = kPi / 2;
  const auto rope_y = [&](int r) { return kMatY - kBase - r * kGap; };
  const auto rope_at = [&](uint32_t side, int r) {
    double x = 0, z = 0;
    if (side == 960) z = kRopeOut;
    else if (side == 961) x = -kRopeOut;
    else if (side == 962) z = -kRopeOut;
    else x = kRopeOut;
    out.push_back(YawAt(x, rope_y(r), z, 0));
    names.push_back("_rope" + std::to_string(r + 1));
  };
  if (id >= 960 && id <= 963) {
    if (!per_rope)
      for (int r = 0; r < 3; ++r) rope_at(id, r);
    return true;
  }
  if (id >= 900 && id <= 911) {
    static const uint32_t kSide[4] = {962, 961, 960, 963};
    rope_at(kSide[(id - 900) % 4], int(id - 900) / 4);
    return true;
  }
  if (id >= 964 && id <= 967) {
    for (int r = 0; r < 3; ++r) {
      out.push_back(YawAt(0, rope_y(r), 0, 0));
      names.push_back("_rope" + std::to_string(r + 1));
    }
    return true;
  }
  if (id == 952) {
    const double post = -32, d = -kPi / 4, cd = std::cos(d), sd = std::sin(d);
    const double ox = post - (post * cd + post * sd), oz = post - (-post * sd + post * cd);
    for (int corner = 0; corner < 4; ++corner) {
      const double a = corner * kHalfPi, ca = std::cos(a), sa = std::sin(a);
      for (int r = 0; r < 3; ++r) {
        out.push_back(YawAt(ox * ca + oz * sa, rope_y(r), -ox * sa + oz * ca, a + d));
        names.push_back("_corner" + std::to_string(corner + 1) + "_rope" + std::to_string(r + 1));
      }
    }
    return true;
  }
  if (id == 973 || id == 974) {
    for (int side = 0; side < 2; ++side) {
      const double yaw = (id == 974 ? 0 : kHalfPi) + side * kPi;
      out.push_back(YawAt(-kApronOut * std::sin(yaw), 0, -kApronOut * std::cos(yaw), yaw));
      names.push_back("_side" + std::to_string(side + 1));
    }
    return true;
  }
  return (id >= 956 && id <= 959) || id == 40200;  // (the game doesn't draw these)
}

std::string JsonEsc(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

std::string Hex4(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%04X", v);
  return b;
}

// Each picture's one Texture object (with its Video), linked to any material slots.
std::map<std::string, int64_t> g_texture_ids;

// The picture linked to a material slot.
void LinkTexture(fbx::Document& doc, const std::string& out_dir, const std::string& name, int64_t material, const char* slot) {
  if (const auto it = g_texture_ids.find(name); it != g_texture_ids.end()) {
    doc.Connect(it->second, material, slot);
    return;
  }
  const std::string rel = "textures/" + name + ".png";
  const std::string abs = P8(fs::u8path(out_dir) / "textures" / fs::u8path(name + ".png"));
  const int64_t tid = doc.NewId(), vid = doc.NewId();
  g_texture_ids[name] = tid;
  fbx::Node& tx = doc.objects->Add("Texture");
  tx.L(tid).S(fbx::ObjName(name, "Texture")).S("");
  tx.Add("Type").S("TextureVideoClip");
  tx.Add("Version").I(202);
  tx.Add("TextureName").S(fbx::ObjName(name, "Texture"));
  tx.Add("Media").S(fbx::ObjName(name, "Video"));
  tx.Add("FileName").S(abs);
  tx.Add("RelativeFilename").S(rel);
  fbx::Node& vd = doc.objects->Add("Video");
  vd.L(vid).S(fbx::ObjName(name, "Video")).S("Clip");
  vd.Add("Type").S("Clip");
  fbx::Node& vp = vd.Add("Properties70");
  vp.Add("P").S("Path").S("KString").S("XRefUrl").S("").S(abs);
  vd.Add("UseMipMap").I(0);
  vd.Add("Filename").S(abs);
  vd.Add("RelativeFilename").S(rel);
  doc.Connect(vid, tid);
  doc.Connect(tid, material, slot);
}

// One object: the model's meshes, placed, in FBX axes (the game's turned 180 degrees about X: game Y points down).
bool AddObject(fbx::Document& doc, const ArenaModel& am, const std::string& oname, const std::string& flags, const Place& place,
               bool mirror, const std::set<std::string>& written, const std::string& out_dir, ArenaFullReport& rep) {
  const Model& m = am.model;
  const bool blended = Word(flags, "s") || Word(flags, "k") || Word(flags, "i") || Word(flags, "j");
  const bool added = Word(flags, "a") || Word(flags, "o");  // (drawn additive: black is see-through)
  std::vector<double> verts, normals, uvs, colors;
  std::vector<int32_t> poly, uv_index, col_index, mat_index;
  int base = 0, tris = 0;
  for (size_t si = 0; si < m.meshes.size(); ++si) {
    const Mesh& s = m.meshes[si];
    double nr[3][3];  // (normals: the rotation without the scale)
    for (int i = 0; i < 3; ++i) {
      const double l = std::sqrt(place.r[i][0] * place.r[i][0] + place.r[i][1] * place.r[i][1] + place.r[i][2] * place.r[i][2]);
      for (int j = 0; j < 3; ++j) nr[i][j] = l > 0 ? place.r[i][j] / l : 0;
    }
    const auto apply = [](const double r[3][3], const float v[3], double o[3]) {
      for (int j = 0; j < 3; ++j) o[j] = v[0] * r[0][j] + v[1] * r[1][j] + v[2] * r[2][j];
    };
    for (size_t k = 0; k < s.verts.size(); ++k) {
      const Vertex& v = s.verts[k];
      double p[3];
      apply(place.r, v.pos, p);
      for (int j = 0; j < 3; ++j) p[j] += place.t[j];
      verts.insert(verts.end(), {mirror ? -p[0] : p[0], -p[1], -p[2]});
      uvs.insert(uvs.end(), {double(s.uvs[k][0]), 1.0 - double(s.uvs[k][1])});
      colors.insert(colors.end(), {(v.color >> 16 & 255) / 255.0, (v.color >> 8 & 255) / 255.0, (v.color & 255) / 255.0,
                                   (v.color >> 24 & 255) / 255.0});
    }
    for (const auto& st : s.strips)
      for (const auto& t : StripToTriangles(st.indices)) {
        for (int c = 0; c < 3; ++c) {
          const int corner = t[mirror ? 2 - c : c];  // (mirrored: the winding turned)
          const int vi = base + corner;
          poly.push_back(c == 2 ? -(vi + 1) : vi);
          double n[3];
          apply(nr, s.verts[size_t(corner)].normal, n);
          normals.insert(normals.end(), {mirror ? -n[0] : n[0], -n[1], -n[2]});
          uv_index.push_back(vi);
          col_index.push_back(vi);
        }
        mat_index.push_back(int32_t(si));
        ++tris;
      }
    base += int(s.verts.size());
  }
  if (poly.empty()) return false;
  rep.triangles += tris;
  rep.meshes += int(m.meshes.size());
  const int64_t gid = doc.NewId(), mid = doc.NewId();
  fbx::Node& g = doc.objects->Add("Geometry");
  g.L(gid).S(fbx::ObjName(oname, "Geometry")).S("Mesh");
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
    fbx::Node& n = g.Add("LayerElementColor").I(0);
    n.Add("Version").I(101), n.Add("Name").S("Col");
    n.Add("MappingInformationType").S("ByPolygonVertex");
    n.Add("ReferenceInformationType").S("IndexToDirect");
    n.Add("Colors").Ad(colors);
    n.Add("ColorIndex").Ai(col_index);
  }
  {
    fbx::Node& n = g.Add("LayerElementMaterial").I(0);
    n.Add("Version").I(101), n.Add("Name").S("");
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
    fbx::PString(p, "svr_flags", flags, true);
  }
  mo.Add("Shading").C(true);
  mo.Add("Culling").S("CullingOff");
  doc.Connect(mid, 0);
  doc.Connect(gid, mid);
  for (size_t si = 0; si < m.meshes.size(); ++si) {
    const Mesh& s = m.meshes[si];
    const int64_t matid = doc.NewId();
    fbx::Node& mat = doc.objects->Add("Material");
    mat.L(matid).S(fbx::ObjName(oname + "_m" + std::to_string(si), "Material")).S("");
    mat.Add("Version").I(102);
    mat.Add("ShadingModel").S("phong");
    fbx::Node& p = mat.Add("Properties70");
    fbx::PVec(p, "DiffuseColor", "Color", 1, 1, 1);
    fbx::PString(p, "svr_shader", s.shader, true);
    doc.Connect(matid, mid);
    const auto tex = [&](const char* param) {
      const int slot = DiffuseSlot(s, param);
      const std::string t = slot >= 0 && slot < int(m.textures.size()) ? m.textures[size_t(slot)] : "";
      return written.count(t) ? t : std::string();
    };
    const std::string color = tex("texDiffuse"), normal = tex("texNormal");
    if (!color.empty()) {
      LinkTexture(doc, out_dir, color, matid, "DiffuseColor");
      if (blended) LinkTexture(doc, out_dir, color, matid, "TransparentColor");  // (see-through by the picture's alpha)
      if (added) {  // (a copy whose alpha is its brightness: the nearest other programs have to additive)
        const std::string glow = color + "_glow";
        const std::string file = P8(fs::u8path(out_dir) / "textures" / fs::u8path(glow + ".png"));
        std::error_code ec;
        if (!fs::exists(fs::u8path(file), ec)) {
          Image img;
          if (LoadImageFile(P8(fs::u8path(out_dir) / "textures" / fs::u8path(color + ".png")), img)) {
            for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) img.rgba[i + 3] = std::max({img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]});
            SavePng(file, img);
          }
        }
        LinkTexture(doc, out_dir, glow, matid, "TransparentColor");
      }
    }
    if (!normal.empty()) {
      LinkTexture(doc, out_dir, normal, matid, "NormalMap");
      ++rep.normal_maps;
    }
  }
  return true;
}

// A light as an FBX light: it stands at pos and shines at the centre (FBX lights shine along their local -Y).
void AddLight(fbx::Document& doc, const std::string& name, const Key& k, ArenaFullReport& rep) {
  const double pos[3] = {k.v[0], -double(k.v[1]), -double(k.v[2])};
  const double len = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
  double y[3] = {0, 1, 0};  // local +Y: away from the centre
  if (len > 1e-6)
    for (int i = 0; i < 3; ++i) y[i] = pos[i] / len;
  double x[3];
  const double up[3] = {std::fabs(y[1]) > 0.9 ? 1.0 : 0.0, std::fabs(y[1]) > 0.9 ? 0.0 : 1.0, 0};
  x[0] = up[1] * y[2] - up[2] * y[1], x[1] = up[2] * y[0] - up[0] * y[2], x[2] = up[0] * y[1] - up[1] * y[0];
  const double xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
  for (double& c : x) c /= xl;
  const double z[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
  // the matrix with those columns, as Euler XYZ (R = Rz Ry Rx)
  const double sy = std::clamp(-x[2], -1.0, 1.0), ey = std::asin(sy);
  double ex, ez;
  if (std::fabs(sy) < 0.99999) ex = std::atan2(y[2], z[2]), ez = std::atan2(x[1], x[0]);
  else ex = std::atan2(-z[1], y[1]), ez = 0;
  const double deg = 180.0 / kPi;
  const int64_t attr = doc.NewId(), id = doc.NewId();
  fbx::Node& na = doc.objects->Add("NodeAttribute");
  na.L(attr).S(fbx::ObjName(name, "NodeAttribute")).S("Light");
  na.Add("TypeFlags").S("Light");
  na.Add("GeometryVersion").I(124);
  {
    fbx::Node& p = na.Add("Properties70");
    p.Add("P").S("LightType").S("enum").S("").S("").I(1);  // (directional)
    fbx::PVec(p, "Color", "Color", std::min(1.0f, k.v[6]), std::min(1.0f, k.v[7]), std::min(1.0f, k.v[8]));
    fbx::PDouble(p, "Intensity", 100.0);
  }
  fbx::Node& mo = doc.objects->Add("Model");
  mo.L(id).S(fbx::ObjName(name, "Model")).S("Light");
  mo.Add("Version").I(232);
  fbx::Node& p = mo.Add("Properties70");
  fbx::PVec(p, "Lcl Translation", "Lcl Translation", pos[0], pos[1], pos[2]);
  fbx::PVec(p, "Lcl Rotation", "Lcl Rotation", ex * deg, ey * deg, ez * deg);
  mo.Add("Shading").C(true);
  doc.Connect(id, 0);
  doc.Connect(attr, id);
  ++rep.lights;
}

}  // namespace

bool ExportArenaFull(const Arena& a, const std::string& out_dir, const std::string& title, ArenaFullReport& rep) {
  std::error_code ec;
  fs::create_directories(fs::u8path(out_dir) / "textures", ec);
  if (ec) {
    rep.warnings.push_back("cannot create " + out_dir);
    return false;
  }
  std::set<std::string> written;
  for (const auto& b : a.bundles)
    for (const auto& t : b.textures) {
      if (written.count(t.name)) continue;
      Image img;
      if (DdsDecode(t.data, img) && SavePng(P8(fs::u8path(out_dir) / "textures" / fs::u8path(t.name + ".png")), img)) {
        written.insert(t.name);
        ++rep.textures;
      } else {
        ++rep.textures_failed;
        rep.warnings.push_back("texture " + t.name + ": not written");
      }
    }
  Bank lights, motions;
  for (const auto& e : a.entries) {
    if (e.id == 30000) lights = ReadBank(Unpack(e.data), false);
    if (e.id == 30001) motions = ReadBank(Unpack(e.data), true);
  }
  const auto flags = FlagsOf(a);
  fbx::Document doc(10.0, 1);
  g_texture_ids.clear();
  const bool per_rope = std::any_of(a.models.begin(), a.models.end(), [](const ArenaModel& m) { return m.id >= 900 && m.id <= 911; });
  for (const auto& am : a.models) {
    const auto fl = flags.find(am.id);
    const std::string f = fl == flags.end() ? std::string("  ") : fl->second;
    std::vector<Place> ring;
    std::vector<std::string> ring_names;
    if (f.find(" m(") == std::string::npos && RingPlaces(am.id, per_rope, ring, ring_names)) {
      if (ring.empty()) ++rep.not_drawn;
      for (size_t k = 0; k < ring.size(); ++k)
        if (AddObject(doc, am, am.model.name + "__" + Hex4(am.id) + ring_names[k], f, ring[k], false, written, out_dir, rep)) {
          ++rep.objects;
          ++rep.ring_parts;
        }
      continue;
    }
    Place place;
    const size_t mp = f.find(" m(");
    bool placed = false;
    if (mp != std::string::npos) {
      const auto t = motions.find({std::atoi(f.c_str() + mp + 3), 0});
      if (t != motions.end()) place = PlaceOf(t->second), placed = true;
      else rep.warnings.push_back(am.model.name + ": its motion isn't in the arena, left at the centre");
    }
    const std::string name = am.model.name + "__" + Hex4(am.id);
    if (!AddObject(doc, am, name, f, place, false, written, out_dir, rep)) continue;
    ++rep.objects;
    rep.placed += placed;
    if (Word(f, "r") && AddObject(doc, am, name + "__mirror", f, place, true, written, out_dir, rep)) {
      ++rep.objects;
      ++rep.mirrored;
    }
  }
  // the default light set as lights; every set's first keys in lights.json
  static const struct {
    int channel;
    const char* name;
  } kChannels[] = {{0, "fill_ground"}, {1, "fill_sky"}, {10, "dir_1"}, {11, "dir_2"}, {12, "dir_3"}, {13, "dir_4"}};
  for (const auto& c : kChannels) {
    const auto t = lights.find({100, c.channel});
    if (t != lights.end()) AddLight(doc, std::string("light100_") + c.name, t->second, rep);
  }
  std::string json = "{\n  \"arena\": " + JsonEsc(title) +
                     ",\n  \"note\": \"first frame of each light: game units, game axes (Y points down); colour 1 = full\",\n  \"lights\": [\n";
  std::set<int> sets;
  bool first = true;
  for (const auto& [key, k] : lights) {
    sets.insert(key.first);
    char b[256];
    std::snprintf(b, sizeof b, "%s    {\"set\": %d, \"channel\": %d, \"pos\": [%.3f, %.3f, %.3f], \"colour\": [%.3f, %.3f, %.3f]}", first ? "" : ",\n",
                  key.first, key.second, k.v[0], k.v[1], k.v[2], k.v[6], k.v[7], k.v[8]);
    json += b;
    first = false;
  }
  json += "\n  ]\n}\n";
  rep.light_sets = int(sets.size());
  WriteFile(P8(fs::u8path(out_dir) / "lights.json"), Bytes(json.begin(), json.end()));
  if (!WriteFile(P8(fs::u8path(out_dir) / "arena.fbx"), doc.Write())) {
    rep.warnings.push_back("cannot write arena.fbx");
    return false;
  }
  return true;
}

}  // namespace svrfmt
