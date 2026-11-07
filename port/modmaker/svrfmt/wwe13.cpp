#include "wwe13.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

#include "texture.h"

extern "C" {
#include "mspack.h"
#include "lzx.h"
}

namespace svrfmt {

namespace {

// ------------------------------------------------------------------ LZX (libmspack lzxd over memory)

struct MemFile {
  const uint8_t* buf;
  uint8_t* wbuf;
  size_t size, off;
};
mspack_file* MOpen(mspack_system*, const char*, int) { return nullptr; }
void MClose(mspack_file*) {}
int MRead(mspack_file* f, void* b, int n) {
  auto* m = reinterpret_cast<MemFile*>(f);
  const size_t r = std::min(size_t(n), m->size - std::min(m->size, m->off));
  std::memcpy(b, m->buf + m->off, r);
  m->off += r;
  return int(r);
}
int MWrite(mspack_file* f, void* b, int n) {
  auto* m = reinterpret_cast<MemFile*>(f);
  const size_t r = std::min(size_t(n), m->size - std::min(m->size, m->off));
  std::memcpy(m->wbuf + m->off, b, r);
  m->off += r;
  return n;
}
int MSeek(mspack_file*, off_t, int) { return 0; }
off_t MTell(mspack_file* f) { return off_t(reinterpret_cast<MemFile*>(f)->off); }
void MMsg(mspack_file*, const char*, ...) {}
void* MAlloc(mspack_system*, size_t n) { return std::calloc(n, 1); }
void MFree(void* p) { std::free(p); }
void MCopy(void* s, void* d, size_t n) { std::memmove(d, s, n); }

constexpr size_t B = 8;  // JBOY pointers are offsets from byte 8
constexpr size_t kMesh13 = 0xB8, kNode = 80;

// ------------------------------------------------------------------ materials

const Param* FindParam(const Mesh& s, const char* name) {
  for (const auto& p : s.params)
    if (p.name == name) return &p;
  return nullptr;
}
Param P4(const char* name, const float* v) {
  Param p{name, 0x0d, {}};
  for (int k = 0; k < 4; ++k) AppBeF(p.value, v[k]);
  return p;
}
Param PF(const char* name, float v) {
  Param p{name, 0x0a, {}};
  AppBeF(p.value, v);
  return p;
}
Param PI(const char* name, uint16_t type, int32_t v) {
  Param p{name, type, {}};
  AppBe32(p.value, uint32_t(v));
  return p;
}
float GetF(const Mesh& s, const char* name, float def) {
  const Param* p = FindParam(s, name);
  if (!p || p->value.size() < 4) return def;
  if (p->type == 0x05) return float(int32_t(Be32(p->value.data())));
  return BeF(p->value.data());
}
void Get4(const Mesh& s, const char* name, float* v) {
  const Param* p = FindParam(s, name);
  if (p && p->type == 0x0d && p->value.size() >= 16)
    for (int k = 0; k < 4; ++k) v[k] = BeF(p->value.data() + 4 * k);
}
int GetSlot(const Mesh& s, std::initializer_list<const char*> names) {
  for (const char* n : names) {
    const Param* p = FindParam(s, n);
    if (p && p->type == 0x0f && p->value.size() >= 4) return int32_t(Be32(p->value.data()));
  }
  return -1;
}

bool IsTexParam(const Param& p) { return p.type == 0x0f && p.value.size() >= 4; }

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.4f", v);
  std::string s = b;
  while (s.size() > 1 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
  return s;
}

}  // namespace

// ------------------------------------------------------------------ containers

bool XcompressDecode(const Bytes& d, Bytes& out, std::string* error) {
  auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
  if (!IsXcompress(d)) return fail("not a 0x0FF512ED file");
  // header: magic, u32 1, u32 hash, u32 flags: bits 4-5 chunk = 32 KB << n,
  // bits 6-21 chunk count, bits 22-23 table entry width (20 or 32 bits)
  const uint32_t fl = Be32(&d[12]);
  const uint32_t n = (fl >> 6) & 0xFFFF, bits = (fl & 0xC00000) ? 32 : 20;
  const uint32_t shift = (fl >> 4) & 3;
  const size_t chunk = size_t(0x8000) << shift;
  const int window_bits = 15 + int(shift);
  const size_t table = ((size_t(bits) * n + 31) >> 5) * 4, data = 16 + table;
  if (data > d.size()) return fail("table out of range");
  std::vector<uint32_t> sizes(n);  // unpacked size of each chunk, MSB-first bit packed
  uint64_t total = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t v = 0;
    for (uint32_t b = 0; b < bits; ++b) {
      const uint64_t q = uint64_t(i) * bits + b;
      v = v << 1 | ((d[16 + q / 8] >> (7 - q % 8)) & 1);
    }
    sizes[i] = uint32_t(v);
    total += v;
  }
  out.assign(size_t(total), 0);
  mspack_system sys = {MOpen, MClose, MRead, MWrite, MSeek, MTell, MMsg, MAlloc, MFree, MCopy, nullptr};
  // a chunk's frames from p: {u16 packed size} (32 KB unpacked) or {0xFF, u16 unpacked, u16 packed},
  // read until the chunk's unpacked size; *after = where the next byte is
  auto frames_at = [&](size_t p, size_t unpacked, Bytes& frames, size_t* after) {
    frames.clear();
    size_t got = 0;
    while (got < unpacked && p + 2 <= d.size()) {
      size_t c, u = 0x8000;
      if (d[p] == 0xFF) {
        if (p + 5 > d.size()) return false;
        u = size_t(d[p + 1]) << 8 | d[p + 2];
        c = size_t(d[p + 3]) << 8 | d[p + 4];
        p += 5;
      } else {
        c = size_t(d[p]) << 8 | d[p + 1];
        p += 2;
      }
      if (!c || p + c > d.size()) return false;
      frames.insert(frames.end(), d.begin() + ptrdiff_t(p), d.begin() + ptrdiff_t(p + c));
      if (c & 1) frames.push_back(0);  // the LZX reader works in 16-bit words
      p += c;
      got += u ? u : 0x8000;
    }
    *after = p;
    return got >= unpacked;
  };
  auto decode = [&](const Bytes& frames, uint8_t* dst, uint32_t size) {
    MemFile in{const_cast<uint8_t*>(frames.data()), nullptr, frames.size(), 0};
    MemFile o{nullptr, dst, size, 0};
    lzxd_stream* l = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&in), reinterpret_cast<mspack_file*>(&o),
                               window_bits, 0, 0x8000, off_t(size), 0);
    if (!l) return -1;
    const int r = lzxd_decompress(l, off_t(size));
    lzxd_free(l);
    return r;
  };
  Bytes frames;
  size_t op = 0, next = data;  // (next: right after the previous chunk's frames)
  for (uint32_t i = 0; i < n; ++i) {
    // chunk i: usually at file offset i * chunk (chunk 0 after the header); some files pack them back to back
    const size_t slot = i ? size_t(i) * chunk : data;
    int r = -2;
    size_t after = 0;
    std::vector<size_t> starts{slot};
    if (next != slot) starts.push_back(next);
    for (const size_t start : starts) {
      // a stored chunk (2K14): u16 0, then its bytes as they are
      if (start + 2 + sizes[i] <= d.size() && d[start] == 0 && d[start + 1] == 0) {
        std::memcpy(out.data() + op, &d[start + 2], sizes[i]);
        after = start + 2 + sizes[i], r = 0;
        break;
      }
      if (!frames_at(start, sizes[i], frames, &after)) continue;
      r = decode(frames, out.data() + op, sizes[i]);
      if (!r) break;
    }
    if (r) return fail("LZX error " + std::to_string(r) + " in chunk " + std::to_string(i));
    next = after;
    op += sizes[i];
  }
  return true;
}

// WWE 2K17 arena pacs: xcompressed 'HSPC' instead of EPAC. The stage index is an
// 'SHDC' block in PACH's place: u32 count at +0x38, table at u32 +0x1C of
// {u32 entry id, u32 offset (from the SHDC), u32 size, u32 0}; the entries are
// 2K14's ("ZLIB" packed, the same ids and formats).
bool ShdcRead13(const Bytes& d, size_t s, std::vector<PachEntry>& out);

bool HspcStage13(const Bytes& in, std::vector<PachEntry>& se) {
  Bytes plain;
  const Bytes* d = &in;
  if (IsXcompress(in)) {
    if (!XcompressDecode(in, plain, nullptr)) return false;
    d = &plain;
  }
  if (d->size() < 0x40 || std::memcmp(d->data(), "HSPC", 4)) return false;
  for (size_t s = 0; s + 0x40 <= d->size(); s += 0x800)
    if (!std::memcmp(&(*d)[s], "SHDC", 4)) return ShdcRead13(*d, s, se) && !se.empty();
  return false;
}

// An 'SHDC' block at `s` of `d` -> its entries (as stored)
bool ShdcRead13(const Bytes& d, size_t s, std::vector<PachEntry>& out) {
  if (s + 0x40 > d.size() || std::memcmp(&d[s], "SHDC", 4)) return false;
  const uint32_t n = Le32(&d[s + 0x38]), t = Le32(&d[s + 0x1C]);
  if (s + t + size_t(n) * 16 > d.size()) return false;
  out.clear();
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = &d[s + t + 16 * i];
    const size_t off = s + Le32(r + 4), size = Le32(r + 8);
    if (off + size > d.size()) return false;
    out.push_back({Le32(r), Bytes(d.begin() + off, d.begin() + off + size)});
  }
  return true;
}

bool ArchiveRead(const Bytes& in, Epac& out, std::string* error) {
  auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
  Bytes plain;
  const Bytes* d = &in;
  if (IsXcompress(in)) {
    if (!XcompressDecode(in, plain, error)) return false;
    d = &plain;
  }
  if (d->size() < 0x4000) return fail("too small");
  if (!std::memcmp(d->data(), "EPAC", 4)) return EpacRead(*d, out) || fail("bad EPAC");
  if (std::memcmp(d->data(), "EPK8", 4)) return fail("not an EPAC / EPK8 archive");
  return EpacRead(*d, out) || fail("bad EPK8");  // (16-byte entries, 8-char names: pac.h)
}

// ------------------------------------------------------------------ JBOY

bool IsJboy13(const Bytes& d) {
  if (!IsJboy(d)) return false;
  const uint32_t nmesh = Be32(&d[0x18]), mptr = Be32(&d[0x1C]);
  const size_t o = B + mptr;
  if (!nmesh || o + kMesh13 > d.size()) return false;
  return Be32(&d[o + 0x78]) < 0x10000 && d[o + 0x7C] == 'y';
}

bool Jboy13Read(const Bytes& d, Model& m, std::string* error) {
  auto fail = [&](const char* why) { if (error) *error = why; return false; };
  if (!IsJboy(d)) return fail("not JBOY");
  if (!IsJboy13(d)) return JboyRead(d, m, error);  // (no meshes, or already SvR2011 layout)
  const size_t len = Be32(&d[4]);
  if (8 + len > d.size()) return fail("length");
  auto in = [&](size_t o, size_t n) { return o + n <= 8 + len; };
  m = {};
  m.header.assign(d.begin() + 8, d.begin() + 0x48);
  const uint32_t nmesh = Be32(&d[0x18]), mptr = Be32(&d[0x1C]), nnode = Be32(&d[0x20]), ntex = Be32(&d[0x24]);
  const uint32_t nptr = Be32(&d[0x28]), tptr = Be32(&d[0x2C]), optr = Be32(&d[0x30]);
  if (!in(B + optr, 32) || !in(B + tptr, 16 * size_t(ntex)) || !in(B + nptr, kNode * nnode) ||
      !in(B + mptr, kMesh13 * nmesh))
    return fail("table out of range");
  m.name = Name16(&d[B + optr]);
  m.group.assign(d.begin() + B + optr + 16, d.begin() + B + optr + 32);
  for (uint32_t i = 0; i < ntex; ++i) m.textures.push_back(Name16(&d[B + tptr + 16 * i]));
  for (uint32_t i = 0; i < nnode; ++i) {
    const uint8_t* o = &d[B + nptr + kNode * i];
    Node n;
    n.name = Name16(o);
    for (int k = 0; k < 3; ++k) { n.t[k] = BeF(o + 16 + 4 * k); n.r[k] = BeF(o + 32 + 4 * k); }
    n.pad28 = Be32(o + 28);
    n.pad44 = Be32(o + 44);
    n.parent = int32_t(Be32(o + 48));
    for (int k = 0; k < 3; ++k) n.u52[k] = Be32(o + 52 + 4 * k);
    for (int k = 0; k < 4; ++k) n.sphere[k] = BeF(o + 64 + 4 * k);
    m.nodes.push_back(n);
  }
  for (uint32_t i = 0; i < nmesh; ++i) {
    const size_t o13 = B + mptr + kMesh13 * i;
    Mesh s;
    // the SvR2011 descriptor: WWE '13's without its +0x74 word
    s.raw.assign(d.begin() + o13, d.begin() + o13 + 0x74);
    s.raw.insert(s.raw.end(), d.begin() + o13 + 0x78, d.begin() + o13 + kMesh13);
    const uint8_t* q = s.raw.data();
    const uint32_t vc = Be32(q), nstrip = Be32(q + 4), npal = Be32(q + 8);
    for (uint32_t k = 0; k < npal && k < 20; ++k) s.palette.push_back(int32_t(Be32(q + 0x0C + 4 * k)));
    const uint32_t nw = std::max<uint32_t>(1, Be32(q + 0x5C));
    const uint32_t vblk = Be32(q + 0x68), wptr = Be32(q + 0x6C), uvptr = Be32(q + 0x70);
    s.material = Be32(q + 0x74);
    s.shader = Name16(q + 0x78);
    s.vfmt = Be32(q + 0x8C);
    const uint32_t npar = Be32(q + 0x90), parptr = Be32(q + 0x94), iptr = Be32(q + 0x98);
    for (int k = 0; k < 4; ++k) s.sphere[k] = BeF(q + 0xA4 + 4 * k);
    if (!in(B + vblk, 4)) return fail("vertex header out of range");
    const size_t vd = B + Be32(&d[B + vblk]);
    if (!in(vd, 28 * size_t(vc)) || !in(B + wptr, 8 * size_t(vc) * nw) || !in(B + uvptr, 8 * size_t(vc)))
      return fail("vertex data out of range");
    s.verts.resize(vc);
    s.uvs.resize(vc);
    for (uint32_t k = 0; k < vc; ++k) {
      const uint8_t* v = &d[vd + 28 * k];
      for (int c = 0; c < 3; ++c) { s.verts[k].pos[c] = BeF(v + 4 * c); s.verts[k].normal[c] = BeF(v + 12 + 4 * c); }
      s.verts[k].color = Be32(v + 24);
      s.uvs[k] = {BeF(&d[B + uvptr + 8 * k]), BeF(&d[B + uvptr + 8 * k + 4])};
    }
    for (uint32_t j = 0; j < nw; ++j) {
      std::vector<Weight> blk(vc);
      for (uint32_t k = 0; k < vc; ++k) {
        const uint8_t* w = &d[B + wptr + 8 * (size_t(nw) * k + j)];
        std::memcpy(blk[k].bones, w, 4);
        blk[k].weight = BeF(w + 4);
      }
      s.weights.push_back(std::move(blk));
    }
    if (!in(B + parptr, 4 * size_t(npar))) return fail("params out of range");
    for (uint32_t k = 0; k < npar; ++k) {
      const size_t po = B + Be32(&d[B + parptr + 4 * k]);
      if (!in(po, 20)) return fail("param out of range");
      Param p;
      p.name = Name16(&d[po]);
      p.type = Be16(&d[po + 16]);
      const uint16_t size = Be16(&d[po + 18]);
      if (size < 20 || !in(po, size)) return fail("param size");
      p.value.assign(d.begin() + po + 20, d.begin() + po + size);
      s.params.push_back(std::move(p));
    }
    if (!in(B + iptr, 12 * size_t(nstrip))) return fail("strips out of range");
    for (uint32_t k = 0; k < nstrip; ++k) {
      const uint8_t* h = &d[B + iptr + 12 * k];
      Strip st;
      st.prim = Be32(h);
      // WWE '13 stores the primitive type byte-swapped (06 00 00 00 = strip);
      // SvR2011 reads 0x06000000 as an unknown primitive and draws stray
      // rectangles (the grey blocks across the canvas)
      if (st.prim > 0xFFFF) st.prim = Le32(h);
      const uint32_t cnt = Be32(h + 4), at = Be32(h + 8);
      if (!in(B + at, 2 * size_t(cnt))) return fail("indices out of range");
      for (uint32_t z = 0; z < cnt; ++z) st.indices.push_back(Be16(&d[B + at + 2 * z]));
      s.strips.push_back(std::move(st));
    }
    m.meshes.push_back(std::move(s));
  }
  return true;
}

