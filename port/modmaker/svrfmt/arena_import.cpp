#include "arena_import.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
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

// "name__0123" -> 0x123 (or -1)
long IdFromName(const std::string& n) {
  const size_t at = n.rfind("__");
  if (at == std::string::npos || n.size() - at - 2 != 4) return -1;
  char* end = nullptr;
  const long v = std::strtol(n.c_str() + at + 2, &end, 16);
  return end && !*end ? v : -1;
}

// "<object>_m<i>" -> i (or -1)
int MeshIndexFromMaterial(const std::string& mat, const std::string& object) {
  const std::string pre = object + "_m";
  if (mat.rfind(pre, 0) != 0) return -1;
  char* end = nullptr;
  const long v = std::strtol(mat.c_str() + pre.size(), &end, 10);
  return end && (!*end || *end == '.') ? int(v) : -1;
}

std::string TextureName(const ufbx_material* m) {
  const ufbx_texture* t = nullptr;
  if (m) {
    if (m->fbx.diffuse_color.texture) t = m->fbx.diffuse_color.texture;
    else if (m->pbr.base_color.texture) t = m->pbr.base_color.texture;
  }
  if (!t) return "";
  std::string f = Str(t->relative_filename);
  if (f.empty()) f = Str(t->filename);
  if (f.empty()) f = Str(t->name);
  return PathStr(fs::u8path(f).stem());
}

uint32_t PackColor(const ufbx_vec4& c) {
  auto q = [](double v) { return uint32_t(std::clamp(int(std::lround(v * 255.0)), 0, 255)); };
  return q(c.w) << 24 | q(c.x) << 16 | q(c.y) << 8 | q(c.z);
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

// One mesh's worth of triangles from an FBX node, in game coordinates.
struct Part {
  std::vector<Corner> verts;
  std::vector<std::array<uint16_t, 3>> tris;
  std::unordered_map<Corner, uint16_t, CornerHash> index;
  bool overflow = false;
  uint16_t Add(const Corner& c) {
    auto it = index.find(c);
    if (it != index.end()) return it->second;
    if (verts.size() >= 0xFFFF) { overflow = true; return 0; }
    const uint16_t i = uint16_t(verts.size());
    verts.push_back(c);
    index.emplace(c, i);
    return i;
  }
};

std::array<float, 4> Sphere(const std::vector<Vertex>& v) {
  if (v.empty()) return {0, 0, 0, 0};
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto& x : v)
    for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], x.pos[k]); hi[k] = std::max(hi[k], x.pos[k]); }
  std::array<float, 4> s{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2, 0};
  for (const auto& x : v) {
    const float d = std::sqrt((x.pos[0] - s[0]) * (x.pos[0] - s[0]) + (x.pos[1] - s[1]) * (x.pos[1] - s[1]) +
                              (x.pos[2] - s[2]) * (x.pos[2] - s[2]));
    s[3] = std::max(s[3], d);
  }
  return s;
}

