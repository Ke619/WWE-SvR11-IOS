#include "model_objects.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>

#include "png.h"
#include "ufbx.h"

namespace fs = std::filesystem;

namespace svrfmt {

namespace {

std::string Str(const ufbx_string& s) { return std::string(s.data, s.length); }

std::string PathStr(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

bool IsStatic(const Mesh& s) { return s.palette.size() <= 1 && s.weights.size() <= 1; }

size_t Triangles(const Mesh& s) {
  size_t n = 0;
  for (const auto& st : s.strips) n += StripToTriangles(st.indices).size();
  return n;
}

// A texture name the game's 16-byte fields hold: letters, digits and _ (lower case), at most 15.
std::string GameTextureName(std::string n) {
  std::string out;
  for (char c : n) {
    if (std::isalnum(static_cast<unsigned char>(c))) out += char(std::tolower(static_cast<unsigned char>(c)));
    else if (c == '_' || c == '-' || c == ' ') out += '_';
  }
  if (out.empty()) out = "prop";
  return out.substr(0, 15);
}

// A texture file named by a material, next to the model (as written, by its name only, by its stem with any of the
// usual extensions).
std::string FindTextureFile(const fs::path& dir, const ufbx_texture* t) {
  std::vector<fs::path> tries;
  for (const ufbx_string& s : {t->absolute_filename, t->filename, t->relative_filename})
    if (s.length) {
      const fs::path p = fs::u8path(Str(s));
      tries.push_back(p);
      tries.push_back(dir / p);
      tries.push_back(dir / p.filename());
    }
  std::error_code ec;
  for (const auto& p : tries)
    if (!p.empty() && fs::is_regular_file(p, ec)) return PathStr(p);
  if (!tries.empty()) {
    const fs::path stem = tries.back().stem();
    for (const char* ext : {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".psd"})
      if (fs::is_regular_file(dir / (stem.string() + ext), ec)) return PathStr(dir / (stem.string() + ext));
  }
  return "";
}

const ufbx_texture* DiffuseTexture(const ufbx_material* m) {
  if (!m) return nullptr;
  if (m->fbx.diffuse_color.texture) return m->fbx.diffuse_color.texture;
  if (m->pbr.base_color.texture) return m->pbr.base_color.texture;
  return nullptr;
}

struct Corner {
  float pos[3], normal[3], uv[2];
  uint32_t color;
  bool operator==(const Corner& o) const { return !std::memcmp(this, &o, sizeof *this); }
};
struct CornerHash {
  size_t operator()(const Corner& c) const {
    size_t h = 1469598103934665603ull;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&c);
    for (size_t i = 0; i < sizeof c; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
  }
};
struct Part {
  std::vector<Corner> verts;
  std::vector<std::array<uint16_t, 3>> tris;
  std::unordered_map<Corner, uint16_t, CornerHash> index;
  bool overflow = false;
  uint16_t Add(const Corner& c) {
    auto it = index.find(c);
    if (it != index.end()) return it->second;
    if (verts.size() >= 0xFFFF) {
      overflow = true;
      return 0;
    }
    const uint16_t i = uint16_t(verts.size());
    verts.push_back(c);
    index.emplace(c, i);
    return i;
  }
};

uint32_t PackColor(const ufbx_vec4& c) {
  auto q = [](double v) { return uint32_t(std::clamp(int(std::lround(v * 255.0)), 0, 255)); };
  return q(c.w) << 24 | q(c.x) << 16 | q(c.y) << 8 | q(c.z);
}

// The image `file` as an arena texture: a free name (`want`, or want_2..) unless the same picture is there already.
std::string AddTexture(Arena& a, const std::string& file, const std::string& want, AddReport& rep) {
  Image img;
  if (!LoadImageFile(file, img) || img.w <= 0 || img.h <= 0) {
    rep.warnings.push_back("texture " + file + ": not readable (the object gets the host's texture)");
    return "";
  }
  if (a.bundles.empty()) {
    rep.warnings.push_back("the arena has no texture set: " + file + " left out");
    return "";
  }
  bool alpha = false;
  for (size_t i = 3; i < img.rgba.size() && !alpha; i += 4) alpha = img.rgba[i] < 250;
  auto pot = [](int v) { int p = 4; while (p < v && p < 1024) p <<= 1; return p; };
  if (pot(img.w) != img.w || pot(img.h) != img.h) img = Resize(img, pot(img.w), pot(img.h));
  const Bytes dds = DdsEncode(img, alpha ? DxtFormat::kDxt5 : DxtFormat::kDxt1);
  std::string name = GameTextureName(want);
  for (int n = 2; n < 1000; ++n) {
    const BundleTexture* have = a.FindTexture(name);
    if (!have) break;
    if (have->data == dds) return name;  // (the same picture: shared)
    Image cur;  // (an exported texture coming back: the arena's own picture, pixel for pixel)
    if (DdsDecode(have->data, cur) && cur.w == img.w && cur.h == img.h && cur.rgba == img.rgba) return name;
    char suffix[8];
    std::snprintf(suffix, sizeof suffix, "_%d", n);
    name = GameTextureName(want).substr(0, 15 - std::strlen(suffix)) + suffix;
  }
  a.bundles[0].textures.push_back({name, "dds", dds});
  a.bundles[0].changed = true;
  ++rep.textures_added;
  return name;
}

std::string Sanitize(const std::string& n) {
  std::string out;
  for (char c : n) out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? c : '_';
  return out.empty() ? "object" : out;
}

}  // namespace

std::vector<ObjectInfo> ObjectsOf(const Model& m) {
  std::vector<ObjectInfo> out;
  for (size_t i = 0; i < m.meshes.size(); ++i) {
    const Mesh& s = m.meshes[i];
    ObjectInfo o;
    o.mesh = int(i);
    const int slot = DiffuseSlot(s);
    o.texture = slot >= 0 && slot < int(m.textures.size()) ? m.textures[slot] : "";
    o.verts = s.verts.size();
    o.triangles = Triangles(s);
    o.skinned = !IsStatic(s);
    o.empty = o.triangles == 0;
    out.push_back(o);
  }
  return out;
}

size_t MeshCount(const Arena& a) {
  size_t n = 0;
  for (const auto& am : a.models)
    for (const auto& s : am.model.meshes) n += Triangles(s) > 0;
  return n;
}

int FindModel(const Arena& a, const std::string& key) {
  for (size_t i = 0; i < a.models.size(); ++i)
    if (a.models[i].model.name == key) return int(i);
  char* end = nullptr;
  const unsigned long id = std::strtoul(key.c_str(), &end, key.rfind("0x", 0) == 0 ? 16 : 10);
  if (end && !*end)
    for (size_t i = 0; i < a.models.size(); ++i)
      if (a.models[i].id == id) return int(i);
  return -1;
}

bool AddObjectFile(Arena& a, int model, const std::string& path, const AddOptions& opt, AddReport& rep) {
  if (model < 0 || model >= int(a.models.size())) {
    rep.errors.push_back("no such model");
    return false;
  }
  Model& host = a.models[size_t(model)].model;
  const Mesh* tmpl = nullptr;
  for (const auto& s : host.meshes)
    if (IsStatic(s) && DiffuseSlot(s) >= 0) {
      tmpl = &s;
      break;
    }
  if (!tmpl) {
    rep.errors.push_back(host.name + " has no plain textured mesh to copy a material from - pick another model");
    return false;
  }
  if (!host.nodes.empty()) {
    const Node& n = host.nodes[0];
    if (n.t[0] || n.t[1] || n.t[2] || n.r[0] || n.r[1] || n.r[2])
      rep.warnings.push_back(host.name + "'s node moves its meshes: the object is placed relative to it");
  }
  ufbx_load_opts lo = {};
  lo.target_axes = ufbx_axes_right_handed_y_up;
  lo.target_unit_meters = 0.1f;  // one game unit = 10 cm
  lo.obj_unit_meters = opt.obj_unit_meters > 0 ? opt.obj_unit_meters : 1.0f;
  lo.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
  lo.obj_search_mtl_by_filename = true;
  lo.load_external_files = true;  // (an OBJ's .mtl - its materials and texture names)
  lo.ignore_missing_external_files = true;
  lo.generate_missing_normals = true;  // (an OBJ without vn: flat faces lit as faces, not all facing up)
  ufbx_error err;
  ufbx_scene* scene = ufbx_load_file(path.c_str(), &lo, &err);
  if (!scene) {
    char buf[512];
    ufbx_format_error(buf, sizeof buf, &err);
    rep.errors.push_back(std::string("can't read ") + path + ": " + buf);
    return false;
  }
  const fs::path dir = fs::u8path(path).parent_path();
  const float yaw = opt.yaw_degrees * 3.14159265f / 180.0f, cy = std::cos(yaw), sy = std::sin(yaw);
  const float sc = opt.scale > 0 ? opt.scale : 1.0f;
  std::map<const ufbx_material*, std::string> tex_of;
  const int first = int(host.meshes.size());
  for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
    ufbx_node* node = scene->nodes.data[ni];
    ufbx_mesh* mesh = node->mesh;
    if (!mesh) continue;
    const ufbx_matrix nm = ufbx_matrix_for_normals(&node->geometry_to_world);
    std::map<uint32_t, Part> parts;
    std::vector<uint32_t> tri(mesh->max_face_triangles * 3);
    for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
      const ufbx_face face = mesh->faces.data[fi];
      const uint32_t slot = mesh->face_material.count ? mesh->face_material.data[fi] : 0;
      const uint32_t n = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
      Part& part = parts[slot];
      for (uint32_t t = 0; t < n; ++t) {
        std::array<uint16_t, 3> out;
        for (int c = 0; c < 3; ++c) {
          const uint32_t ix = tri[t * 3 + c];
          Corner k{};
          ufbx_vec3 p = ufbx_transform_position(&node->geometry_to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, ix));
          ufbx_vec3 nn = {0, 1, 0};
          if (mesh->vertex_normal.exists) {
            nn = ufbx_transform_direction(&nm, ufbx_get_vertex_vec3(&mesh->vertex_normal, ix));
            const double l = std::sqrt(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
            if (l > 1e-12) nn.x /= l, nn.y /= l, nn.z /= l;
          }
          const float px = float(p.x) * sc, py = float(p.y) * sc, pz = float(p.z) * sc;
          // Y up -> the game's Y down (a half turn about X), then the yaw and the place
          const float gx = px, gy = -py, gz = -pz;
          k.pos[0] = gx * cy + gz * sy + opt.pos[0];
          k.pos[1] = gy + opt.pos[1];
          k.pos[2] = -gx * sy + gz * cy + opt.pos[2];
          const float nx = float(nn.x), ny = -float(nn.y), nz = -float(nn.z);
          k.normal[0] = nx * cy + nz * sy;
          k.normal[1] = ny;
          k.normal[2] = -nx * sy + nz * cy;
          if (mesh->vertex_uv.exists) {
            const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
            k.uv[0] = float(uv.x);
            k.uv[1] = float(1.0 - uv.y);
          }
          k.color = mesh->vertex_color.exists ? PackColor(ufbx_get_vertex_vec4(&mesh->vertex_color, ix)) : 0xFFFFFFFFu;
          out[c] = part.Add(k);
        }
        // (front faces: counter-clockwise in OBJ / FBX, clockwise in the game - Direct3D's - which culls the
        // other side: each triangle turned round; the Y-up -> Y-down half turn is a rotation and mirrors nothing)
        part.tris.push_back({out[0], out[2], out[1]});
      }
    }
    for (auto& [slot, part] : parts) {
      if (part.overflow) {
        rep.errors.push_back(Str(node->name) + ": more than 65,535 vertices with one material - split it");
        continue;
      }
      if (part.tris.empty()) continue;
      const ufbx_material* mat = slot < node->materials.count ? node->materials.data[slot] : nullptr;
      if (!tex_of.count(mat)) {
        std::string name;
        if (const ufbx_texture* t = DiffuseTexture(mat)) {
          const std::string file = FindTextureFile(dir, t);
          if (file.empty())
            rep.warnings.push_back("texture of material " + (mat ? Str(mat->name) : std::string("?")) +
                                   ": file not found (the object gets the host's texture)");
          else
            name = AddTexture(a, file, PathStr(fs::u8path(file).stem()), rep);
        }
        tex_of[mat] = name;
      }
      Mesh s = *tmpl;
      s.material = 0;
      s.verts.clear();
      s.uvs.clear();
      for (const auto& c : part.verts) {
        Vertex v;
        std::memcpy(v.pos, c.pos, sizeof v.pos);
        std::memcpy(v.normal, c.normal, sizeof v.normal);
        v.color = c.color;
        s.verts.push_back(v);
        s.uvs.push_back({c.uv[0], c.uv[1]});
      }
      s.weights.assign(1, std::vector<Weight>(s.verts.size(), Weight{{0, 0, 0, 0}, 1.f}));
      if (s.palette.empty()) s.palette = {0};
      s.strips.assign(1, Strip{6, TrianglesToStrip(part.tris)});
      if (!tex_of[mat].empty()) SetDiffuseSlot(s, TextureSlot(host, tex_of[mat]));
      Bound(s);
      host.meshes.push_back(std::move(s));
      ++rep.meshes_added;
    }
  }
  ufbx_free_scene(scene);
  if (!rep.meshes_added) {
    if (rep.errors.empty()) rep.errors.push_back(path + ": no triangles in it");
    return false;
  }
  rep.first_mesh = first;
  a.models[size_t(model)].changed = true;
  // (the file's budget is the save's: FitFile with file_max, as the arena page does)
  const ArenaGeometry g = GeometryOf(a);
  if (g.Over()) rep.errors.push_back("the arena now has more 3D detail than the game can hold");
  else if (g.OverStock()) rep.warnings.push_back("the arena now needs 2.1.6 or newer (more 3D detail than stock arenas)");
  if (const size_t n = MeshCount(a); n >= kMeshDanger)
    rep.warnings.push_back(std::to_string(n) + " objects in the arena: the game may not load it (625 crashed in tests)");
  else if (n > kMeshWarn)
    rep.warnings.push_back(std::to_string(n) + " objects in the arena: more than any stock arena (446) - test it");
  return rep.errors.empty();
}