// WWE '12 / '13 / 2K14 light their floors, stands and props with baked light
// maps (sampLightMapPrm). The floors' (fade_lightmap*, 512 x 512) are top-down
// pictures of the whole floor placed by world position - the stage side at the
// top - over the floor the meshes using them cover; the others are sampled
// with the mesh's UVs. SvR2011 has no light
// map shader and bakes its own arena lighting into vertex colours (its floors:
// vertex format bit 1, greys). The light map is sampled at each vertex (UV
// wrapped, bilinear) and multiplied into the vertex colour, which turns the
// colour bit on - without it those meshes came out near black (the user's
// "black floors").
struct LightMap13 {
  Image img;
  bool top_down = false;
  float x0 = 0, x1 = 1, z0 = 0, z1 = 1;  // the floor area it covers (source units)
};

void BakeLightMaps13(Model& m, const std::map<std::string, LightMap13>& lightmaps, int& baked) {
  for (auto& s : m.meshes) {
    const int slot = GetSlot(s, {"sampLightMapPrm"});
    if (slot < 0 || slot >= int(m.textures.size())) continue;
    auto it = lightmaps.find(Lower(m.textures[slot]));
    if (it == lightmaps.end() || it->second.img.w <= 0 || s.uvs.size() != s.verts.size()) continue;
    const LightMap13& L = it->second;
    const Image& lm = L.img;
    auto px = [&](int x, int y, int c) {
      if (L.top_down) {
        x = std::clamp(x, 0, lm.w - 1);
        y = std::clamp(y, 0, lm.h - 1);
      } else {
        x = ((x % lm.w) + lm.w) % lm.w;
        y = ((y % lm.h) + lm.h) % lm.h;
      }
      return float(lm.rgba[(size_t(y) * lm.w + x) * 4 + c]);
    };
    for (size_t k = 0; k < s.verts.size(); ++k) {
      float u = s.uvs[k][0], v = s.uvs[k][1];
      if (L.top_down) {
        u = (s.verts[k].pos[0] - L.x0) / std::max(1e-3f, L.x1 - L.x0);
        v = (L.z1 - s.verts[k].pos[2]) / std::max(1e-3f, L.z1 - L.z0);
        u = std::clamp(u, 0.f, 0.999f), v = std::clamp(v, 0.f, 0.999f);
      }
      const float fx = u * lm.w - 0.5f, fy = v * lm.h - 0.5f;
      const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
      const float ax = fx - x0, ay = fy - y0;
      uint32_t& col = s.verts[k].color;
      uint32_t out = col & 0xFF000000u;
      for (int c = 0; c < 3; ++c) {
        const float v = (px(x0, y0, c) * (1 - ax) + px(x0 + 1, y0, c) * ax) * (1 - ay) +
                        (px(x0, y0 + 1, c) * (1 - ax) + px(x0 + 1, y0 + 1, c) * ax) * ay;
        const int shift = 16 - 8 * c;  // ARGB: R 16, G 8, B 0
        // (the source's own vertex colours are unused there - often black: replaced; light
        // maps hold light at half scale, as SvR2011's vertex colours do)
        out |= uint32_t(std::clamp(int(std::lround(v * 2.f)), 0, 255)) << shift;
      }
      col = out;
    }
    s.vfmt |= 2u;
    ++baked;
  }
}

void ConvertModel13(Model& m, float scale, Convert13Stats& st) {
  ++st.models;
  for (auto& n : m.nodes) {
    for (float& f : n.t) f *= scale;
    for (float& f : n.sphere) f *= scale;
  }
  // texture slots in use, renumbered
  std::vector<std::string> used;
  auto slot_of = [&](int old) -> int {
    if (old < 0 || old >= int(m.textures.size())) return -1;
    for (size_t i = 0; i < used.size(); ++i)
      if (used[i] == m.textures[old]) return int(i);
    used.push_back(m.textures[old]);
    return int(used.size() - 1);
  };
  for (auto& s : m.meshes) {
    ++st.meshes;
    for (auto& v : s.verts)
      for (float& f : v.pos) f *= scale;
    for (float& f : s.sphere) f *= scale;
    if (FindParam(s, "sampLightMapPrm") && GetSlot(s, {"sampLightMapPrm"}) >= 0) ++st.lightmaps_dropped;

    const bool scroll = s.shader.find("UVScroll") != std::string::npos;
    float amb[4] = {1, 1, 1, 1}, dif[4] = {1, 1, 1, 1}, spc[4] = {1, 1, 1, 1};
    Get4(s, "g_f4MatAmbCol", amb);
    Get4(s, "g_f4MatDifCol", dif);
    Get4(s, "g_f4SpecularCol", spc);
    // (WWE '13 drives specular up to 1.5 under its own lights; SvR2011's ring
    // lights make that a silver sheen: at most 0.5, as SvR2011's own parts)
    const float lev = std::min(GetF(s, "g_fSpecularLev", 0.5f), 0.5f);
    const int pow = int(std::lround(GetF(s, "g_iSpecularPow", 5.f)));
    const int diffuse = slot_of(GetSlot(s, {"texDiffuse", "sampDiffusePrm", "g_texDiffuse"}));
    const int specular = slot_of(GetSlot(s, {"texSpecularMap", "sampSpecularPrm"}));
    std::vector<Param> ps = {P4("g_f4MatAmbCol", amb), P4("g_f4MatDifCol", dif), P4("g_f4SpecularCol", spc),
                             PF("g_fSpecularLev", lev), PI("g_iSpecularPow", 0x05, pow),
                             PF("g_fHDRAlpha", GetF(s, "g_fHDRAlpha", 1.f))};
    if (scroll) {
      ++st.uvscroll;
      for (const char* n : {"g_fScrollU", "g_fScrollV", "g_fScrollU2", "g_fScrollV2", "g_fScrollU3", "g_fScrollV3",
                            "g_fScrollU4", "g_fScrollV4"})
        ps.push_back(PF(n, GetF(s, n, 0.f)));
      ps.push_back(PF("g_fSL_Scale", GetF(s, "g_fSL_Scale", 0.2f)));
      const int l2 = slot_of(GetSlot(s, {"texUVScroll2", "sampDiffuse1Prm"}));
      const int l3 = slot_of(GetSlot(s, {"texUVScroll3", "sampDiffuse2Prm"}));
      const int l4 = slot_of(GetSlot(s, {"texUVScroll4", "sampDiffuse3Prm"}));
      ps.push_back(PI("texDiffuse", 0x0f, diffuse));
      ps.push_back(PI("texSpecularMap", 0x0f, specular));
      ps.push_back(PI("texUVScroll2", 0x0f, l2));
      ps.push_back(PI("texUVScroll3", 0x0f, l3));
      ps.push_back(PI("texUVScroll4", 0x0f, l4));
      s.shader = "yUVScroll";
      s.vfmt = 4;
    } else {
      ps.push_back(PI("texDiffuse", 0x0f, diffuse));
      ps.push_back(PI("texSpecularMap", 0x0f, specular));
      ps.push_back(PI("g_bUseGlow", 0x10, 0));
      ps.push_back(PI("texGlowMap", 0x0f, -1));
      s.shader = "yDefault";
      s.vfmt &= 3;
    }
    s.params = std::move(ps);
  }
  // WWE '13's header screen video has no SvR2011 counterpart: the arena video. WWE 2K15 - 2K17 name their screens
  // layout_<show>_movie (the titantron: the entrance videos - raw, ss, nxt, me ...), layout_arena_movie and
  // layout_mini_tron (the arena's own loop), cut to 15 characters: names SvR2011 doesn't know play the live camera,
  // in entrances too. WWE 2K14 names them main&banner (the titantron), <show>_header&wall and mini_tron (loops); WWE
  // '12 minitron_movie - any other video name (…movie, never stored) is a loop.
  for (auto& t : used) {
    const std::string l = Lower(t);
    if (l == "titantron_movie" || l == "arena_movie") continue;
    if ((l.rfind("layout_", 0) == 0 && l.find("_mov", 6) != std::string::npos && l.rfind("layout_arena_mo", 0) != 0) ||
        l == "main&banner")
      t = "titantron_movie";
    else if (l == "header_wall_mov" || l.rfind("layout_", 0) == 0 && l.find("_mov", 6) != std::string::npos ||
             l.rfind("layout_mini_tro", 0) == 0 || l == "mini_tron" || l.find("movie") != std::string::npos ||
             (l.size() > 12 && l.compare(l.size() - 12, 12, "_header&wall") == 0))
      t = "arena_movie";
  }
  if (!m.meshes.empty()) m.textures = used;  // (a model without meshes keeps its list)
}

Bytes ConvertHmd13(const Bytes& raw, float scale) {
  Bytes out = raw;
  if (out.size() < 8 || std::memcmp(out.data(), "HMD ", 4)) return out;
  const uint32_t n = Le32(&out[4]);
  auto sc = [&](size_t at) {
    float f = LeF(&out[at]) * scale;
    uint32_t v;
    std::memcpy(&v, &f, 4);
    PutLe32(&out[at], v);
  };
  for (uint32_t i = 0; i < n && 8 + 96 * size_t(i + 1) <= out.size(); ++i) {
    const size_t r = 8 + 96 * size_t(i);
    // 4x4 frame: rows 0-2 = axis + half size, row 3 = position; then a 2D
    // outline (7 floats) and a flags word
    for (int k : {3, 7, 11, 12, 13, 14}) sc(r + 4 * k);
    for (int k = 16; k < 23; ++k) sc(r + 4 * k);
  }
  return out;
}

std::string ConvertFlags13(const std::string& t) {
  // (the light numbers l(n) stay: the arena keeps its own light bank 0x7530,
  // see LightBank13)
  std::string o;
  for (size_t i = 0; i < t.size();) {
    // "p" / "pg" / "pe": the model drops a shadow. WWE '13 marks its floor
    // pieces and mats; SvR2011 projects those shadows flat onto the canvas
    // (grey slabs and dashes on the mat): left out
    const bool word_start = i == 0 || t[i - 1] == ' ' || t[i - 1] == '\t';
    if (word_start && t[i] == 'p') {
      size_t e = i + 1;
      if (e < t.size() && (t[e] == 'g' || t[e] == 'e')) ++e;
      if (e >= t.size() || t[e] == ' ' || t[e] == '\t' || t[e] == '\r' || t[e] == '\n') {
        // only on model lines (they start with a number)
        size_t ls = t.rfind('\n', i);
        ls = ls == std::string::npos ? 0 : ls + 1;
        while (ls < i && (t[ls] == ' ' || t[ls] == '\t')) ++ls;
        if (ls < i && std::isdigit(static_cast<unsigned char>(t[ls]))) {
          i = e;
          continue;
        }
      }
    }
    o += t[i++];
  }
  return o;
}

// Arena lights: entry 0x7530 is the arena's bank of light sets (YMKs, LE:
// [16-byte header], event-size table T[256], u32 count, count x {u8 y, u8 x,
// u16 set id, u32 offset, u32 frames, u32 rt}, key streams). A set: ground /
// sky colour (y 0 / 1), up to 4 directional lights (y 10-13), moving crowd
// lights (y 50-59). The model flags (0xC351 l(n)) and the light zones (0xC353)
// pick sets by id; an id the bank lacks is index -1 in the game. WWE '12 /
// '13 / 2K14 banks have the SvR2011 format and id plan (their y 14 track is
// ignored), but lack sets the game looks up itself (110, 115) and some the
// flags name (111, and RAW's 102 / 108 given by RelightByRole13).
constexpr int kExeLights13[] = {10, 11, 12, 13, 14, 20, 30, 31, 100, 110, 115, 500};