// Triangles as sorted, rotation-normalised keys (position to 0.05 units, UV
// to 1/512): equal sets mean Blender gave the mesh back unchanged.
using TriKey = std::array<int32_t, 15>;
std::vector<TriKey> Keys(const std::vector<std::array<float, 5>>& c, const std::vector<std::array<uint16_t, 3>>& tris) {
  std::vector<TriKey> out;
  out.reserve(tris.size());
  for (const auto& t : tris) {
    std::array<std::array<int32_t, 5>, 3> k;
    for (int i = 0; i < 3; ++i) {
      const auto& v = c[t[i]];
      k[i] = {int32_t(std::lround(v[0] * 20)), int32_t(std::lround(v[1] * 20)), int32_t(std::lround(v[2] * 20)),
              int32_t(std::lround(v[3] * 512)), int32_t(std::lround(v[4] * 512))};
    }
    int r = 0;
    for (int i = 1; i < 3; ++i)
      if (k[i] < k[r]) r = i;
    TriKey key;
    for (int i = 0; i < 3; ++i)
      std::copy(k[(r + i) % 3].begin(), k[(r + i) % 3].end(), key.begin() + 5 * i);
    out.push_back(key);
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool SameGeometry(const Mesh& s, const Part& p) {
  std::vector<std::array<float, 5>> a, b;
  for (size_t i = 0; i < s.verts.size(); ++i)
    a.push_back({s.verts[i].pos[0], s.verts[i].pos[1], s.verts[i].pos[2], s.uvs[i][0], s.uvs[i][1]});
  for (const auto& c : p.verts) b.push_back({c.pos[0], c.pos[1], c.pos[2], c.uv[0], c.uv[1]});
  std::vector<std::array<uint16_t, 3>> ta;
  for (const auto& st : s.strips)
    for (const auto& t : StripToTriangles(st.indices)) ta.push_back(t);
  return Keys(a, ta) == Keys(b, p.tris);
}

void FillMesh(Mesh& s, const Part& p) {
  s.verts.clear();
  s.uvs.clear();
  for (const auto& c : p.verts) {
    Vertex v;
    std::memcpy(v.pos, c.pos, sizeof v.pos);
    std::memcpy(v.normal, c.normal, sizeof v.normal);
    v.color = c.color;
    s.verts.push_back(v);
    s.uvs.push_back({c.uv[0], c.uv[1]});
  }
  // static geometry: one weight block, palette slot 0, weight 1
  s.weights.assign(1, std::vector<Weight>(s.verts.size(), Weight{{0, 0, 0, 0}, 1.f}));
  if (s.palette.empty()) s.palette = {0};
  s.strips.assign(1, Strip{6, TrianglesToStrip(p.tris)});
  const auto sp = Sphere(s.verts);
  std::copy(sp.begin(), sp.end(), s.sphere.begin());
}

}  // namespace

bool ImportFbx(Arena& arena, const std::string& fbx_path, const ImportOptions& opt, ImportReport& rep) {
  ufbx_load_opts lo = {};
  lo.target_axes = ufbx_axes_right_handed_y_up;
  lo.target_unit_meters = 0.1f;  // one game unit = 10 cm
  lo.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
  ufbx_error err;
  ufbx_scene* scene = ufbx_load_file(fbx_path.c_str(), &lo, &err);
  if (!scene) {
    char buf[512];
    ufbx_format_error(buf, sizeof buf, &err);
    rep.errors.push_back(std::string("FBX: ") + buf);
    return false;
  }
  const fs::path tex_dir = opt.textures_dir.empty() ? fs::u8path(fbx_path).parent_path() / "textures"
                                                    : fs::u8path(opt.textures_dir);
  std::map<uint32_t, ArenaModel*> by_id;
  for (auto& m : arena.models) by_id[m.id] = &m;
  // host for new objects: the biggest free model with a texture (the floor)
  ArenaModel* host = nullptr;
  for (auto& m : arena.models) {
    if (!opt.host_model.empty() ? m.model.name == opt.host_model
                                : (m.zone == Zone::kFree && m.model.name.rfind("ar_ground", 0) == 0)) {
      host = &m;
      break;
    }
  }
  std::map<uint32_t, bool> seen;
  std::map<std::string, bool> textures_used;

  for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
    ufbx_node* node = scene->nodes.data[ni];
    ufbx_mesh* mesh = node->mesh;
    if (!mesh) continue;
    const std::string oname = Str(node->name);
    long id = long(ufbx_find_int(&node->props, "svr_id", -1));
    if (id < 0) id = IdFromName(oname);
    ArenaModel* target = id >= 0 && by_id.count(uint32_t(id)) ? by_id[uint32_t(id)] : nullptr;

    // corners -> parts by material slot
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
            if (l > 1e-12) { nn.x /= l; nn.y /= l; nn.z /= l; }
          }
          // FBX (Y up) -> game (Y down): the export's 180-degree turn about X, undone
          k.pos[0] = float(p.x); k.pos[1] = float(-p.y); k.pos[2] = float(-p.z);
          k.normal[0] = float(nn.x); k.normal[1] = float(-nn.y); k.normal[2] = float(-nn.z);
          if (mesh->vertex_uv.exists) {
            const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
            k.uv[0] = float(uv.x);
            k.uv[1] = float(1.0 - uv.y);
          }
          k.color = mesh->vertex_color.exists ? PackColor(ufbx_get_vertex_vec4(&mesh->vertex_color, ix)) : 0xFFFFFFFFu;
          out[c] = part.Add(k);
        }
        part.tris.push_back(out);
      }
    }

    // apply
    for (auto& [slot, part] : parts) {
      if (part.overflow) { rep.errors.push_back(oname + ": more than 65535 vertices in one material"); continue; }
      const ufbx_material* mat = slot < node->materials.count ? node->materials.data[slot] : nullptr;
      const std::string mname = mat ? Str(mat->name) : "";
      const std::string tex = TextureName(mat);
      if (!tex.empty()) textures_used[tex] = true;
      ArenaModel* owner = target ? target : host;
      if (!owner) { rep.errors.push_back(oname + ": no arena model to add it to"); continue; }
      Model& m = owner->model;
      const int mi = target ? MeshIndexFromMaterial(mname, oname) : -1;
      if (mi >= 0 && mi < int(m.meshes.size())) {
        Mesh& s = m.meshes[mi];
        const int cur_slot = DiffuseSlot(s);
        const std::string cur_tex = cur_slot >= 0 && cur_slot < int(m.textures.size()) ? m.textures[cur_slot] : "";
        if ((tex.empty() || tex == cur_tex) && SameGeometry(s, part)) continue;  // untouched: keep as is
        const bool skinned = s.palette.size() > 1 || s.weights.size() > 1;
        if (skinned) {
          if (part.verts.size() != s.verts.size()) {
            rep.warnings.push_back(oname + ": rigged part " + std::to_string(mi) +
                                   " keeps its shape (vertex count changed)");
          } else {
            for (size_t k = 0; k < s.verts.size(); ++k) std::memcpy(s.verts[k].pos, part.verts[k].pos, 12);
          }
        } else {
          FillMesh(s, part);
        }
        if (!tex.empty()) SetDiffuseSlot(s, TextureSlot(m, tex));
      } else {
        // new mesh, from a template: the owner's first static mesh
        const Mesh* tmpl = nullptr;
        for (const auto& s : m.meshes)
          if (s.palette.size() <= 1 && s.weights.size() <= 1) { tmpl = &s; break; }
        if (!tmpl) { rep.errors.push_back(oname + ": no static mesh to copy a material from"); continue; }
        Mesh s = *tmpl;
        s.material = 0;
        FillMesh(s, part);
        if (!tex.empty()) SetDiffuseSlot(s, TextureSlot(m, tex));
        m.meshes.push_back(std::move(s));
        ++rep.meshes_added;
      }
      owner->changed = true;
    }
    if (target) {
      // material slots that vanished: their meshes are emptied
      seen[target->id] = true;
    } else {
      ++rep.objects_added;
    }
  }
  ufbx_free_scene(scene);
  for (const auto& m : arena.models)
    if (m.changed) ++rep.models_changed;

  if (opt.hide_missing)
    for (auto& m : arena.models)
      if (!seen.count(m.id) && m.zone == Zone::kFree) {
        for (auto& s : m.model.meshes)
          for (auto& st : s.strips) st.indices.clear();
        m.changed = true;
        ++rep.models_hidden;
      }

  // textures: replace changed ones, add new ones
  for (const auto& [name, _] : textures_used) {
    const fs::path png = tex_dir / (name + ".png");
    std::error_code ec;
    if (!fs::exists(png, ec)) {
      if (!arena.FindTexture(name)) rep.warnings.push_back("texture " + name + ": no " + PathStr(png));
      continue;
    }
    Image img;
    if (!LoadImageFile(PathStr(png), img)) { rep.warnings.push_back("texture " + name + ": unreadable PNG"); continue; }
    bool alpha = false;
    for (size_t i = 3; i < img.rgba.size() && !alpha; i += 4) alpha = img.rgba[i] < 250;
    BundleTexture* existing = arena.FindTexture(name);
    if (existing) {
      Image cur;
      if (DdsDecode(existing->data, cur) && cur.w == img.w && cur.h == img.h && cur.rgba == img.rgba) continue;
      DdsInfo info;
      DdsInfoOf(existing->data, info);
      DxtFormat f = info.format;
      if (f == DxtFormat::kDxt1 && alpha) f = DxtFormat::kDxt5;
      // keep power-of-two sizes the game expects
      auto pot = [](int v) { int p = 1; while (p < v && p < 2048) p <<= 1; return p; };
      if (pot(img.w) != img.w || pot(img.h) != img.h) img = Resize(img, pot(img.w), pot(img.h));
      existing->data = DdsEncode(img, f == DxtFormat::kUnknown ? DxtFormat::kDxt5 : f);
      for (auto& b : arena.bundles)
        for (auto& t : b.textures)
          if (&t == existing) b.changed = true;
      ++rep.textures_replaced;
    } else if (!arena.bundles.empty()) {
      auto pot = [](int v) { int p = 4; while (p < v && p < 1024) p <<= 1; return p; };
      if (pot(img.w) != img.w || pot(img.h) != img.h) img = Resize(img, pot(img.w), pot(img.h));
      arena.bundles[0].textures.push_back({name, "dds", DdsEncode(img, alpha ? DxtFormat::kDxt5 : DxtFormat::kDxt1)});
      arena.bundles[0].changed = true;
      ++rep.textures_added;
    }
  }
  // stay within the arena's memory budget (textures the modder supplied are kept)
  std::vector<std::string> keep;
  for (const auto& [name, _] : textures_used) keep.push_back(name);
  for (const auto& t : arena.FitBudget(keep)) rep.warnings.push_back("texture " + t + " halved to fit the arena's memory");
  return rep.errors.empty();
}

}  // namespace svrfmt