bool RemoveObject(Arena& a, int model, int mesh, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (model < 0 || model >= int(a.models.size())) return fail("no such model");
  Model& m = a.models[size_t(model)].model;
  if (mesh >= int(m.meshes.size())) return fail("no such object");
  bool any = false;
  for (int i = mesh < 0 ? 0 : mesh; i < (mesh < 0 ? int(m.meshes.size()) : mesh + 1); ++i) {
    Mesh& s = m.meshes[size_t(i)];
    if (!IsStatic(s)) {
      if (mesh >= 0) return fail("a rigged object (ropes, apron, crowd) can't be removed");
      continue;  // (a whole model: its rigged parts stay)
    }
    if (s.verts.size() <= 1 && Triangles(s) == 0) continue;
    s.verts.resize(1);
    s.uvs.resize(1);
    for (auto& w : s.weights) w.resize(1);
    for (auto& st : s.strips) st.indices.clear();
    Bound(s);
    any = true;
  }
  if (any) a.models[size_t(model)].changed = true;
  return true;
}

bool ExportObjectObj(const Arena& a, int model, const std::vector<int>& meshes, const std::string& out_dir,
                     std::string* obj_path, std::string* error, const std::string& name) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (model < 0 || model >= int(a.models.size())) return fail("no such model");
  const Model& m = a.models[size_t(model)].model;
  std::vector<int> list = meshes;
  if (list.empty())
    for (int i = 0; i < int(m.meshes.size()); ++i) list.push_back(i);
  for (int i : list)
    if (i < 0 || i >= int(m.meshes.size())) return fail("no such object");
  std::error_code ec;
  fs::create_directories(fs::u8path(out_dir), ec);
  const std::string base = !name.empty() ? Sanitize(name)
                           : Sanitize(m.name) + (meshes.size() == 1 ? "_" + std::to_string(meshes[0]) : "");
  const fs::path obj = fs::u8path(out_dir) / (base + ".obj"), mtl = fs::u8path(out_dir) / (base + ".mtl");
  std::ofstream o(obj), t(mtl);
  if (!o || !t) return fail("can't write in " + out_dir);
  o << "# " << m.name << (meshes.empty() ? std::string(" (all objects)") : " objects " + std::to_string(list.size()))
    << " - SvR 2011 Mod Maker. Metres, Y up.\nmtllib " << base << ".mtl\n";
  size_t base_index = 1;
  std::map<std::string, bool> written;
  int exported = 0;
  for (int i : list) {
    const Mesh& s = m.meshes[size_t(i)];
    const auto tris = [&] {
      std::vector<std::array<uint16_t, 3>> all;
      for (const auto& st : s.strips)
        for (const auto& tr : StripToTriangles(st.indices)) all.push_back(tr);
      return all;
    }();
    if (tris.empty()) continue;
    const int slot = DiffuseSlot(s);
    const std::string tex = slot >= 0 && slot < int(m.textures.size()) ? m.textures[slot] : "";
    const std::string mat = tex.empty() ? "untextured" : Sanitize(tex);
    if (!written[mat]) {
      written[mat] = true;
      t << "newmtl " << mat << "\nKd 1 1 1\n";
      if (!tex.empty()) {
        for (const auto& b : a.bundles)
          for (const auto& bt : b.textures)
            if (bt.name == tex) {
              Image img;
              if (DdsDecode(bt.data, img)) SavePng(PathStr(fs::u8path(out_dir) / (mat + ".png")), img);
            }
        t << "map_Kd " << mat << ".png\n";
      }
      t << "\n";
    }
    o << "o " << Sanitize(m.name) << "_" << i << "\nusemtl " << mat << "\n";
    char line[160];
    // game units (10 cm, Y down) -> metres, Y up: the half turn about X back
    for (const auto& v : s.verts) {
      std::snprintf(line, sizeof line, "v %.5f %.5f %.5f\n", v.pos[0] * 0.1, -v.pos[1] * 0.1, -v.pos[2] * 0.1);
      o << line;
    }
    for (size_t k = 0; k < s.verts.size(); ++k) {
      const auto uv = k < s.uvs.size() ? s.uvs[k] : std::array<float, 2>{0, 0};
      std::snprintf(line, sizeof line, "vt %.6f %.6f\n", uv[0], 1.0 - uv[1]);
      o << line;
    }
    for (const auto& v : s.verts) {
      std::snprintf(line, sizeof line, "vn %.5f %.5f %.5f\n", v.normal[0], -v.normal[1], -v.normal[2]);
      o << line;
    }
    for (const auto& tr : tris) {
      std::snprintf(line, sizeof line, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", base_index + tr[0], base_index + tr[0],
                    base_index + tr[0], base_index + tr[2], base_index + tr[2], base_index + tr[2], base_index + tr[1],
                    base_index + tr[1], base_index + tr[1]);  // (the game's clockwise front faces -> OBJ's counter-clockwise)
      o << line;
    }
    base_index += s.verts.size();
    ++exported;
  }
  if (!exported) return fail("nothing visible to export");
  if (obj_path) *obj_path = PathStr(obj);
  return true;
}

int RefreshBounds(Arena& a) {
  for (auto& am : a.models)
    if (am.changed) FitNodeSpheres(am.model);
  return GrowVisibility(a);
}

}  // namespace svrfmt