struct LightDir13 { uint8_t y, x; uint16_t id; uint32_t off, frames, rt; };

bool LightBankRead13(const Bytes& b, size_t& head, std::vector<LightDir13>& dirs, size_t& data) {
  head = (b.size() >= 4 && (!std::memcmp(b.data(), "YMKs", 4) || !std::memcmp(b.data(), "YMBs", 4))) ? 16 : 0;
  if (b.size() < head + 0x104) return false;
  const uint32_t n = Le32(&b[head + 0x100]);
  data = head + 0x104 + size_t(n) * 16;
  if (n > 100000 || data > b.size()) return false;
  dirs.clear();
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = &b[head + 0x104 + 16 * i];
    dirs.push_back({r[0], r[1], Le16(r + 2), Le32(r + 4), Le32(r + 8), Le32(r + 12)});
  }
  return true;
}

std::set<int> LightIds13(const Bytes& bank) {
  size_t head, data;
  std::vector<LightDir13> dirs;
  std::set<int> ids;
  if (LightBankRead13(bank, head, dirs, data))
    for (const auto& d : dirs) ids.insert(d.id);
  return ids;
}

// The sets in `want` the bank lacks are copied from the host's (2011 RAW's)
// bank: directory records appended, their key streams after the data. Sets
// neither the host has nor `want` names are dropped (unused): every set costs
// the game ~18 KB of stage memory (two 8608-byte players and a 928-byte
// object), and WWE 2K14's 52 crashed its arena load (in the object motion
// loader sub_82183C10, after the light sets); with 41 or 47 it plays.
Bytes FillLights13(const Bytes& bank, const Bytes& host, const std::set<int>& want, std::vector<int>& added,
                   std::vector<int>& unused, const std::set<int>& replace = {}) {
  size_t head, data, hh, hdata;
  std::vector<LightDir13> dirs, hd;
  if (!LightBankRead13(bank, head, dirs, data) || !LightBankRead13(host, hh, hd, hdata)) return bank;
  std::set<int> host_ids;
  for (const auto& d : hd) host_ids.insert(d.id);
  {
    std::vector<LightDir13> keep;
    for (const auto& d : dirs)
      if (replace.count(d.id)) continue;  // (the host's instead)
      else if (host_ids.count(d.id) || want.count(d.id)) keep.push_back(d);
      else if (unused.empty() || unused.back() != d.id) unused.push_back(d.id);
    dirs.swap(keep);
  }
  std::set<int> have;
  for (const auto& d : dirs) have.insert(d.id);
  std::set<uint32_t> hoffs;
  for (const auto& d : hd) hoffs.insert(d.off);
  const size_t hlen = host.size() - hdata;
  Bytes extra;
  std::map<uint32_t, uint32_t> moved;  // host offset -> new offset (shared streams stay shared)
  const uint32_t base = uint32_t(bank.size() - data);
  for (int id : want) {
    if (have.count(id)) continue;
    bool any = false;
    for (const auto& d : hd) {
      if (d.id != id || d.off > hlen) continue;
      auto mv = moved.find(d.off);
      if (mv == moved.end()) {
        auto nx = hoffs.upper_bound(d.off);
        const size_t end = (nx == hoffs.end() || *nx > hlen) ? hlen : *nx;
        mv = moved.emplace(d.off, base + uint32_t(extra.size())).first;
        App(extra, &host[hdata + d.off], end - d.off);
      }
      dirs.push_back({d.y, d.x, d.id, mv->second, d.frames, d.rt});
      any = true;
    }
    if (any) added.push_back(id);
  }
  if (added.empty() && unused.empty()) return bank;
  Bytes out(bank.begin(), bank.begin() + head + 0x100);
  AppLe32(out, uint32_t(dirs.size()));
  for (const auto& d : dirs) {
    out.push_back(d.y), out.push_back(d.x), out.push_back(uint8_t(d.id)), out.push_back(uint8_t(d.id >> 8));
    AppLe32(out, d.off), AppLe32(out, d.frames), AppLe32(out, d.rt);
  }
  out.insert(out.end(), bank.begin() + data, bank.end());
  App(out, extra);
  return out;
}

// A light set's first keys (25-byte pose: u8 count, f32 x3 position, s16 x3
// rotation, u16 x3 colour / 4096; after an event block when the record's
// first byte has bit 7): ground / sky colour (y 0 / 1, the RGB mean) and
// whether a directional light (y 10-13) has colour.
LightLevel LightLevelOf(const Bytes& bank, int id) {
  LightLevel lv;
  size_t head, data;
  std::vector<LightDir13> dirs;
  if (!LightBankRead13(bank, head, dirs, data)) return lv;
  std::set<uint32_t> offs;
  for (const auto& d : dirs) offs.insert(d.off);
  for (const auto& d : dirs) {
    if (d.id != id) continue;
    lv.found = true;
    if (data + d.off >= bank.size()) continue;
    auto nx = offs.upper_bound(d.off);
    const size_t end = nx == offs.end() ? bank.size() : std::min(bank.size(), data + *nx);
    // the stream's first record (b < 0x60 skips frames; 0x60 + u16 or b - 0x60: a record's size)
    size_t p = data + d.off;
    const uint8_t* rec = nullptr;
    size_t len = 0;
    while (p < end) {
      const uint8_t c = bank[p];
      if (c < 0x60) { ++p; continue; }
      if (c == 0x60) {
        if (p + 3 > end) break;
        len = Le16(&bank[p + 1]), p += 3;
      } else {
        len = c - 0x60, p += 1;
      }
      if (p + len <= end) rec = &bank[p];
      break;
    }
    if (!rec || !len) continue;
    if (rec[0] & 0x80) {
      const size_t ev = rec[0] & 0x7F;
      if (ev + 1 >= len) continue;
      rec += 1 + ev, len -= 1 + ev;
    }
    if (len < 25) continue;
    const float c = (Le16(rec + 19) + Le16(rec + 21) + Le16(rec + 23)) / (3.0f * 4096.0f);
    if (d.y == 0) lv.ground = c;
    if (d.y == 1) lv.sky = c;
    if (d.y >= 10 && d.y <= 13 && c > 0.01f) lv.directional = true;
  }
  return lv;
}

std::set<int> DarkLightSets(const Bytes& bank) {
  std::set<int> ids;
  const LightLevel match = LightLevelOf(bank, 10);
  if (match.found && match.ground >= 0 && match.ground < kDarkMatchGround)
    for (int id = 10; id <= 14; ++id) ids.insert(id);
  const LightLevel def = LightLevelOf(bank, 100);
  if (def.found && !def.directional && def.ground + def.sky < kDarkDefaultAmbient) ids.insert(100);
  return ids;
}

Bytes ReplaceLightSets(const Bytes& bank, const Bytes& host, const std::set<int>& ids, std::vector<int>* replaced) {
  std::set<int> want = LightIds13(bank);
  want.insert(ids.begin(), ids.end());
  std::vector<int> added, unused;
  Bytes out = FillLights13(bank, host, want, added, unused, ids);
  if (replaced) *replaced = added;
  return out;
}

// The l(n) / le(n) / lg(n) numbers of a flags text
std::set<int> FlagLights13(const std::string& t) {
  std::set<int> ids;
  for (size_t i = t.find('('); i != std::string::npos; i = t.find('(', i + 1)) {
    size_t s = i;
    if (s >= 1 && (t[s - 1] == 'e' || t[s - 1] == 'g')) --s;
    if (s >= 1 && t[s - 1] == 'l' && (s == 1 || t[s - 2] == ' ' || t[s - 2] == '\t'))
      ids.insert(std::atoi(t.c_str() + i + 1));
  }
  return ids;
}

