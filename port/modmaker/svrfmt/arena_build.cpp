#include "arena_build.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <set>

namespace svrfmt {

namespace {

bool IsStaticMesh(const Mesh& s) { return s.palette.size() <= 1 && s.weights.size() <= 1; }

bool IsFloor(const std::string& n) {
  return n.rfind("ar_ground", 0) == 0 || n.rfind("ar_ring_sita", 0) == 0 || n.rfind("ar_rin_sdw", 0) == 0;
}

size_t MeshBytes(const Mesh& s) {
  size_t n = s.verts.size() * (28 + 8) + s.weights.size() * s.verts.size() * 8;
  for (const auto& st : s.strips) n += st.indices.size() * 2;
  return n;
}

}  // namespace

void MakeEmpty(Arena& a, const EmptyOptions& opt, EmptyReport& rep) {
  std::set<std::string> used;
  for (auto& am : a.models) {
    const bool keep = am.zone == Zone::kRing || (opt.keep_ringside && am.zone == Zone::kRingside) ||
                      (opt.keep_floor && IsFloor(am.model.name));
    if (keep) {
      for (const auto& t : am.model.textures) used.insert(t);
      continue;
    }
    bool any = false;
    for (auto& s : am.model.meshes) {
      if (s.verts.size() <= 1) continue;
      rep.bytes_freed += MeshBytes(s);
      s.verts.resize(1);
      s.uvs.resize(1);
      for (auto& w : s.weights) w.resize(1);
      for (auto& st : s.strips) st.indices.clear();
      Bound(s);
      any = true;
    }
    if (any) {
      am.changed = true;
      ++rep.models_emptied;
    }
  }
  // textures only emptied models used: 4 x 4 (same name, same format)
  for (auto& b : a.bundles)
    for (auto& t : b.textures) {
      if (used.count(t.name)) continue;
      DdsInfo info;
      if (!DdsInfoOf(t.data, info) || (info.w <= 4 && info.h <= 4)) continue;
      Image img;
      img.w = img.h = 4;
      img.rgba.assign(4 * 4 * 4, 128);
      const size_t before = t.data.size();
      t.data = DdsEncode(img, info.format == DxtFormat::kArgb ? DxtFormat::kDxt5 : info.format, false);
      rep.bytes_freed += before > t.data.size() ? before - t.data.size() : 0;
      b.changed = true;
      ++rep.textures_shrunk;
    }
}

namespace {
bool IsAudienceLightmap(const BundleTexture& t) {
  std::string n = t.name;
  for (char& c : n) c = char(std::tolower(uint8_t(c)));
  return n == "aud_lightmap" && t.data.size() > 128 && !std::memcmp(t.data.data(), "DDS ", 4) &&
         (!std::memcmp(&t.data[84], "DXT1", 4) || !std::memcmp(&t.data[84], "DXT5", 4));
}
}  // namespace

bool HasAudienceLightmap(const Arena& a) {
  for (const auto& b : a.bundles)
    for (const auto& t : b.textures)
      if (IsAudienceLightmap(t)) return true;
  return false;
}

bool TintAudienceLightmap(Arena& a, const float rgb[3], float bright) {
  bool done = false;
  auto to565 = [&](uint16_t c) {
    const float r = float(c >> 11) / 31, g = float((c >> 5) & 63) / 63, b = float(c & 31) / 31;
    const float y = 0.299f * r + 0.587f * g + 0.114f * b;
    const float o[3] = {rgb ? y * rgb[0] : r, rgb ? y * rgb[1] : g, rgb ? y * rgb[2] : b};  // (null: its own colour)
    auto q = [&](float v, int top) { return int(std::lround(std::clamp(v * bright, 0.0f, 1.0f) * top)); };
    return uint16_t(q(o[0], 31) << 11 | q(o[1], 63) << 5 | q(o[2], 31));
  };
  for (auto& b : a.bundles)
    for (auto& t : b.textures) {
      if (!IsAudienceLightmap(t)) continue;
      const bool dxt5 = !std::memcmp(&t.data[84], "DXT5", 4);
      const size_t step = dxt5 ? 16 : 8, at = dxt5 ? 8 : 0;  // (DXT5: the colour half after the alpha half)
      for (size_t p = 128; p + step <= t.data.size(); p += step) {
        uint8_t* blk = &t.data[p + at];
        const uint16_t c0 = uint16_t(blk[0] | blk[1] << 8), c1 = uint16_t(blk[2] | blk[3] << 8);
        uint16_t g0 = to565(c0), g1 = to565(c1);
        if (c0 > c1) {  // (4-colour mode needs c0 > c1 still: equal would make index 3 transparent black)
          if (g0 < g1) std::swap(g0, g1), blk[4] ^= 0x55, blk[5] ^= 0x55, blk[6] ^= 0x55, blk[7] ^= 0x55;
          if (g0 == g1) g0 < 0xFFFF ? ++g0 : --g1;
        } else if (g0 > g1) {  // (3-colour mode: c0 <= c1; swapping the ends swaps indices 0 and 1)
          std::swap(g0, g1);
          for (int k = 4; k < 8; ++k) {
            uint8_t v = blk[k], o = 0;
            for (int s = 0; s < 8; s += 2) {
              const uint8_t i = (v >> s) & 3;
              o |= uint8_t((i == 0 ? 1 : i == 1 ? 0 : i) << s);
            }
            blk[k] = o;
          }
        }
        blk[0] = uint8_t(g0), blk[1] = uint8_t(g0 >> 8), blk[2] = uint8_t(g1), blk[3] = uint8_t(g1 >> 8);
      }
      b.changed = true;
      done = true;
    }
  return done;
}

bool CopyAudienceLightmap(Arena& to, const Arena& from) {
  const BundleTexture* src = nullptr;
  for (const auto& b : from.bundles)
    for (const auto& t : b.textures)
      if (IsAudienceLightmap(t)) src = &t;
  if (!src) return false;
  bool done = false;
  for (auto& b : to.bundles)
    for (auto& t : b.textures)
      if (IsAudienceLightmap(t) && t.data.size() == src->data.size()) t.data = src->data, b.changed = true, done = true;
  return done;
}

int HideCrowd(Arena& a) {
  int n = 0;
  for (auto& e : a.entries) {
    if (e.id != 0x4E20) continue;
    const bool packed = IsBpe(e.data);
    std::vector<PachEntry> subs;
    if (!PachRead(Unpack(e.data), subs)) continue;
    for (auto& sub : subs) {
      const Bytes raw = Unpack(sub.data);
      Model m;
      if (!IsJboy(raw) || !JboyRead(raw, m)) continue;
      for (auto& s : m.meshes)
        for (auto& st : s.strips) st.indices.clear();
      const Bytes w = JboyWrite(m);
      sub.data = IsBpe(sub.data) ? BpeEncode(w) : w;
      ++n;
    }
    const Bytes pach = PachWrite(subs);
    e.data = packed ? BpeEncode(pach) : pach;
  }
  return n;
}

std::vector<int> LibraryModels(const Arena& src) {
  std::vector<int> out;
  for (size_t i = 0; i < src.models.size(); ++i) {
    const auto& am = src.models[i];
    if (am.zone == Zone::kRing) continue;
    size_t tris = 0;
    for (const auto& s : am.model.meshes)
      for (const auto& st : s.strips) tris += st.indices.size();
    if (tris >= 3) out.push_back(int(i));
  }
  return out;
}

int CopyModelInto(Arena& dst, int host_model, const Arena& src, int src_model, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return -1;
  };
  if (host_model < 0 || host_model >= int(dst.models.size())) return fail("no model to add to");
  if (src_model < 0 || src_model >= int(src.models.size())) return fail("no such model");
  Model& host = dst.models[host_model].model;
  const Mesh* tmpl = nullptr;
  for (const auto& s : host.meshes)
    if (IsStaticMesh(s)) { tmpl = &s; break; }
  if (!tmpl) return fail("the floor model has no plain mesh to copy a material from");
  if (dst.bundles.empty()) return fail("the arena has no texture set");
  const Model& from = src.models[src_model].model;
  const int first = int(host.meshes.size());
  std::vector<Mesh> add;
  for (const auto& sm : from.meshes) {
    bool empty = true;
    for (const auto& st : sm.strips) empty &= st.indices.size() < 3;
    if (empty || sm.verts.empty()) continue;
    // its texture, into this arena
    const int slot = DiffuseSlot(sm);
    std::string tex = slot >= 0 && slot < int(from.textures.size()) ? from.textures[slot] : "";
    const BundleTexture* st = nullptr;
    for (const auto& b : src.bundles)
      for (const auto& t : b.textures)
        if (t.name == tex) st = &t;
    if (st) {
      const BundleTexture* have = dst.FindTexture(tex);
      if (have && have->data != st->data) {
        // a different texture of that name: a new name
        char nm[32];
        for (int n = 0; n < 1000; ++n) {
          std::snprintf(nm, sizeof nm, "lb%03d_%.9s", n, tex.c_str());
          const BundleTexture* other = dst.FindTexture(nm);
          if (!other || other->data == st->data) break;
        }
        tex = nm;
        have = dst.FindTexture(tex);
      }
      if (!have) {
        dst.bundles[0].textures.push_back({tex, "dds", st->data});
        dst.bundles[0].changed = true;
      }
    }
    Mesh s = *tmpl;  // the host's plain material (its shader and params)
    s.verts = sm.verts;
    s.uvs = sm.uvs;
    s.uvs.resize(s.verts.size());
    s.weights.assign(1, std::vector<Weight>(s.verts.size(), Weight{{0, 0, 0, 0}, 1.f}));
    if (s.palette.empty()) s.palette = {0};
    s.strips = sm.strips;
    if (!tex.empty()) SetDiffuseSlot(s, TextureSlot(host, tex));
    Bound(s);
    add.push_back(std::move(s));
  }
  if (add.empty()) return fail("nothing visible in that model");
  for (auto& s : add) host.meshes.push_back(std::move(s));
  dst.models[host_model].changed = true;
  return first;
}

}  // namespace svrfmt