// The light sets a zones text (0xC353) names
std::set<int> ZoneLights13(const std::string& t) {
  std::set<int> ids;
  size_t i = 0;
  while (i < t.size()) {
    size_t e = t.find('\n', i);
    if (e == std::string::npos) e = t.size();
    const std::string line = t.substr(i, e - i);
    i = e + 1;
    float v[4];
    int l[4];
    if (line.compare(0, 2, "//") != 0 &&
        std::sscanf(line.c_str(), "%f %f %f %f %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &l[0], &l[1], &l[2], &l[3]) == 8)
      for (int k : l) ids.insert(k);
  }
  return ids;
}

// WWE '12 / '13 / 2K14 light zones (0xC353: "x0 z0 x1 z1 L0 L1 L2 ambient", the
// entrance stage and ramp) are in cm, SvR2011's in its arena units (x 0.1)
std::string ScaleZones13(const std::string& t, float k) {
  std::string o;
  size_t i = 0;
  while (i < t.size()) {
    size_t e = t.find('\n', i);
    if (e == std::string::npos) e = t.size(); else ++e;
    std::string line = t.substr(i, e - i), eol;
    i = e;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) eol.insert(eol.begin(), line.back()), line.pop_back();
    float v[4];
    int l[4];
    const size_t c = line.find(';');
    const std::string body = line.substr(0, c);
    if (line.compare(0, 2, "//") != 0 &&
        std::sscanf(body.c_str(), "%f %f %f %f %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &l[0], &l[1], &l[2], &l[3]) == 8) {
      char buf[160];
      std::snprintf(buf, sizeof buf, "%7.1f  %7.1f   %7.1f  %7.1f    %d  %d  %d  %d", v[0] * k, v[1] * k, v[2] * k, v[3] * k,
                    l[0], l[1], l[2], l[3]);
      const std::string comment = c == std::string::npos ? "" : "     " + line.substr(c);
      line = buf + comment;
    }
    o += line + eol;
  }
  return o;
}

// Light numbers l(n) the shipped bank has no set for go to its nearest one of
// the same hundred.
std::string BankLights13(const std::string& t, const std::set<int>& have) {
  if (have.empty()) return t;
  std::string o;
  for (size_t i = 0; i < t.size();) {
    size_t k = i + 1;
    if (t[i] == 'l' && k < t.size() && (t[k] == 'e' || t[k] == 'g')) ++k;
    if (t[i] == 'l' && k < t.size() && t[k] == '(' && (i == 0 || t[i - 1] == ' ' || t[i - 1] == '	')) {
      const size_t e = t.find(')', i);
      if (e != std::string::npos && e - i < 9) {
        const std::string word = t.substr(i, k - i);
        int v = std::atoi(t.c_str() + k + 1);
        if (!have.count(v)) {
          int best = -1;
          for (int h : have)
            if (h / 100 == v / 100 && (best < 0 || std::abs(h - v) < std::abs(best - v))) best = h;
          if (best < 0)
            for (int h : have)
              if (best < 0 || std::abs(h - v) < std::abs(best - v)) best = h;
          v = best;
        }
        o += word + "(" + std::to_string(v) + ")";
        i = e + 1;
        continue;
      }
    }
    o += t[i++];
  }
  return o;
}

std::string RelightByRole13(const std::string& t, const std::map<uint32_t, std::string>& names, int stand_light) {
  // WWE '13 lights its stands with baked light maps and no light group; the
  // light maps are gone, so they take the lights SvR2011's RAW gives the
  // same parts (stands 108, barrier and floor mats 102)
  auto light_for = [stand_light](const std::string& n) -> int {
    auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
    if (starts("Sta_Fog") || n.find("glow") != std::string::npos) return 0;
    if (starts("Sta_") || starts("Vip_") || starts("arena_chair") || starts("Pas_light")) return stand_light;
    if (starts("ar_fence") || starts("ar_dwnmt") || starts("ar_sak") || starts("ji_")) return 102;
    return 0;
  };
  std::string o;
  size_t pos = 0;
  while (pos < t.size()) {
    size_t nl = t.find('\n', pos);
    std::string line = t.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    const bool has_nl = nl != std::string::npos;
    pos = has_nl ? nl + 1 : t.size();
    size_t a = 0;
    while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
    size_t b = a;
    while (b < line.size() && std::isdigit(static_cast<unsigned char>(line[b]))) ++b;
    if (b > a && (b == line.size() || line[b] == ' ' || line[b] == '\t' || line[b] == '\r')) {
      auto it = names.find(uint32_t(std::atoi(line.c_str() + a)));
      const int l = it != names.end() ? light_for(it->second) : 0;
      if (l) {
        bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        // drop any l(n), then add the new one
        for (size_t p = line.find("l(", b); p != std::string::npos; p = line.find("l(", b)) {
          const size_t e = line.find(')', p);
          if (e == std::string::npos) break;
          line.erase(p, e + 1 - p);
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        line += " l(" + std::to_string(l) + ")";
        if (cr) line += '\r';
      }
    }
    o += line;
    if (has_nl) o += '\n';
  }
  return o;
}

std::string NoSpotShadows13(const std::string& t, const std::map<uint32_t, std::string>& names) {
  // flag "x": the model drops no spotlight shadow. SvR2011's ring spotlights
  // project the arena's models (barriers, flags, chairs) flat onto the canvas
  // as grey slabs and dashes; WWE '13 bakes its lighting instead. Every
  // converted model line gets "x" (lines without a model are left alone).
  std::string o;
  size_t pos = 0;
  while (pos < t.size()) {
    size_t nl = t.find('\n', pos);
    std::string line = t.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    const bool has_nl = nl != std::string::npos;
    pos = has_nl ? nl + 1 : t.size();
    size_t a = 0;
    while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
    size_t b = a;
    while (b < line.size() && std::isdigit(static_cast<unsigned char>(line[b]))) ++b;
    if (b > a && (b == line.size() || line[b] == ' ' || line[b] == '\t' || line[b] == '\r') &&
        names.count(uint32_t(std::atoi(line.c_str() + a)))) {
      const bool cr = !line.empty() && line.back() == '\r';
      if (cr) line.pop_back();
      bool has_x = false;
      for (size_t p = b; p < line.size(); ++p)
        if (line[p] == 'x' && (line[p - 1] == ' ' || line[p - 1] == '\t') &&
            (p + 1 == line.size() || line[p + 1] == ' ' || line[p + 1] == 'g' || line[p + 1] == 'e'))
          has_x = true;
      if (!has_x) line += " x";
      if (cr) line += '\r';
    }
    o += line;
    if (has_nl) o += '\n';
  }
  // models with no line at all
  for (const auto& [id, name] : names) {
    const std::string key = "\n" + std::to_string(id) + " ";
    if (o.find(key) == std::string::npos && o.rfind(std::to_string(id) + " ", 0) != 0)
      o += std::to_string(id) + " x\r\n";
  }
  return o;
}

std::string ConvertVisTree13(const std::string& t, float scale) {
  // "NODE MY_NAME = x PARENT_NAME = y" then "{" box (min xyz, max xyz) "}"
  std::string o;
  bool in_box = false;
  size_t pos = 0;
  while (pos < t.size()) {
    size_t nl = t.find('\n', pos);
    std::string line = t.substr(pos, nl == std::string::npos ? std::string::npos : nl + 1 - pos);
    pos = nl == std::string::npos ? t.size() : nl + 1;
    const std::string tl = Trim(line);
    if (tl.rfind("{", 0) == 0) {
      in_box = true;
    } else if (tl.rfind("}", 0) == 0) {
      in_box = false;
    } else if (in_box && !tl.empty() && tl[0] != ';') {
      std::string r;
      const char* p = line.c_str();
      while (*p) {
        char* e;
        const double v = std::strtod(p, &e);
        if (e != p && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '.')) {
          r += Num(v * scale);
          p = e;
        } else {
          r += *p++;
        }
      }
      line = r;
    }
    o += line;
  }
  return o;
}

// A crowd area file: no "(" seat entries; its first data line is numbers
bool IsCrowdArea13(const std::string& t) {
  if (t.find('(') != std::string::npos) return false;
  size_t pos = 0;
  while (pos < t.size()) {
    size_t nl = t.find('\n', pos);
    const std::string tl = Trim(t.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
    pos = nl == std::string::npos ? t.size() : nl + 1;
    if (tl.empty() || tl[0] == ';') continue;
    return std::isdigit(static_cast<unsigned char>(tl[0])) || tl[0] == '-' || tl[0] == '.';
  }
  return false;
}

std::string ConvertCrowdText13(const std::string& t, float scale) {
  // seat files: "A (x, z, y, w); count" -> all four scaled. Area files: rows
  // of numbers, all positions ("; R" / "; NR"; WWE 2K14 names them freely -
  // "; off_scr_audience_r", "; audience_a", "; kyauku_a" ... - and those left
  // in cm crashed the crowd code): told by their content
  const bool area = IsCrowdArea13(t);
  std::string o;
  size_t pos = 0;
  while (pos < t.size()) {
    size_t nl = t.find('\n', pos);
    std::string line = t.substr(pos, nl == std::string::npos ? std::string::npos : nl + 1 - pos);
    pos = nl == std::string::npos ? t.size() : nl + 1;
    const std::string tl = Trim(line);
    if (tl.empty() || tl[0] == ';' || tl[0] == '{' || tl[0] == '}' || tl[0] == '[') { o += line; continue; }
    if (area) {
      std::string r;
      const char* p = line.c_str();
      while (*p) {
        char* e;
        const double v = std::strtod(p, &e);
        if (e != p && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '.')) {
          r += Num(v * scale);
          p = e;
        } else {
          r += *p++;
        }
      }
      o += r;
      continue;
    }
    const size_t a = line.find('('), b = line.find(')');
    if (a == std::string::npos || b == std::string::npos || b < a) { o += line; continue; }
    std::string inner = line.substr(a + 1, b - a - 1), r;
    int k = 0;
    size_t q = 0;
    while (q <= inner.size()) {
      size_t c = inner.find(',', q);
      const std::string tok = inner.substr(q, c == std::string::npos ? std::string::npos : c - q);
      const double v = std::atof(tok.c_str());
      // (all four: the fourth is in the same units. SvR2011's RAW seat file
      // has the same seat rows as WWE '13's with "M (80.0, 67.5, 0.0, -9.0)"
      // where WWE '13 has "M (800.0, 675.0, 0.0, -90.0)"; left at -90 the
      // side stands' rows M-P / p-s, about 140 people, were not seated)
      r += (k ? ", " : "") + (k < 4 ? Num(v * scale) : Trim(tok));
      ++k;
      if (c == std::string::npos) break;
      q = c + 1;
    }
    o += line.substr(0, a + 1) + r + line.substr(b);
  }
  return o;
}

// ------------------------------------------------------------------ arena

namespace {

bool InRing(uint32_t id) { return id >= 0x3B7 && id <= 0x3D6; }

std::string Text(const Bytes& b) { return std::string(b.begin(), b.end()); }
Bytes FromText(const std::string& s) { return Bytes(s.begin(), s.end()); }

// BPE, but never larger than the data (the game unpacks in place)
Bytes Pack(const Bytes& raw) {
  Bytes p = BpeEncode(raw);
  return p;
}

void CollectTextures(const Model& m, std::set<std::string>& names) {
  for (const auto& s : m.meshes)
    for (const auto& p : s.params)
      if (IsTexParam(p)) {
        const int slot = int32_t(Be32(p.value.data()));
        if (slot >= 0 && slot < int(m.textures.size())) names.insert(m.textures[slot]);
      }
}

// ---- ring parts: WWE '13's meshes on SvR2011's ring skeletons
// SvR2011 draws its ring from fixed models (posts as one model of four, a
// turnbuckle, the mat, one rope per side, a pad per corner, two aprons) whose
// bones the ring code moves (rope physics, apron sway, mat bounce). WWE '13
// keeps one corner / one side and lets its code place copies. So the WWE '13
// meshes go onto the SvR2011 models: turned to each corner where SvR2011 has
// a model per corner, their bone slots pointed at the SvR2011 bone that has
// the same name, else the nearest one.

// node position in the model (nodes hang off the root, no rotations)
std::array<float, 3> NodePos(const Model& m, int i) {
  std::array<float, 3> p{0, 0, 0};
  for (int guard = 0; i >= 0 && i < int(m.nodes.size()) && guard < 64; ++guard) {
    for (int k = 0; k < 3; ++k) p[k] += m.nodes[i].t[k];
    const int up = m.nodes[i].parent;
    if (up == i || up < 0) break;
    i = up;
  }
  return p;
}

// turn by d radians in the floor plane (game x, z)
void TurnXZ(float* v, float d) {
  const float c = std::cos(d), s = std::sin(d), x = v[0], z = v[2];
  v[0] = x * c - z * s;
  v[2] = x * s + z * c;
}

void Bounds(Mesh& s) {
  if (s.verts.empty()) return;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto& v : s.verts)
    for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.pos[k]), hi[k] = std::max(hi[k], v.pos[k]);
  float c[3], r = 0;
  for (int k = 0; k < 3; ++k) c[k] = (lo[k] + hi[k]) / 2;
  for (const auto& v : s.verts) {
    float d = 0;
    for (int k = 0; k < 3; ++k) d += (v.pos[k] - c[k]) * (v.pos[k] - c[k]);
    r = std::max(r, d);
  }
  s.sphere = {c[0], c[1], c[2], std::sqrt(r)};
}

enum class BoneMap { kByName, kNearest, kFixed };

// host node for each WWE '13 node
std::vector<int> MapNodes(const Model& host, const Model& w, float turn, BoneMap mode, int fixed) {
  std::vector<int> map(w.nodes.size(), fixed);
  for (size_t j = 0; j < w.nodes.size(); ++j) {
    if (mode == BoneMap::kFixed) continue;
    int best = -1;
    if (mode == BoneMap::kByName)
      for (size_t k = 0; k < host.nodes.size(); ++k)
        if (host.nodes[k].name == w.nodes[j].name) best = int(k);
    if (best < 0) {
      auto p = NodePos(w, int(j));
      TurnXZ(p.data(), turn);
      float bd = 1e30f;
      for (size_t k = 0; k < host.nodes.size(); ++k) {
        const auto q = NodePos(host, int(k));
        float d = 0;
        for (int c = 0; c < 3; ++c) d += (p[c] - q[c]) * (p[c] - q[c]);
        // (the mesh's own and the root node sit at the origin: never pick them by position)
        const bool helper = host.nodes[k].name == host.name || host.nodes[k].name == "root";
        if (!helper && d < bd) bd = d, best = int(k);
      }
    }
    map[j] = best >= 0 ? best : fixed;
  }
  return map;
}

// w's meshes, turned by each of `turns`, onto host's skeleton
void PutMeshes(Model& host, const Model& w, const std::vector<float>& turns, BoneMap mode, int fixed) {
  host.textures = w.textures;
  // the ring code and the shadow pass read the descriptor's format / draw
  // words: the SvR2011 part's own values (WWE '13's converted ones make the
  // apron and pads drop their shadows flat across the canvas)
  const bool have = !host.meshes.empty();
  const Bytes host_raw = have ? host.meshes[0].raw : Bytes();
  const uint32_t host_vfmt = have ? host.meshes[0].vfmt : 0;
  host.meshes.clear();
  for (float t : turns) {
    const std::vector<int> map = MapNodes(host, w, t, mode, fixed);
    for (Mesh s : w.meshes) {
      if (have) {
        s.raw = host_raw;
        s.vfmt = s.shader == "yDefault" ? (host_vfmt & 3) : host_vfmt;  // (yDefault: formats 0-3 only)
      }
      for (auto& v : s.verts) {
        TurnXZ(v.pos, t);
        TurnXZ(v.normal, t);
      }
      for (auto& p : s.palette) {
        const int j = p - 1;
        p = (j >= 0 && j < int(map.size()) ? map[j] : fixed) + 1;
      }
      Bounds(s);
      host.meshes.push_back(std::move(s));
    }
  }
}

// The mat keeps SvR2011's mesh (its bounce bones); WWE '13's canvas mapping
// and material go on it, by vertex position.
bool PutMatLook(Model& host, const Model& w, float tone) {
  if (host.meshes.empty() || w.meshes.empty()) return false;
  Mesh& hm = host.meshes[0];
  const Mesh& wm = w.meshes[0];
  // (the rolled edge has seam vertices: two at one place with different UVs;
  // among equally near ones the one with the nearest UV, or the picture's
  // seam lands on the wrong side and the edge trim smears)
  for (size_t i = 0; i < hm.verts.size(); ++i) {
    float bd = 1e30f;
    size_t bk = 0;
    for (size_t k = 0; k < wm.verts.size(); ++k) {
      float d = 0;
      for (int c = 0; c < 3; ++c) d += (hm.verts[i].pos[c] - wm.verts[k].pos[c]) * (hm.verts[i].pos[c] - wm.verts[k].pos[c]);
      if (i < hm.uvs.size() && k < wm.uvs.size())
        d += 1e-3f * ((hm.uvs[i][0] - wm.uvs[k][0]) * (hm.uvs[i][0] - wm.uvs[k][0]) + (hm.uvs[i][1] - wm.uvs[k][1]) * (hm.uvs[i][1] - wm.uvs[k][1]));
      if (d < bd) bd = d, bk = k;
    }
    if (bk < wm.uvs.size() && i < hm.uvs.size()) hm.uvs[i] = wm.uvs[bk];
    hm.verts[i].color = wm.verts[bk].color;
  }
  // the canvas keeps SvR2011's own parameter list (its shader set-up for the
  // mat); only the colours come from WWE '13, its picture is slot 0
  for (auto& p : hm.params) {
    const Param* src = FindParam(wm, p.name.c_str());
    if (p.type == 0x0d && src && src->type == 0x0d) p.value = src->value;
    if (p.type == 0x0f && p.value.size() >= 4)
      PutBe32(p.value.data(), p.name == "texDiffuse" ? 0u : 0xFFFFFFFFu);
  }
  // SvR2011's ring lights are stronger than WWE '13's light-mapped ones: the
  // canvas colours are toned down to keep the grey canvas from going white
  for (auto& p : hm.params)
    if ((p.name == "g_f4MatAmbCol" || p.name == "g_f4MatDifCol") && p.type == 0x0d && p.value.size() >= 12)
      for (int k = 0; k < 3; ++k) PutBeF(&p.value[4 * k], BeF(&p.value[4 * k]) * tone);
  const int ws = GetSlot(wm, {"texDiffuse"});
  host.textures = {ws >= 0 && ws < int(w.textures.size()) ? w.textures[ws] : std::string("rin_mat00")};
  return true;
}

// The nested crowd pack: models converted (bodies are already in SvR2011
// units), seat files scaled.
Bytes ConvertCrowd(const Bytes& raw, Wwe13Report& rep) {
  std::vector<PachEntry> ents;
  if (!PachRead(raw, ents)) return raw;
  for (auto& e : ents) {
    const bool bpe = IsBpe(e.data);
    Bytes r = Unpack(e.data);
    if (IsJboy13(r)) {
      Model m;
      std::string err;
      if (!Jboy13Read(r, m, &err)) { rep.notes.push_back("crowd model " + std::to_string(e.id) + ": " + err); continue; }
      ConvertModel13(m, 1.f, rep.conv);
      r = JboyWrite(m);
    } else if (r.size() > 4 && r[0] == ';') {
      const std::string t = Text(r);
      if (t.find('(') != std::string::npos || IsCrowdArea13(t))
        r = FromText(ConvertCrowdText13(t, 0.1f));
    } else {
      continue;
    }
    e.data = bpe ? Pack(r) : r;
  }
  return PachWrite(ents);
}

// ---- barrier corners
// WWE '13 models each barrier corner (ar_fence01_c/f/g/h) once, near the
// origin, as a diagonal run of panels between two end posts on bone01 and
// bone03; its code moves those bones onto the gap between the side run's end
// and the front / back run's end, stretching the piece. SvR2011 does not place
// them (nothing in its vis tree either): the stretch is baked here, by
// position along the piece, and the meshes go into the side run's model (drawn
// and culled with it). WWE '13's collision (HMD) already has the corners.
int NodeByName(const Model& m, const char* name) {
  for (size_t k = 0; k < m.nodes.size(); ++k)
    if (m.nodes[k].name == name) return int(k);
  return -1;
}

int BakeBarrierCorners(std::map<uint32_t, Model>& models, Wwe13Report& rep) {
  struct P { float x, z; uint32_t id; };
  std::vector<P> runs;
  std::vector<uint32_t> corners;
  for (const auto& [id, m] : models) {
    if (Lower(m.name).rfind("ar_fence01", 0) != 0) continue;
    if (NodeByName(m, "bone01") >= 0 && NodeByName(m, "bone03") >= 0) { corners.push_back(id); continue; }
    for (const auto& s : m.meshes)
      for (const auto& v : s.verts)
        if (v.pos[1] < -2.f) runs.push_back({v.pos[0], v.pos[2], id});  // (the panels, not the feet)
  }
  int done = 0;
  for (uint32_t cid : corners) {
    Model& c = models[cid];
    const auto e1a = NodePos(c, NodeByName(c, "bone01")), e3a = NodePos(c, NodeByName(c, "bone03"));
    float e1[2] = {e1a[0], e1a[2]}, e3[2] = {e3a[0], e3a[2]};
    if (std::fabs(e3[0]) > std::fabs(e1[0])) std::swap(e1, e3);  // e1: the end toward the side run
    const float sx = e1[0] + e3[0] > 0 ? 1.f : -1.f, sz = e1[1] + e3[1] > 0 ? 1.f : -1.f;
    std::vector<P> q;
    for (const auto& p : runs)
      if (p.x * sx > 0 && p.z * sz > 0) q.push_back(p);
    if (q.empty()) continue;
    float mx = 0;
    for (const auto& p : q) mx = std::max(mx, std::fabs(p.x));
    // the side run's end post (A), then the nearest other run end (B)
    const P* a = nullptr;
    for (const auto& p : q)
      if (std::fabs(p.x) > mx - 3 && (!a || p.z * sz > a->z * sz)) a = &p;
    if (!a) continue;
    auto centre = [&](float x, float z, bool side, float* out) {
      double sx_ = 0, sz_ = 0;
      int n = 0;
      for (const auto& p : q)
        if ((std::fabs(p.x) > mx - 3) == side && (p.x - x) * (p.x - x) + (p.z - z) * (p.z - z) < 1.f) sx_ += p.x, sz_ += p.z, ++n;
      out[0] = n ? float(sx_ / n) : x;
      out[1] = n ? float(sz_ / n) : z;
    };
    float A[2], B[2];
    centre(a->x, a->z, true, A);
    // (B lies beyond A's end along z: SD99's side run is two panels deep, x
    // 70-73, and its inner posts at A's own z made a 3-unit "corner" squashed
    // to 0.18 - gaps at all four corners)
    const P* b = nullptr;
    float bd = 1e30f;
    for (const auto& p : q) {
      if (std::fabs(p.x) > mx - 3 || p.z * sz < A[1] * sz + 1.f) continue;
      const float d = (p.x - A[0]) * (p.x - A[0]) + (p.z - A[1]) * (p.z - A[1]);
      if (d < bd) bd = d, b = &p;
    }
    if (!b) continue;
    centre(b->x, b->z, false, B);
    const float L = std::hypot(e3[0] - e1[0], e3[1] - e1[1]), L2 = std::hypot(B[0] - A[0], B[1] - A[1]);
    if (L < 1.f || L2 < 1.f || L2 > 3 * L) continue;
    const float u[2] = {(e3[0] - e1[0]) / L, (e3[1] - e1[1]) / L}, n[2] = {-u[1], u[0]};
    const float u2[2] = {(B[0] - A[0]) / L2, (B[1] - A[1]) / L2}, n2[2] = {-u2[1], u2[0]};
    Model& host = models[a->id];
    for (Mesh s : c.meshes) {
      for (auto& v : s.verts) {
        const float dx = v.pos[0] - e1[0], dz = v.pos[2] - e1[1];
        const float t = (dx * u[0] + dz * u[1]) * (L2 / L), w = dx * n[0] + dz * n[1];
        v.pos[0] = A[0] + u2[0] * t + n2[0] * w;
        v.pos[2] = A[1] + u2[1] * t + n2[1] * w;
        const float nt = v.normal[0] * u[0] + v.normal[2] * u[1], nw = v.normal[0] * n[0] + v.normal[2] * n[1];
        v.normal[0] = u2[0] * nt + n2[0] * nw;
        v.normal[2] = u2[1] * nt + n2[1] * nw;
      }
      for (auto& p : s.palette) p = 1;  // every bone slot: the run's own node
      for (auto& p : s.params)
        if (IsTexParam(p)) {
          const int slot = int32_t(Be32(p.value.data()));
          if (slot < 0 || slot >= int(c.textures.size())) continue;
          int at = -1;
          for (size_t k = 0; k < host.textures.size(); ++k)
            if (Lower(host.textures[k]) == Lower(c.textures[slot])) at = int(k);
          if (at < 0) at = int(host.textures.size()), host.textures.push_back(c.textures[slot]);
          PutBe32(p.value.data(), uint32_t(at));
        }
      Bounds(s);
      host.meshes.push_back(std::move(s));
    }
    // the run's node sphere grows to hold the corner
    if (!host.nodes.empty()) {
      float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
      for (const auto& s : host.meshes)
        for (const auto& v : s.verts)
          for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.pos[k]), hi[k] = std::max(hi[k], v.pos[k]);
      float r = 0;
      for (int k = 0; k < 3; ++k) r += (hi[k] - lo[k]) * (hi[k] - lo[k]);
      for (int k = 0; k < 3; ++k) host.nodes[0].sphere[k] = (lo[k] + hi[k]) / 2;
      host.nodes[0].sphere[3] = std::sqrt(r) / 2;
    }
    for (auto& s : c.meshes)
      for (auto& st : s.strips) st.indices.clear();
    char msg[160];
    std::snprintf(msg, sizeof msg, "barrier corner %s: (%.1f, %.1f)-(%.1f, %.1f), stretched x%.2f, into %s",
                  c.name.c_str(), A[0], A[1], B[0], B[1], L2 / L, host.name.c_str());
    rep.notes.push_back(msg);
    ++done;
  }
  return done;
}

}  // namespace

// WWE '13 hangs a video cube over the ring (bg28: four arena_movie panels
// 11-13 m up, raw_801 / raw_804, and four diagonal ones on the end of the
// titantron's strip in raw_803 / raw_808). SvR2011 arenas have none, and the
// high entrance camera shows it as a screen floating over the ramp (2.0.3
// report). A movie model wholly over the ring goes; one whose strip ends with
// such panels keeps the part before them. True: drop the whole model.
bool DropHangingScreens(Model& m) {
  static const char* kMovies[] = {"arena_movie", "titantron_movie", "header_wall_mov"};
  bool movie = false;
  for (const auto& t : m.textures)
    for (const char* k : kMovies) movie |= Lower(t) == k;
  if (!movie || m.meshes.empty()) return false;
  auto over_ring = [](const Vertex& v) {
    return std::fabs(v.pos[0]) <= 40.f && std::fabs(v.pos[2]) <= 40.f && v.pos[1] < -100.f;
  };
  bool any_left = false;
  for (auto& mesh : m.meshes) {
    for (auto& st : mesh.strips) {
      size_t keep = st.indices.size();
      for (size_t k = 0; k < st.indices.size(); ++k)
        if (st.indices[k] < mesh.verts.size() && over_ring(mesh.verts[st.indices[k]])) { keep = k; break; }
      if (keep == st.indices.size()) continue;
      // the rest must be only hanging panels (or the joining repeats of the last kept index)
      bool rest_hangs = true;
      for (size_t k = keep; k < st.indices.size(); ++k) {
        const uint16_t i = st.indices[k];
        if (i < mesh.verts.size() && !over_ring(mesh.verts[i]) && !(keep && i == st.indices[keep - 1])) rest_hangs = false;
      }
      if (!rest_hangs) continue;
      while (keep && keep >= 2 && st.indices[keep - 1] == st.indices[keep - 2]) --keep;  // (joining repeat)
      st.indices.resize(keep < 3 ? 0 : keep);
    }
    mesh.strips.erase(std::remove_if(mesh.strips.begin(), mesh.strips.end(),
                                     [](const Strip& st) { return st.indices.empty(); }),
                      mesh.strips.end());
    any_left |= !mesh.strips.empty();
  }
  return !any_left;
}

namespace {

// SvR2011 plays videos on few screen models: the entrance video on 803 / 806 / 808, the arena's loop on 801 / 807
// (sub_821828D0: 800-808 are slots, 809 and up none); any other screen gets the live camera, in entrances too. WWE
// 2K15 - 2K17 hang 10 - 11 of them (800 - 809, 820): their extra titantrons showed the camera (the walk filmed into
// the screen behind it). Their screens go to those ids, biggest first, the rest's meshes into the biggest of their
// kind (arena vertices are world positions: models whose nodes all sit at the origin only); an announce table's
// monitor (small, at the ring) keeps its id and the game's feed. The flag lines (0xC351) follow the ids.
void ArrangeScreens(std::map<uint32_t, Model>& pending, std::string& flags, Wwe13Report& rep) {
  auto kind = [](const Model& m) {
    for (const auto& t : m.textures) {
      if (Lower(t) == "titantron_movie") return 1;
      if (Lower(t) == "arena_movie") return 2;
    }
    return 0;
  };
  auto at_origin = [](const Model& m) {
    for (const auto& n : m.nodes)
      for (int k = 0; k < 3; ++k)
        if (std::fabs(n.t[k]) > 1e-4f || std::fabs(n.r[k]) > 1e-4f) return false;
    return true;
  };
  auto radius = [](const Model& m) {
    float r = 0;
    for (const auto& s : m.meshes) r = std::max(r, s.sphere[3]);
    return r;
  };
  auto monitor = [](const Model& m) {
    bool small = !m.meshes.empty();
    for (const auto& s : m.meshes)
      small = small && s.sphere[3] < 60.f && std::fabs(s.sphere[0]) < 60.f && std::fabs(s.sphere[2]) < 60.f;
    return small;
  };
  std::vector<uint32_t> screens[3];
  for (const auto& [id, m] : pending)
    if (id >= 800 && id <= 899 && kind(m) && !monitor(m) && !m.meshes.empty()) screens[kind(m)].push_back(id);
  const std::vector<uint32_t> slots[3] = {{}, {803, 806, 808}, {801, 807}};
  std::map<uint32_t, uint32_t> moved;  // old id -> new id (0: merged into another)
  std::map<uint32_t, Model> placed;
  std::string note;
  for (int k = 1; k <= 2; ++k) {
    auto& ids = screens[k];
    if (ids.empty()) continue;
    std::stable_sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) { return radius(pending[a]) > radius(pending[b]); });
    // (a slot id held by something that isn't one of these screens stays as it is)
    std::vector<uint32_t> free_slots;
    for (uint32_t s : slots[k])
      if (!pending.count(s) || std::find(ids.begin(), ids.end(), s) != ids.end()) free_slots.push_back(s);
    if (free_slots.empty()) continue;
    for (size_t i = 0; i < ids.size(); ++i) {
      Model& src = pending[ids[i]];
      if (i < free_slots.size()) {
        moved[ids[i]] = free_slots[i];
        placed[free_slots[i]] = src;
        continue;
      }
      Model& dst = placed[free_slots[0]];
      // (both in world positions first: each mesh through its node - rotation Rz Ry Rx, then the offset, up the
      // parents - and the nodes then at the origin)
      auto to_world = [](Model& m) {
        auto world = [&m](int node, float v[3], bool direction) {
          for (int n = node, steps = 0; n >= 0 && n < int(m.nodes.size()) && steps < 64; n = m.nodes[size_t(n)].parent, ++steps) {
            const Node& nd = m.nodes[size_t(n)];
            float x = v[0], y = v[1], z = v[2];
            const float cx = std::cos(nd.r[0]), sx = std::sin(nd.r[0]), cy = std::cos(nd.r[1]), sy = std::sin(nd.r[1]),
                        cz = std::cos(nd.r[2]), sz = std::sin(nd.r[2]);
            const float y1 = cx * y - sx * z, z1 = sx * y + cx * z;  // Rx
            y = y1, z = z1;
            const float x2 = cy * x + sy * z, z2 = -sy * x + cy * z;  // Ry
            x = x2, z = z2;
            const float x3 = cz * x - sz * y, y3 = sz * x + cz * y;  // Rz
            x = x3, y = y3;
            v[0] = x + (direction ? 0.f : nd.t[0]), v[1] = y + (direction ? 0.f : nd.t[1]), v[2] = z + (direction ? 0.f : nd.t[2]);
            if (nd.parent == n) break;
          }
        };
        for (auto& mesh : m.meshes) {
          const int node = mesh.palette.empty() ? 0 : mesh.palette[0];
          for (auto& v : mesh.verts) world(node, v.pos, false), world(node, v.normal, true);
          world(node, mesh.sphere.data(), false);
        }
        std::vector<std::array<float, 3>> centres(m.nodes.size());
        for (size_t n = 0; n < m.nodes.size(); ++n) {
          float c[3] = {m.nodes[n].sphere[0], m.nodes[n].sphere[1], m.nodes[n].sphere[2]};
          world(int(n), c, false);
          centres[n] = {c[0], c[1], c[2]};
        }
        for (size_t n = 0; n < m.nodes.size(); ++n)
          for (int k = 0; k < 3; ++k) m.nodes[n].t[k] = 0.f, m.nodes[n].r[k] = 0.f, m.nodes[n].sphere[k] = centres[n][size_t(k)];
      };
      if (!at_origin(dst)) to_world(dst), note += " " + std::to_string(free_slots[0]) + " (to world positions)";
      if (!at_origin(src)) to_world(src), note += " " + std::to_string(ids[i]) + " (to world positions)";
      for (auto& mesh : src.meshes)
        for (auto& p : mesh.palette) p = 0;  // (the screen it joins: its first node, at the origin now)
      // its textures onto the biggest one's list, its meshes' texture slots renumbered
      std::vector<int> map(src.textures.size());
      for (size_t t = 0; t < src.textures.size(); ++t) {
        auto f = std::find(dst.textures.begin(), dst.textures.end(), src.textures[t]);
        if (f == dst.textures.end()) dst.textures.push_back(src.textures[t]), f = dst.textures.end() - 1;
        map[t] = int(f - dst.textures.begin());
      }
      for (Mesh mesh : src.meshes) {
        for (auto& p : mesh.params)
          if (IsTexParam(p)) {
            const int32_t v = int32_t(Be32(p.value.data()));
            if (v >= 0 && v < int(map.size())) PutBe32(p.value.data(), uint32_t(map[size_t(v)]));
          }
        dst.meshes.push_back(std::move(mesh));
      }
      moved[ids[i]] = 0;
      note += " " + std::to_string(ids[i]) + "->" + std::to_string(free_slots[0]) + " (merged)";
    }
  }
  if (moved.empty()) return;
  for (const auto& [from, to] : moved) {
    pending.erase(from);
    if (to && from != to) note += " " + std::to_string(from) + "->" + std::to_string(to);
  }
  for (auto& [id, m] : placed) pending[id] = std::move(m);
  // the flag lines: a moved model's under its new id, a merged one's gone (the one it went into keeps its own)
  std::map<uint32_t, std::string> line_of;
  std::string out;
  for (size_t at = 0; at < flags.size();) {
    size_t end = flags.find('\n', at);
    end = end == std::string::npos ? flags.size() : end + 1;
    std::string line = flags.substr(at, end - at);
    at = end;
    size_t p = line.find_first_not_of(" \t");
    if (p != std::string::npos && std::isdigit(static_cast<unsigned char>(line[p]))) {
      const uint32_t id = uint32_t(std::strtoul(line.c_str() + p, nullptr, 10));
      if (moved.count(id)) {
        line_of[id] = line;  // (written back below under its new id)
        continue;
      }
      if (pending.count(id) && id >= 800 && id <= 899 && std::find_if(moved.begin(), moved.end(), [&](const auto& mv) { return mv.second == id; }) != moved.end())
        continue;  // (a slot id something else moved onto: its old line goes)
    }
    out += line;
  }
  for (const auto& [from, to] : moved) {
    if (!to || !line_of.count(from)) continue;
    std::string line = line_of[from];
    const size_t p = line.find_first_not_of(" \t");
    size_t e = p;
    while (e < line.size() && std::isdigit(static_cast<unsigned char>(line[e]))) ++e;
    line.replace(p, e - p, std::to_string(to));
    if (!out.empty() && out.back() != '\n') out += "\r\n";
    out += line;
  }
  flags = out;
  rep.notes.push_back("screens to SvR2011's video slots (entrance 803 / 806 / 808, loop 801 / 807):" + note);
}

}  // namespace

int LiftTableLids(Arena& a) {
  auto lum = [](const uint8_t* p) { return (p[0] + p[1] + p[2]) / 3.0f; };
  const BundleTexture* desk = nullptr;
  for (const auto& b : a.bundles)
    for (const auto& t : b.textures)
      if (Lower(t.name) == "ji_desk") desk = &t;
  int n = 0;
  for (auto& b : a.bundles) {
    if (b.id != 0x192) continue;
    for (auto& t : b.textures) {
      if (Lower(t.name) != "ji_top") continue;
      Image img;
      DdsInfo info;
      if (!DdsInfoOf(t.data, info) || !DdsDecode(t.data, img) || img.rgba.empty()) continue;
      // the cloth: the median brightness (the logo is a small part)
      std::vector<float> l;
      for (size_t k = 0; k < img.rgba.size(); k += 16) l.push_back(lum(&img.rgba[k]));
      std::nth_element(l.begin(), l.begin() + l.size() / 2, l.end());
      const float cloth = std::max(l[l.size() / 2], 1.0f);
      if (cloth >= 60) continue;  // (SvR2011's own lids: a picture)
      float tint[3] = {96, 96, 96};
      Image d;
      if (desk && DdsDecode(desk->data, d) && !d.rgba.empty()) {
        double s[3] = {};
        for (size_t k = 0; k < d.rgba.size(); k += 4)
          for (int c = 0; c < 3; ++c) s[c] += d.rgba[k + size_t(c)];
        const size_t px = d.rgba.size() / 4;
        const float m[3] = {float(s[0] / px), float(s[1] / px), float(s[2] / px)};
        if ((m[0] + m[1] + m[2]) / 3 >= cloth * 1.5f) tint[0] = m[0], tint[1] = m[1], tint[2] = m[2];
      }
      for (size_t k = 0; k < img.rgba.size(); k += 4) {
        uint8_t* p = &img.rgba[k];
        const int sat = std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]});
        if (sat > 40 || std::fabs(lum(p) - cloth) > 60) continue;  // (the logo)
        const float g = std::min(lum(p) / cloth, 2.0f);
        for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::min(255.0f, tint[c] * g));
      }
      t.data = DdsEncode(img, info.format == DxtFormat::kUnknown ? DxtFormat::kDxt1 : info.format, info.mips > 1);
      b.changed = true;
      ++n;
    }
  }
  return n;
}

bool ConvertProp13(const Bytes& in, Bytes& out, float scale, std::string* error) {
  std::vector<PachEntry> ents;
  if (!PachRead(in, ents)) {
    if (error) *error = "not a PACH";
    return false;
  }
  for (auto& e : ents) {
    const Bytes r = Unpack(e.data);
    if (IsJboy13(r)) {
      Model m;
      if (!Jboy13Read(r, m, error)) return false;
      Convert13Stats st;
      ConvertModel13(m, scale, st);
      e.data = Pack(JboyWrite(m));
    } else {
      e.data = Pack(r);  // (2K14 and later keep their parts in ZLIB: the game's are BPE)
    }
  }
  out = PachWrite(ents);
  return true;
}

bool BuildArenaFromWwe13(const Bytes& wwe13, const Bytes& host_file, const Wwe13Options& opt, Bytes& out,
                         Wwe13Report& rep, std::string* error) {
  auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
  std::vector<PachEntry> se;
  if (!HspcStage13(wwe13, se)) {  // (WWE 2K17's container; else EPAC / EPK8)
    Epac src;
    if (!ArchiveRead(wwe13, src, error)) return false;
    for (const auto& g : src.groups)
      for (const auto& e : g.entries)
        if (IsPach(e.data) && se.empty()) PachRead(e.data, se);
  }
  if (se.empty()) return fail("WWE '13 file: no stage index (PACH)");
  Arena host;
  if (!host.LoadData(host_file, error)) return false;
  // WWE 2K14 (its entries packed as "ZLIB")
  bool k14 = false;
  for (const auto& e : se) k14 = k14 || (e.data.size() >= 4 && !std::memcmp(e.data.data(), "ZLIB", 4));

  std::map<uint32_t, Bytes> outm;  // id -> stored entry
  std::map<uint32_t, Model> pending;  // converted arena models (written after the barrier corners)
  std::map<uint32_t, std::string> names;  // converted models
  std::map<uint32_t, const PachEntry*> hostm;
  for (const auto& e : host.entries) hostm[e.id] = &e;
  Bytes bank13;  // the arena's light bank (0x7530)
  std::string flags13;  // its converted model flags (0xC351)
  bool has_flags13 = false;
  // the source's light maps (by name), for BakeLightMaps13
  std::map<std::string, LightMap13> lightmaps;
  {
    std::set<std::string> wanted;
    std::map<std::string, std::array<float, 4>> bounds;  // name -> x0 x1 z0 z1 of the meshes using it
    for (const auto& e : se) {
      if (e.id >= 0x190) continue;
      const Bytes raw = Unpack(e.data);
      Model lm_model;
      if (!IsJboy(raw) || !Jboy13Read(raw, lm_model)) continue;
      for (const auto& msh : lm_model.meshes) {
        const int slot = GetSlot(msh, {"sampLightMapPrm"});
        if (slot < 0 || slot >= int(lm_model.textures.size())) continue;
        const std::string n = Lower(lm_model.textures[slot]);
        wanted.insert(n);
        auto b = bounds.emplace(n, std::array<float, 4>{1e9f, -1e9f, 1e9f, -1e9f}).first;
        for (const auto& v : msh.verts) {
          b->second[0] = std::min(b->second[0], v.pos[0]), b->second[1] = std::max(b->second[1], v.pos[0]);
          b->second[2] = std::min(b->second[2], v.pos[2]), b->second[3] = std::max(b->second[3], v.pos[2]);
        }
      }
    }
    for (const auto& e : se)
      if (e.id >= 0x190 && e.id <= 0x192) {
        std::vector<BundleTexture> texs;
        if (!BundleRead(Unpack(e.data), texs)) continue;
        for (const auto& t : texs)
          if (wanted.count(Lower(t.name)) && !lightmaps.count(Lower(t.name))) {
            LightMap13 L;
            if (!DdsDecode(t.data, L.img)) continue;
            const std::string n = Lower(t.name);
            L.top_down = n.rfind("fade_lightmap", 0) == 0;
            if (bounds.count(n)) {
              const auto& b = bounds[n];
              L.x0 = b[0], L.x1 = b[1], L.z0 = b[2], L.z1 = b[3];
            }
            lightmaps[n] = std::move(L);
          }
      }
  }

  // WWE '13's ring materials (colours by diffuse texture name), for the host's ring parts
  std::map<std::string, std::array<float, 12>> ring_colours;
  std::map<uint32_t, Model> ring13;  // WWE '13's ring models, converted
  for (const auto& e : se) {
    if (opt.host_ring && InRing(e.id)) {
      ++rep.dropped;
      Model m;
      if (Jboy13Read(Unpack(e.data), m)) {
        Model c = m;
        Convert13Stats dummy;
        ConvertModel13(c, 0.1f, dummy);
        ring13[e.id] = std::move(c);
      }
      if (Jboy13Read(Unpack(e.data), m))
        for (const auto& s : m.meshes) {
          const int slot = GetSlot(s, {"sampDiffusePrm", "texDiffuse"});
          if (slot < 0 || slot >= int(m.textures.size()) || ring_colours.count(Lower(m.textures[slot]))) continue;
          std::array<float, 12> c;
          c.fill(1.f);
          Get4(s, "g_f4MatAmbCol", &c[0]);
          Get4(s, "g_f4MatDifCol", &c[4]);
          Get4(s, "g_f4SpecularCol", &c[8]);
          ring_colours[Lower(m.textures[slot])] = c;
        }
      continue;
    }
    if (e.id == 0x7532 || e.id == 0x7533) { ++rep.dropped; continue; }  // (WWE '13 only)
    if (e.id == 0x7530) { bank13 = Unpack(e.data); continue; }  // (written after the flags: LightBank13)
    // (2K14 stores a few small texts - ring zones 0xEB29 - as bare zlib streams
    // of unknown size: the host's, which match)
    if (k14 && e.data.size() > 2 && e.data[0] == 0x78 && hostm.count(e.id)) {
      outm[e.id] = hostm[e.id]->data;
      ++rep.from_host;
      continue;
    }
    Bytes raw = Unpack(e.data);
    // 2K17 nests 'SHDC' blocks where 2K14 has PACHs (its crowd packs 0x4E20 / 0x4E2A: left
    // as they were, the crowd was missing): the same entries, written as a PACH
    if (raw.size() >= 4 && !std::memcmp(raw.data(), "SHDC", 4)) {
      std::vector<PachEntry> kids;
      if (ShdcRead13(raw, 0, kids)) raw = PachWrite(kids);
    }
    if (IsJboy(raw)) {
      Model m;
      std::string err;
      if (!Jboy13Read(raw, m, &err)) return fail("model " + std::to_string(e.id) + ": " + err);
      BakeLightMaps13(m, lightmaps, rep.conv.lightmaps_baked);
      ConvertModel13(m, 0.1f, rep.conv);
      if (DropHangingScreens(m)) { ++rep.dropped; continue; }
      names[e.id] = m.name;
      // vertex format bit 0 makes a mesh cast the ring lights' shadows onto the
      // mat. WWE '13 sets it on stands, ringside chairs and the announce table,
      // whose projected shadows lay rows of grey blocks across the canvas in
      // SvR2011 (seat rows as dashes, the table as a slab): no arena model casts
      // (the ring parts are SvR2011's slots and keep theirs)
      for (auto& s : m.meshes) s.vfmt &= opt.no_shadows ? 0u : ~1u;
      if (opt.draw_word >= 0)  // test: descriptor +0x88 (4 / 6 in both games) forced
        for (auto& s : m.meshes)
          if (s.raw.size() >= 0x8C) PutBe32(&s.raw[0x88], uint32_t(opt.draw_word));
      // test aid: models in the shift ranges moved / turned 90 degrees
      for (const auto& sh : opt.shift)
        if (e.id >= sh.lo && e.id <= sh.hi) {
          auto mv = [&](float* p) {
            if (sh.turn) { const float x = p[0]; p[0] = -p[2]; p[2] = x; }
            p[0] += sh.d[0], p[1] += sh.d[1], p[2] += sh.d[2];
          };
          for (auto& s : m.meshes) {
            for (auto& v : s.verts) {
              mv(v.pos);
              if (sh.turn) { const float x = v.normal[0]; v.normal[0] = -v.normal[2]; v.normal[2] = x; }
            }
            mv(s.sphere.data());
          }
          for (auto& n : m.nodes) mv(n.sphere);
        }
      // test aid: bounding spheres of the ranges shrunk to 1 unit (verts unchanged)
      for (const auto& r : opt.tiny)
        if (e.id >= r.first && e.id <= r.second) {
          for (auto& s : m.meshes) s.sphere[3] = 1.f;
          for (auto& n : m.nodes) n.sphere[3] = 1.f;
        }
      // test aid: only mesh k of model id drawn
      for (const auto& km : opt.keep_mesh)
        if (e.id == km.first)
          for (size_t k = 0; k < m.meshes.size(); ++k)
            if (int(k) != km.second)
              for (auto& st : m.meshes[k].strips) st.indices.clear();
      // meshes left out (from the last, so the others keep their numbers)
      {
        std::vector<int> drop;
        for (const auto& dm : opt.drop_mesh)
          if (e.id == dm.first && dm.second >= 0 && dm.second < int(m.meshes.size())) drop.push_back(dm.second);
        std::sort(drop.rbegin(), drop.rend());
        drop.erase(std::unique(drop.begin(), drop.end()), drop.end());
        for (int k : drop) m.meshes.erase(m.meshes.begin() + k);
      }
      // test aid: models in the hide ranges drawn with nothing
      for (const auto& r : opt.hide)
        if (e.id >= r.first && e.id <= r.second)
          for (auto& s : m.meshes)
            for (auto& st : s.strips) st.indices.clear();
      pending[e.id] = std::move(m);
    } else if (e.id == 0x3E4 || e.id == 0x3E7) {
      outm[e.id] = Pack(ConvertHmd13(raw, 0.1f));
    } else if (e.id == 0xC35A) {
      // mip map XML: WWE '13 appends a <MipmapBiasData> section SvR2011's
      // reader does not know (the load never ends): keep <MipmapData> only
      std::string t = Text(raw);
      const size_t end = t.find("</MipmapData>");
      if (end != std::string::npos) {
        size_t cut = end + 13;
        while (cut < t.size() && (t[cut] == '\r' || t[cut] == '\n')) ++cut;
        t.resize(cut);
      }
      outm[e.id] = Pack(FromText(t));
    } else if (e.id == 0xC364) {
      // (SvR2011 keeps these boxes in metres, a tenth of its model units;
      // WWE '13 in cm, its model units)
      std::string vt = ConvertVisTree13(Text(raw), 0.01f);
      if (opt.no_shmap)  // WWE '13's shadow-map groups (SHMAP_*): renamed plain groups
        for (size_t p = vt.find("SHMAP_"); p != std::string::npos; p = vt.find("SHMAP_", p)) vt.replace(p, 6, "GRPSH_");
      outm[e.id] = Pack(FromText(vt));
    } else if (e.id == 0xC351) {
      std::string fl = ConvertFlags13(Text(raw));
      if (opt.role_lights) fl = RelightByRole13(fl, names, opt.stand_light);
      if (opt.no_spot_shadows) fl = NoSpotShadows13(fl, names);
      flags13 = fl, has_flags13 = true;
    } else if (e.id == 0xC353) {
      outm[e.id] = Pack(FromText(ScaleZones13(Text(raw), 0.1f)));
    } else if ((e.id == 0x4E20 || e.id == 0x4E2A) && IsPach(raw)) {
      if (opt.host_crowd && hostm.count(e.id)) { outm[e.id] = hostm[e.id]->data; ++rep.from_host; continue; }
      outm[e.id] = Pack(ConvertCrowd(raw, rep));
    } else {
      // as stored; 2K14's "ZLIB" packing repacked (the game unpacks BPE only:
      // its object motions 0x7531 and 0x7922 left as ZLIB crashed the arena load)
      outm[e.id] = (e.data.size() >= 4 && !std::memcmp(e.data.data(), "ZLIB", 4)) ? Pack(raw) : e.data;
    }
  }
  if (opt.barrier_corners) BakeBarrierCorners(pending, rep);
  ArrangeScreens(pending, flags13, rep);
  // LightBank13: the arena's own light sets, plus those the game looks up
  // itself and the flags name that it lacks, from the 2011 host arena's bank
  if (!bank13.empty()) {
    std::set<int> want(std::begin(kExeLights13), std::end(kExeLights13));
    if (has_flags13)
      for (int id : FlagLights13(flags13)) want.insert(id);
    if (outm.count(0xC353))
      for (int id : ZoneLights13(Text(Unpack(outm[0xC353])))) want.insert(id);
    std::vector<int> added, unused;
    // (match lights far darker than SvR2011's - WWE '13's set 10: ground 0.14, RAW 0.78 - or a default set 100
    // without directional light: the host's, as SvR 2008 / 2009 ports take RAW's set 10; DarkLightSets)
    const std::set<int> dark = DarkLightSets(bank13);
    if (hostm.count(0x7530)) bank13 = FillLights13(bank13, Unpack(hostm[0x7530]->data), want, added, unused, dark);
    if (!dark.empty()) {
      std::string n = "match lighting too dark for SvR 2011 - the 2011 host arena's sets used:";
      for (int id : dark) n += " " + std::to_string(id);
      rep.notes.push_back(n);
    }
    if (!added.empty()) {
      std::string n = "light sets from the 2011 host arena:";
      for (int id : added) n += " " + std::to_string(id);
      rep.notes.push_back(n);
    }
    if (!unused.empty()) {
      std::string n = "light sets nothing uses, left out:";
      for (int id : unused) n += " " + std::to_string(id);
      rep.notes.push_back(n);
    }
    outm[0x7530] = Pack(bank13);
  }
  if (has_flags13) outm[0xC351] = Pack(FromText(BankLights13(flags13, LightIds13(outm.count(0x7530) ? Unpack(outm[0x7530]) : Bytes()))));
  // A model over ~822 KB crashes 2011's arena load (WWE '13 bg35 / 45 / 47: 826-829 KB, KOTR '98's
  // cables 826 KB; 797-822 KB play): over kMaxModel its biggest meshes are left out until it fits
  constexpr size_t kMaxModel = 800000;
  for (auto& [id, m] : pending) {
    Bytes j = JboyWrite(m);
    int dropped = 0;
    while (j.size() > kMaxModel && m.meshes.size() > 1) {
      size_t big = 0;
      for (size_t k = 1; k < m.meshes.size(); ++k)
        if (m.meshes[k].verts.size() > m.meshes[big].verts.size()) big = k;
      m.meshes.erase(m.meshes.begin() + big);
      ++dropped;
      j = JboyWrite(m);
    }
    if (dropped)
      rep.notes.push_back("model " + m.name + " (" + std::to_string(id) + ") over the game's model size: " +
                          std::to_string(dropped) + " biggest mesh(es) left out");
    outm[id] = Pack(j);
  }
  // 0xC35A (50010): the "MipmapData" table (XML: texture name -> mip level the game skips to). Always the host's: WWE
  // 2K16 / 2K17's (241 entries, NumOfData 241; every stock 2011 arena says 200) crashed the game just before the match
  // - a render target resolve into a null texture (2K17 bg09, user report 15 Oct; bisected in game: the host's
  // 0xC35A alone makes it play)
  outm.erase(0xC35A);
  // the host's: the ring kit, and data files WWE '13 has none of (not its
  // models; not its ad boards or titantron flip-book, placed for its own hall)
  for (const auto& e : host.entries)
    if ((opt.host_ring && InRing(e.id)) ||
        (!outm.count(e.id) && !IsJboy(Unpack(e.data)) && e.id != 0x80E8 && (e.id < 0xEA6B || e.id > 0xEA6D))) {
      outm[e.id] = e.data;
      ++rep.from_host;
      if (!InRing(e.id)) {
        char b[64];
        std::snprintf(b, sizeof b, "entry %04x from the host", e.id);
        rep.notes.push_back(b);
      }
    }

  std::vector<PachEntry> ents;
  for (auto& [id, data] : outm) ents.push_back({id, std::move(data)});
  rep.entries = int(ents.size());
  Epac e = host.epac;
  e.groups[host.group].entries[host.entry].data = PachWrite(ents);
  Bytes first = EpacWrite(e);

  Arena a;
  if (!a.LoadData(first, error)) return false;
  a.original_file = host.original_file;
  a.original_unpacked = host.original_unpacked;
  // the ring: WWE '13's meshes, textures and materials on SvR2011's ring models
  // (their ids, bones and sizes, which the ring code and rope physics use)
  std::set<uint32_t> transplanted;
  if (opt.host_ring && opt.ring_meshes) {
    constexpr float kQuarter = 1.5707963f;
    auto w = [&](uint32_t id) -> const Model* { auto it = ring13.find(id); return it == ring13.end() ? nullptr : &it->second; };
    for (auto& am : a.models) {
      if (!InRing(am.id)) continue;
      Model& h = am.model;
      const Model* src = nullptr;
      bool ok = false;
      if (am.id == 0x3B7 && (src = w(0x3B7))) {  // posts: SvR2011 has the four in one model
        PutMeshes(h, *src, {0, kQuarter, 2 * kQuarter, 3 * kQuarter}, BoneMap::kFixed, 0);
        ok = true;
      } else if (am.id == 0x3B8 && (src = w(0x3B8))) {  // turnbuckle (the ring code draws it at each rope end)
        PutMeshes(h, *src, {0}, BoneMap::kByName, 0);
        ok = true;
      } else if (am.id == 0x3B9 && (src = w(0x3B9))) {  // mat
        ok = PutMatLook(h, *src, opt.mat_tone);
      } else if (((am.id >= 0x3BC && am.id <= 0x3BF && (src = w(0x3BC))) ||  // rope shadows
                  (am.id >= 0x3C0 && am.id <= 0x3C3 && (src = w(opt.rope_hi ? 0x3C0 : 0x3D4)))) &&  // ropes
                 h.nodes.size() > 1) {
        // one model per side, each with its own bones along its own side:
        // WWE '13's rope (along x, from its bone49 at x = -28.5) turned onto
        // the side so that its first bone lies on SvR2011's first, then each
        // bone slot to the SvR2011 bone at that place
        int wfirst = -1;
        for (size_t k = 0; k < src->nodes.size(); ++k)
          if (src->nodes[k].name == "bone49") wfirst = int(k);
        if (wfirst >= 0) {
          const auto hp = NodePos(h, 1), wp = NodePos(*src, wfirst);
          const float turn = std::atan2(hp[2], hp[0]) - std::atan2(wp[2], wp[0]);
          PutMeshes(h, *src, {turn}, BoneMap::kNearest, 0);
          ok = true;
        }
      } else if (am.id >= 0x3C4 && am.id <= 0x3C7 && (src = w(0x3C4)) && h.nodes.size() > 1 && src->nodes.size() > 2) {
        // corner pads: SvR2011 has one model per corner, WWE '13 one corner
        int hb = -1, wb = -1;
        for (size_t k = 0; k < h.nodes.size(); ++k)
          if (h.nodes[k].name.rfind("bone", 0) == 0) hb = int(k);
        for (size_t k = 0; k < src->nodes.size(); ++k)
          if (src->nodes[k].name.rfind("bone", 0) == 0) wb = int(k);
        if (hb >= 0 && wb >= 0) {
          const auto hp = NodePos(h, hb), wp = NodePos(*src, wb);
          const float turn = std::atan2(hp[2], hp[0]) - std::atan2(wp[2], wp[0]);
          PutMeshes(h, *src, {turn}, BoneMap::kFixed, hb);
          ok = true;
        }
      } else if ((am.id == 0x3CD || am.id == 0x3CE) && opt.host_aprons) {
        // aprons: SvR2011's own mesh and sway bones (WWE '13's, on SvR2011's
        // bones by position, poke through the ring skirt as they sway). Both
        // games map the apron picture the same way (u along the side, v down),
        // so WWE '13's rin_ep00 / rin_ep01 fit it by name; colours below
        ok = false;
      } else if ((am.id == 0x3CD || am.id == 0x3CE) && (src = w(am.id))) {  // aprons: their sway bones by position
        PutMeshes(h, *src, {0}, BoneMap::kNearest, 0);
        ok = true;
      }
      if (ok && (am.id == 0x3B7 || am.id == 0x3B8)) {
        // the posts' black wrap: WWE '13 gives it a wide specular (power 10),
        // which SvR2011's ring lights turn into a grey sheen
        for (auto& s : h.meshes)
          for (auto& p : s.params)
            if (p.name == "g_fSpecularLev" && p.value.size() >= 4) PutBeF(p.value.data(), std::min(BeF(p.value.data()), 0.05f));
      }
      if (ok) {
        am.changed = true;
        transplanted.insert(am.id);
        ++rep.ring_parts;
      } else if ((am.id == 0x3CD || am.id == 0x3CE) && opt.host_aprons) {
        rep.notes.push_back(std::string("ring model ") + (am.id == 0x3CD ? "03cd" : "03ce") + ": SvR2011's apron, WWE '13's picture");
      } else {
        char b[96];
        std::snprintf(b, sizeof b, "ring model %04x (%s): kept SvR2011's mesh", am.id, h.name.c_str());
        rep.notes.push_back(b);
      }
    }
  }
  // ring parts left as the host's wear WWE '13's colours for the same pictures
  for (auto& am : a.models) {
    if (!InRing(am.id) || transplanted.count(am.id)) continue;
    for (auto& s : am.model.meshes) {
      const int slot = GetSlot(s, {"texDiffuse"});
      if (slot < 0 || slot >= int(am.model.textures.size())) continue;
      auto it = ring_colours.find(Lower(am.model.textures[slot]));
      if (it == ring_colours.end()) continue;
      for (auto& p : s.params) {
        const int at = p.name == "g_f4MatAmbCol" ? 0 : p.name == "g_f4MatDifCol" ? 4 : p.name == "g_f4SpecularCol" ? 8 : -1;
        if (at < 0 || p.type != 0x0d || p.value.size() < 16) continue;
        for (int k = 0; k < 4; ++k) PutBeF(&p.value[4 * k], it->second[at + k]);
        am.changed = true;
      }
    }
  }
  // textures (names match without case): the ring kit's pictures WWE '13
  // lacks come from the host; names nothing has stay missing, as in the
  // shipped arenas (the movie screens are named, not stored); unused ones go
  std::set<std::string> used;
  for (const auto& m : a.models) CollectTextures(m.model, used);
  auto find = [](Arena& ar, const std::string& name) -> BundleTexture* {
    for (auto& b : ar.bundles)
      for (auto& t : b.textures)
        if (Lower(t.name) == Lower(name)) return &t;
    return nullptr;
  };
  for (const auto& name : used)
    if (!find(a, name)) {
      BundleTexture* h = find(host, name);
      // a picture the arena names but doesn't carry - WWE 2K17's WCW Nitro: its light cards' pas_light / pas_light02
      // (2K14's arenas carry them) - draws white: a clear one instead (the videos' names stay unstored, as shipped;
      // "specular" is a map slot's placeholder)
      if (!h && Lower(name) != "arena_movie" && Lower(name) != "titantron_movie" && Lower(name) != "specular" && !a.bundles.empty()) {
        Image clear;
        clear.w = clear.h = 4;
        clear.rgba.assign(4 * 4 * 4, 0);
        a.bundles[0].textures.push_back({name, "dds", DdsEncode(clear, DxtFormat::kDxt5, false)});
        a.bundles[0].changed = true;
        rep.notes.push_back("texture " + name + ": not in the arena - a clear one (it drew white)");
        continue;
      }
      if (!h) {
        rep.notes.push_back("texture " + name + ": none (as shipped)");
        continue;
      }
      if (a.bundles.empty()) return fail("no texture set");
      a.bundles[0].textures.push_back({name, "dds", h->data});
      a.bundles[0].changed = true;
      ++rep.placeholders;
    }
  // SvR2011's apron normal map carries its own apron's lettering and folds;
  // under WWE '13's picture it would emboss the wrong words: flat (each pixel
  // the map's average, i.e. its "straight up" in its own encoding)
  if (opt.host_ring && opt.host_aprons)
    if (BundleTexture* t = find(a, "rin_ep00_n")) {
      Image img;
      DdsInfo info;
      if (DdsInfoOf(t->data, info) && DdsDecode(t->data, img) && !img.rgba.empty()) {
        double sum[4] = {0, 0, 0, 0};
        const size_t px = img.rgba.size() / 4;
        for (size_t k = 0; k < img.rgba.size(); ++k) sum[k & 3] += img.rgba[k];
        for (size_t k = 0; k < img.rgba.size(); ++k) img.rgba[k] = uint8_t(sum[k & 3] / double(px) + 0.5);
        t->data = DdsEncode(Resize(img, std::min(img.w, 64), std::min(img.h, 64)),
                            info.format == DxtFormat::kUnknown ? DxtFormat::kDxt1 : info.format, info.mips > 1);
        for (auto& b : a.bundles) b.changed = true;
      }
    }
  std::set<std::string> used_lc;
  for (const auto& n : used) used_lc.insert(Lower(n));
  for (auto& b : a.bundles) {
    for (const auto& hb : host.bundles)
      if (hb.id == b.id) b.original_raw = hb.original_raw;
    if (!opt.prune_textures || b.id < 0x190 || b.id > 0x192) continue;  // (blood etc.: named by code)
    const size_t before = b.textures.size();
    b.textures.erase(std::remove_if(b.textures.begin(), b.textures.end(),
                                    [&](const BundleTexture& t) { return !used_lc.count(Lower(t.name)); }),
                     b.textures.end());
    if (b.textures.empty()) {  // a set must hold something
      Image img;
      img.w = img.h = 4;
      img.rgba.assign(64, 0);
      b.textures.push_back({"dummy", "dds", DdsEncode(img, DxtFormat::kDxt1, false)});
    }
    if (b.textures.size() != before) b.changed = true;
  }
  // the ring's shadow mask (rin_mask00, 128 x 16): SvR2011's ring code tiles it
  // over the canvas, and WWE '13's (a vertical alpha ramp) lays rows of grey
  // dashes across the mat: SvR2011's is kept
  for (const auto& hb : host.bundles)
    for (const auto& ht : hb.textures)
      if (Lower(ht.name) == "rin_mask00")
        if (BundleTexture* t = find(a, ht.name)) {
          t->data = ht.data;
          for (auto& b : a.bundles) b.changed = true;
        }
  // the crowd's light (aud_lightmap: the game samples it by name; without it
  // the crowd is drawn black): the host's when the arena has none (WWE '12 and
  // WWE '13 arenas never do; an extra texture of that name still wins below)
  if (!find(a, "aud_lightmap") && !a.bundles.empty())
    for (const auto& hb : host.bundles)
      for (const auto& ht : hb.textures)
        if (Lower(ht.name) == "aud_lightmap" && !find(a, ht.name)) {
          a.bundles[0].textures.push_back(ht);
          a.bundles[0].changed = true;
        }
  // extra pictures (e.g. SvR2011 code objects' textures by name) and the arena card
  for (const auto& t : opt.extra_textures) {
    if (BundleTexture* have = find(a, t.name)) have->data = t.data;
    else if (!a.bundles.empty()) a.bundles[0].textures.push_back(t);
    if (!a.bundles.empty()) a.bundles[0].changed = true;
  }
  if (!opt.card.empty())
    for (auto& en : a.entries)
      if (en.id == 0xEE48) en.data = Pack(opt.card);
  // Opaque arena models' pictures: WWE '12 / '13 / 2K14 keep a gloss mask in
  // the diffuse alpha (their materials have no separate specular map) and
  // SvR2011 blends by it (as the posts below), so 2K14's ringside mats
  // (alpha 0.2-0.55) and black barricade came out see-through over the black
  // under the floor. In SvR2011 the model flags (0xC351) make a model
  // see-through: s / a / k (blended), c (cut out), i / j / o (drawn after).
  // Pictures only models without those flags use get alpha 255, in the DXT
  // blocks (DXT3: explicit alpha, DXT5: both end points 255, indices 0).
  {
    std::map<uint32_t, bool> see;  // model id -> see-through flag
    for (const auto& en : a.entries)
      if (en.id == 0xC351) {
        const std::string t = Text(Unpack(en.data));
        size_t i = 0;
        while (i < t.size()) {
          size_t e = t.find('\n', i);
          if (e == std::string::npos) e = t.size();
          const std::string line = t.substr(i, e - i);
          i = e + 1;
          char* end = nullptr;
          const long id = std::strtol(line.c_str(), &end, 10);
          if (end == line.c_str() || line.compare(0, 2, "//") == 0) continue;
          bool s = false;
          for (size_t k = size_t(end - line.c_str()); k < line.size(); ++k) {
            const char c = line[k];
            const bool word = (k == 0 || line[k - 1] == ' ' || line[k - 1] == '\t') &&
                              (k + 1 >= line.size() || line[k + 1] == ' ' || line[k + 1] == '\t' || line[k + 1] == '\r');
            if (word && std::strchr("sakcijo", c)) s = true;
          }
          see[uint32_t(id)] = s;
        }
      }
    std::set<std::string> opaque_tex, see_tex;
    for (const auto& am : a.models) {
      if (am.id >= 0x190) continue;  // (arena models; the ring is the host's)
      auto it = see.find(am.id);
      const bool s = it != see.end() && it->second;
      for (const auto& msh : am.model.meshes) {
        const int slot = GetSlot(msh, {"texDiffuse"});
        if (slot >= 0 && slot < int(am.model.textures.size())) (s ? see_tex : opaque_tex).insert(Lower(am.model.textures[slot]));
      }
    }
    int made = 0;
    for (auto& b : a.bundles)
      for (auto& t : b.textures) {
        const std::string n = Lower(t.name);
        DdsInfo info;
        if (!opaque_tex.count(n) || see_tex.count(n) || !DdsInfoOf(t.data, info)) continue;
        if (info.format != DxtFormat::kDxt3 && info.format != DxtFormat::kDxt5) continue;
        bool changed = false;
        for (size_t k = 128; k + 16 <= t.data.size(); k += 16) {
          uint8_t* blk = &t.data[k];
          if (info.format == DxtFormat::kDxt3) {
            for (int j = 0; j < 8; ++j) changed |= blk[j] != 0xFF, blk[j] = 0xFF;
          } else {
            const uint8_t want[8] = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
            changed |= std::memcmp(blk, want, 8) != 0;
            std::memcpy(blk, want, 8);
          }
        }
        if (changed) ++made, b.changed = true;
      }
    if (made) rep.notes.push_back("pictures of opaque models made opaque (gloss masks in their alpha): " + std::to_string(made));
  }
  // the posts' picture: WWE '13 keeps a specular mask in its alpha (0.75-0.84);
  // SvR2011 blends the post with it, so the black post shows grey: opaque
  if (opt.host_ring && opt.ring_meshes)
    for (auto& am : a.models)
      if (am.id == 0x3B7)
        for (const auto& tn : am.model.textures)
          if (BundleTexture* t = find(a, tn)) {
            Image img;
            DdsInfo info;
            if (DdsInfoOf(t->data, info) && DdsDecode(t->data, img)) {
              for (size_t k = 3; k < img.rgba.size(); k += 4) img.rgba[k] = 255;
              t->data = DdsEncode(img, DxtFormat::kDxt1, info.mips > 1);
              for (auto& b : a.bundles) b.changed = true;
            }
          }
  // a set over its room (WWE '13's announce table set 0x192 is twice SvR2011's)
  // first hands its biggest pictures to the main set, which has room, rather
  // than halving them
  ArenaTextureSet* main_set = nullptr;
  for (auto& b : a.bundles)
    if (b.id == 0x190) main_set = &b;
  auto set_size = [](const ArenaTextureSet& b) {
    size_t t = 16 + 32 * b.textures.size();
    for (const auto& x : b.textures) t += (x.data.size() + 15) & ~size_t(15);
    return t;
  };
  // The announce tables (SvR2011 code objects, bgEtc models 1060 / 1070) read
  // ji_top / ji_mon / ji_bl from set 0x192 only: they stay there. Model 1070
  // has the same vertices and UVs in both games, so WWE '13's pictures fit
  // it as they are; they are made to fit the set's room instead of moving:
  // each takes the size and format of SvR2011's picture of the same name
  // (ji_top DXT1, ji_bl 64 x 64 DXT3). ji_bl's alpha is a gloss mask in
  // SvR2011 (WWE '13's is opaque, which would make the table's black body
  // shine): SvR2011's mask is kept.
  auto table_pic = [](const std::string& n) {
    const std::string l = Lower(n);
    return l == "ji_top" || l == "ji_mon" || l == "ji_bl";
  };
  for (auto& b : a.bundles) {
    if (b.id != 0x192) continue;
    for (auto& t : b.textures) {
      if (!table_pic(t.name)) continue;
      const BundleTexture* ht = nullptr;
      for (const auto& hb : host.bundles)
        if (hb.id == 0x192)
          for (const auto& x : hb.textures)
            if (Lower(x.name) == Lower(t.name)) ht = &x;
      Image img, himg;
      DdsInfo info, hinfo;
      if (!ht || !DdsInfoOf(t.data, info) || !DdsInfoOf(ht->data, hinfo) || !DdsDecode(t.data, img) || !DdsDecode(ht->data, himg)) continue;
      if (info.w == hinfo.w && info.h == hinfo.h && info.format == hinfo.format) continue;
      if (img.w != hinfo.w || img.h != hinfo.h) img = Resize(img, hinfo.w, hinfo.h);
      bool opaque = true;
      for (size_t k = 3; k < img.rgba.size(); k += 4) opaque = opaque && img.rgba[k] == 255;
      if (hinfo.format == DxtFormat::kDxt1) {
        for (size_t k = 3; k < img.rgba.size(); k += 4) img.rgba[k] = 255;
      } else if (opaque && himg.rgba.size() == img.rgba.size()) {
        for (size_t k = 3; k < img.rgba.size(); k += 4) img.rgba[k] = himg.rgba[k];
      }
      t.data = DdsEncode(img, hinfo.format == DxtFormat::kUnknown ? DxtFormat::kDxt1 : hinfo.format, hinfo.mips > 1);
      b.changed = true;
    }
  }
  if (LiftTableLids(a)) rep.notes.push_back("announce table lid (ji_top) tinted with the desk's colour: WWE '13's is the black cloth its desk covers");
  for (auto& b : a.bundles) {
    if (!main_set || &b == main_set || !b.original_raw) continue;
    while (set_size(b) > b.original_raw && b.textures.size() > 1) {
      auto big = b.textures.end();
      for (auto it = b.textures.begin(); it != b.textures.end(); ++it)
        if (!(b.id == 0x192 && table_pic(it->name)) && (big == b.textures.end() || it->data.size() > big->data.size())) big = it;
      if (big == b.textures.end()) break;
      if (set_size(*main_set) + big->data.size() + 48 > main_set->original_raw) break;
      rep.notes.push_back("texture " + big->name + " moved to the main set (room)");
      main_set->textures.push_back(*big);
      b.textures.erase(big);
      b.changed = main_set->changed = true;
    }
  }
  // (the canvas is the picture most on screen: something else gives way)
  a.fit_margin = opt.fit;
  a.file_max = kArenaFileMax;  // (bigger than the host's file: 2.1.6 - the Arena page's save says needs=)
  rep.halved = a.FitFile({"rin_mat00"});
  out = a.Save(error);
  if (error && !error->empty()) return false;
  if (out.size() > host.original_file)
    rep.notes.push_back("bigger than the host file (" + std::to_string(out.size()) + " bytes): it needs a newer game (the manifest says needs=)");
  if (GeometryOf(a).OverStock())
    rep.notes.push_back("more 3D detail than the game's own arenas: it needs a newer game (the manifest says needs=)");
  // (more geometry than any 2011 arena: the game crashes loading it - WWE 2K16 bg06 / bg12 / bg15)
  if (const ArenaGeometry g = GeometryOf(a); g.Over()) {
    if (error)
      *error = "this arena has more 3D detail than SvR 2011 can load (" + std::to_string(g.verts) + " points, " +
               std::to_string(g.indices) + " corners and " + std::to_string(g.meshes) + " meshes; it takes up to " +
               std::to_string(kMaxArenaVertsNew) + ", " + std::to_string(kMaxArenaIndicesNew) + " and " +
               std::to_string(kMaxArenaMeshesNew) + "): it would crash the game in the match";
    return false;
  }
  return true;
}

}  // namespace svrfmt
