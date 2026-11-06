// svrmod: command-line front end of the Mod Maker's format library.
//   svrmod roundtrip <file.pac>...       EPAC/PACH/BPE/textures/JBOY self-test
//   svrmod export <bgNN.pac> <out dir>   arena -> arena.fbx + textures/*.png
//   svrmod xdec <wwe13 file> <out>       WWE '13 0x0FF512ED file -> plain (EPK8 / EPAC ...)
//   svrmod wwe13 <WWE13 bgNN.pac> <host SvR2011 bgNN.pac> <out.pac> [host-crowd] [prune-textures] [wwe13-ring]
//         [host-ring-meshes] [rope-hi] [no-corners] [wwe13-aprons] [wwe13-lights] [tex=<name.dds>]... [card=<256x128 dds>]
#include <algorithm>
#include <sstream>
#include <map>
#include <cmath>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "svrfmt/archive.h"
#include "svrfmt/arena_full.h"
#include "svrfmt/arena.h"
#include "svrfmt/arena_import.h"
#include "svrfmt/jboy.h"
#include "svrfmt/model_objects.h"
#include "svrfmt/png.h"
#include "svrfmt/ring_kit.h"
#include "svrfmt/pac.h"
#include "svrfmt/texture.h"
#include "svrfmt/wwe13.h"
#include "svrfmt/zip_write.h"

using namespace svrfmt;

namespace {

struct Counts {
  int files = 0, files_same = 0, jboy = 0, jboy_same = 0, bundles = 0, bundles_same = 0, dds = 0, dds_ok = 0;
};

void WalkPach(const Bytes& blob, Counts& c) {
  std::vector<PachEntry> ents;
  if (!PachRead(blob, ents)) return;
  for (const auto& e : ents) {
    const Bytes raw = Unpack(e.data);
    if (IsPach(raw)) { WalkPach(raw, c); continue; }
    if (IsJboy(raw)) {
      ++c.jboy;
      Model m, m2;
      std::string err;
      if (!JboyRead(raw, m, &err)) { std::printf("  JBOY %x: %s\n", e.id, err.c_str()); continue; }
      const Bytes w = JboyWrite(m);
      // compare by content: re-read and re-write must be stable, and
      // geometry must match the original read
      if (JboyRead(w, m2) && JboyWrite(m2) == w && m2.meshes.size() == m.meshes.size()) {
        bool same = true;
        for (size_t i = 0; i < m.meshes.size() && same; ++i) {
          const auto &a = m.meshes[i], &b = m2.meshes[i];
          same = a.verts.size() == b.verts.size() && a.strips.size() == b.strips.size() &&
                 !std::memcmp(a.verts.data(), b.verts.data(), a.verts.size() * sizeof(Vertex));
          for (size_t k = 0; same && k < a.strips.size(); ++k) same = a.strips[k].indices == b.strips[k].indices;
        }
        if (same) ++c.jboy_same;
        else std::printf("  JBOY %x: content differs\n", e.id);
      } else {
        std::printf("  JBOY %x: rewrite unstable\n", e.id);
      }
    } else if (IsTextureBundle(raw)) {
      ++c.bundles;
      std::vector<BundleTexture> texs;
      if (BundleRead(raw, texs)) {
        Bytes w = BundleWrite(texs);
        Bytes w2 = w;
        // originals sometimes stop without the final 16-byte pad
        if (w == raw || (w.size() > raw.size() && std::equal(raw.begin(), raw.end(), w.begin()))) ++c.bundles_same;
        for (const auto& t : texs) {
          ++c.dds;
          Image img;
          if (DdsDecode(t.data, img)) ++c.dds_ok;
        }
      }
    }
  }
}

int Roundtrip(int argc, char** argv) {
  Counts c;
  for (int i = 0; i < argc; ++i) {
    Bytes d;
    if (!ReadFile(argv[i], d)) { std::printf("%s: cannot read\n", argv[i]); continue; }
    Epac e;
    if (!EpacRead(d, e)) { std::printf("%s: not EPAC\n", argv[i]); continue; }
    ++c.files;
    if (EpacWrite(e) == d) ++c.files_same;
    else std::printf("%s: EPAC repack differs\n", argv[i]);
    for (const auto& g : e.groups)
      for (const auto& en : g.entries)
        if (IsPach(en.data)) WalkPach(en.data, c);
  }
  std::printf("EPAC %d/%d identical; JBOY %d/%d stable; bundles %d/%d identical; DDS decoded %d/%d\n",
              c.files_same, c.files, c.jboy_same, c.jboy, c.bundles_same, c.bundles, c.dds_ok, c.dds);
  return c.files_same == c.files && c.jboy_same == c.jboy && c.bundles_same == c.bundles && c.dds_ok == c.dds ? 0 : 1;
}

}  // namespace

int ExportFull(const char* pac, const char* out) {
  Arena a;
  std::string err;
  if (!a.Load(pac, &err)) { std::printf("%s: %s\n", pac, err.c_str()); return 1; }
  ArenaFullReport rep;
  const bool ok = ExportArenaFull(a, out, pac, rep);
  std::printf("%s: %d objects (%d placed by motion, %d mirrored copies, %d ring parts, %d models not drawn left out), %d meshes, %d triangles, %d textures (%d failed), %d normal map links, %d lights (%d sets in lights.json)\n",
              ok ? "exported" : "FAILED", rep.objects, rep.placed, rep.mirrored, rep.ring_parts, rep.not_drawn, rep.meshes, rep.triangles, rep.textures, rep.textures_failed,
              rep.normal_maps, rep.lights, rep.light_sets);
  for (const auto& w : rep.warnings) std::printf("  %s\n", w.c_str());
  return ok ? 0 : 1;
}

int Export(const char* pac, const char* out) {
  Arena a;
  std::string err;
  if (!a.Load(pac, &err)) { std::printf("%s: %s\n", pac, err.c_str()); return 1; }
  ExportReport rep;
  const bool ok = ExportArena(a, out, pac, rep);
  std::printf("%s: %d models, %d meshes, %d triangles, %d textures (%d failed)\n", ok ? "exported" : "FAILED",
              rep.models, rep.meshes, rep.triangles, rep.textures, rep.textures_failed);
  for (const auto& w : rep.warnings) std::printf("  %s\n", w.c_str());
  return ok ? 0 : 1;
}

// Re-compress every BPE entry with our encoder: must decode back exactly;
// reports sizes against Yuke's. With an output path, writes the re-packed file.
int BpeTest(const char* pac, const char* out) {
  Bytes d;
  Epac e;
  if (!ReadFile(pac, d) || !EpacRead(d, e)) { std::printf("%s: cannot read\n", pac); return 1; }
  size_t theirs = 0, ours = 0, bad = 0;
  for (auto& g : e.groups)
    for (auto& en : g.entries) {
      std::vector<PachEntry> ents;
      if (!PachRead(en.data, ents)) continue;
      for (auto& pe : ents) {
        if (!IsBpe(pe.data)) continue;
        const Bytes raw = Unpack(pe.data);
        const Bytes enc = BpeEncode(raw);
        Bytes back;
        if (!BpeDecode(enc, back) || back != raw) ++bad;
        theirs += pe.data.size();
        ours += enc.size();
        pe.data = enc;
      }
      en.data = PachWrite(ents);
    }
  std::printf("%s: Yuke's %zu, ours %zu (%.1f%%), bad %zu\n", pac, theirs, ours, 100.0 * ours / theirs, bad);
  if (out) WriteFile(out, EpacWrite(e));
  return bad ? 1 : 0;
}

// Compress a raw file to a BPE entry (used by the Python lab tools).
// lights <arena.pac> <host bg01.pac> [out.pac]: the match / default / entrance light sets' levels and the
// sets DarkLightSets would take from the host; with out.pac, the arena written with them (as the Mod Maker's
// "Repair it" does).
int Lights(const char* pac, const char* host_pac, const char* out) {
  auto bank_of = [](const Epac& e, std::vector<PachEntry>& ents) -> Bytes {
    if (e.groups.empty() || e.groups[0].entries.empty() || !PachRead(e.groups[0].entries[0].data, ents)) return {};
    for (const auto& x : ents)
      if (x.id == 0x7530) return Unpack(x.data);
    return {};
  };
  Bytes b, hb;
  Epac e, h;
  std::vector<PachEntry> ents, hents;
  if (!ReadFile(pac, b) || !EpacRead(b, e) || !ReadFile(host_pac, hb) || !EpacRead(hb, h)) {
    std::printf("can't read the pacs\n");
    return 1;
  }
  const Bytes bank = bank_of(e, ents), host = bank_of(h, hents);
  if (bank.empty() || host.empty()) {
    std::printf("no light bank (0x7530)\n");
    return 1;
  }
  for (int id : {10, 100, 200}) {
    const LightLevel lv = LightLevelOf(bank, id);
    std::printf("set %d: %s ground %.2f sky %.2f directional %s\n", id, lv.found ? "" : "(none)", lv.ground, lv.sky,
                lv.directional ? "yes" : "no");
  }
  const std::set<int> dark = DarkLightSets(bank);
  std::printf("dark sets:");
  for (int id : dark) std::printf(" %d", id);
  std::printf("%s\n", dark.empty() ? " none" : "");
  if (!out) return 0;
  std::vector<int> replaced;
  for (auto& y : ents)
    if (y.id == 0x7530) y.data = BpeEncode(ReplaceLightSets(bank, host, dark, &replaced));
  e.groups[0].entries[0].data = PachWrite(ents);
  std::printf("replaced:");
  for (int id : replaced) std::printf(" %d", id);
  std::printf("\n");
  return WriteFile(out, EpacWrite(e)) ? 0 : 1;
}

// tablelid <arena.pac> <out.pac>: the announce tables' black lid tinted (svrfmt LiftTableLids, as the WWE '13
// converter does) - for arenas converted before it.
int TableLid(const char* pac, const char* out) {
  Arena a;
  std::string err;
  if (!a.Load(pac, &err)) { std::printf("%s: %s\n", pac, err.c_str()); return 1; }
  const int n = LiftTableLids(a);
  std::printf("lids tinted: %d\n", n);
  if (!n) return 0;
  const Bytes o = a.Save(&err);
  if (o.empty()) { std::printf("save: %s\n", err.c_str()); return 1; }
  return WriteFile(out, o) ? 0 : 1;
}

// move <arena.pac> <model name> <right m> <up m> <forward m> <yaw deg> <pitch deg> <roll deg> <out.pac> [vis]:
// one model moved / turned on all three axes about its bottom centre (the editor's pivot), its meshes' bounding
// spheres refitted; with "vis" its visibility-tree box (0xC364, metres, 3ds Max axes) is refitted and its parents
// grown to hold it - svrfmt GrowVisibility, as the editor's save (test aid for the editor's limits).
int Move(int argc, char** argv) {
  const char* pac = argv[2];
  const std::string name = argv[3];
  const float dx = float(std::atof(argv[4])) * 10, up = float(std::atof(argv[5])) * 10, dz = float(std::atof(argv[6])) * 10;
  const float kDeg = 3.14159265f / 180;
  const float yaw = float(std::atof(argv[7])) * kDeg, pitch = float(std::atof(argv[8])) * kDeg,
              roll = float(std::atof(argv[9])) * kDeg;
  const char* out = argv[10];
  const bool vis = argc >= 12 && !std::strcmp(argv[11], "vis");
  Arena a;
  std::string err;
  if (!a.Load(pac, &err)) { std::printf("%s: %s\n", pac, err.c_str()); return 1; }
  ArenaModel* am = nullptr;
  for (auto& m : a.models)
    if (m.model.name == name) am = &m;
  if (!am) { std::printf("no model %s\n", name.c_str()); return 1; }
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto& me : am->model.meshes)
    for (const auto& v : me.verts)
      for (int j = 0; j < 3; ++j) lo[j] = std::min(lo[j], v.pos[j]), hi[j] = std::max(hi[j], v.pos[j]);
  const float olo[3] = {lo[0], lo[1], lo[2]}, ohi[3] = {hi[0], hi[1], hi[2]};
  const float px = (lo[0] + hi[0]) / 2, py = hi[1], pz = (lo[2] + hi[2]) / 2;  // (y down: hi = bottom)
  // R = yaw (about y) * pitch (about x) * roll (about z), game axes
  const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll),
              sr = std::sin(roll);
  auto rot = [&](float x, float y, float z, float* o) {
    float x1 = x * cr - y * sr, y1 = x * sr + y * cr, z1 = z;     // roll
    float y2 = y1 * cp - z1 * sp, z2 = y1 * sp + z1 * cp, x2 = x1;  // pitch
    o[0] = x2 * cy + z2 * sy, o[1] = y2, o[2] = -x2 * sy + z2 * cy;  // yaw
  };
  for (auto& me : am->model.meshes) {
    for (auto& v : me.verts) {
      float r[3], n[3];
      rot(v.pos[0] - px, v.pos[1] - py, v.pos[2] - pz, r);
      v.pos[0] = px + dx + r[0], v.pos[1] = py - up + r[1], v.pos[2] = pz + dz + r[2];
      rot(v.normal[0], v.normal[1], v.normal[2], n);
      for (int j = 0; j < 3; ++j) v.normal[j] = n[j];
    }
    float l[3] = {1e30f, 1e30f, 1e30f}, h[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& v : me.verts)
      for (int j = 0; j < 3; ++j) l[j] = std::min(l[j], v.pos[j]), h[j] = std::max(h[j], v.pos[j]);
    if (!me.verts.empty()) {
      const float c[3] = {(l[0] + h[0]) / 2, (l[1] + h[1]) / 2, (l[2] + h[2]) / 2};
      float rr = 0;
      for (const auto& v : me.verts)
        rr = std::max(rr, (v.pos[0] - c[0]) * (v.pos[0] - c[0]) + (v.pos[1] - c[1]) * (v.pos[1] - c[1]) +
                              (v.pos[2] - c[2]) * (v.pos[2] - c[2]));
      me.sphere = {c[0], c[1], c[2], std::sqrt(rr)};
    }
  }
  if (std::getenv("SVRMOD_NO_NODE_FIT") == nullptr) std::printf("nodes refitted: %d\n", FitNodeSpheres(am->model));
  am->changed = true;
  for (int j = 0; j < 3; ++j) lo[j] = 1e30f, hi[j] = -1e30f;
  for (const auto& me : am->model.meshes)
    for (const auto& v : me.verts)
      for (int j = 0; j < 3; ++j) lo[j] = std::min(lo[j], v.pos[j]), hi[j] = std::max(hi[j], v.pos[j]);
  std::printf("%s: bounds (game units) %.1f %.1f %.1f .. %.1f %.1f %.1f -> %.1f %.1f %.1f .. %.1f %.1f %.1f\n",
              name.c_str(), olo[0], olo[1], olo[2], ohi[0], ohi[1], ohi[2], lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
  for (const auto& n : am->model.nodes)
    std::printf("node %s t %.1f %.1f %.1f r %.2f %.2f %.2f parent %d sphere %.1f %.1f %.1f r %.1f\n", n.name.c_str(), n.t[0],
                n.t[1], n.t[2], n.r[0], n.r[1], n.r[2], n.parent, n.sphere[0], n.sphere[1], n.sphere[2], n.sphere[3]);
  if (vis) std::printf("vis: %d boxes grown\n", GrowVisibility(a));  // (as the editor's save)
  const auto reduced = a.FitFile({});  // (as the editor's save: the shipped size is the budget)
  if (!reduced.empty()) std::printf("fit: %zu textures halved\n", reduced.size());
  std::string serr;
  const Bytes saved = a.Save(&serr);
  if (saved.empty()) { std::printf("save failed: %s\n", serr.c_str()); return 1; }
  return WriteFile(out, saved) ? 0 : 1;
}

int Bpe(const char* in, const char* out) {
  Bytes raw;
  if (!ReadFile(in, raw)) return 1;
  const Bytes enc = BpeEncode(raw);
  if (enc.size() >= raw.size()) std::printf("warning: packed %zu >= unpacked %zu\n", enc.size(), raw.size());
  return WriteFile(out, enc) ? 0 : 1;
}

int Import(const char* pac, const char* fbx, const char* out) {
  Arena a;
  std::string err;
  if (!a.Load(pac, &err)) { std::printf("%s: %s\n", pac, err.c_str()); return 1; }
  ImportOptions opt;
  ImportReport rep;
  const bool ok = ImportFbx(a, fbx, opt, rep);
  std::printf("%s: %d models changed, %d objects added (%d meshes), %d hidden, textures %d replaced %d added\n",
              ok ? "imported" : "FAILED", rep.models_changed, rep.objects_added, rep.meshes_added, rep.models_hidden,
              rep.textures_replaced, rep.textures_added);
  for (const auto& w : rep.warnings) std::printf("  warning: %s\n", w.c_str());
  for (const auto& e : rep.errors) std::printf("  error: %s\n", e.c_str());
  if (!ok) return 1;
  std::string serr;
  const Bytes data = a.Save(&serr);
  if (!serr.empty()) { std::printf("  error: %s\n", serr.c_str()); return 1; }
  if (!WriteFile(out, data)) return 1;
  std::printf("wrote %s (%zu bytes)\n", out, data.size());
  return 0;
}

// The objects of arena models (svrfmt/model_objects.h):
//   objects <bg.pac> [model]                          every model (id, name, zone, objects) or one model's objects
//   obj-add <in.pac> <out.pac> <model> <file.obj|fbx> [x y z] [scale] [yaw] [cm]
//   obj-remove <in.pac> <out.pac> <model> [object]     (no object: the whole model)
//   obj-export <in.pac> <model> <object[,object..]|all> <out dir>
int SaveArena(Arena& a, const char* out) {
  const int grown = RefreshBounds(a);
  a.file_max = kArenaFileMax;  // (as the arena page's save: bigger than shipped needs 2.1.6)
  for (const auto& t : a.FitFile({})) std::printf("  texture %s halved to fit the arena file\n", t.c_str());
  std::string serr;
  const Bytes data = a.Save(&serr);
  if (!serr.empty()) { std::printf("  error: %s\n", serr.c_str()); return 1; }
  if (!WriteFile(out, data)) return 1;
  const ArenaGeometry g = GeometryOf(a);
  std::printf("wrote %s (%zu bytes; %zu objects, %zu vertices, %zu indices; %d visibility boxes grown)\n", out,
              data.size(), MeshCount(a), g.verts, g.indices, grown);
  return 0;
}

int Objects(int argc, char** argv) {
  Arena a;
  std::string err;
  if (!a.Load(argv[2], &err)) { std::printf("%s: %s\n", argv[2], err.c_str()); return 1; }
  if (argc >= 4) {
    const int m = FindModel(a, argv[3]);
    if (m < 0) { std::printf("no model %s\n", argv[3]); return 1; }
    const auto& am = a.models[size_t(m)];
    std::printf("model %X %s (%s): %zu objects, %zu textures, %zu nodes\n", am.id, am.model.name.c_str(),
                ZoneName(am.zone), am.model.meshes.size(), am.model.textures.size(), am.model.nodes.size());
    for (const auto& o : ObjectsOf(am.model))
      std::printf("  %3d  %-16s %6zu verts %6zu tris%s%s\n", o.mesh, o.texture.c_str(), o.verts, o.triangles,
                  o.skinned ? "  rigged" : "", o.empty ? "  (empty)" : "");
    return 0;
  }
  size_t meshes = 0;
  for (const auto& am : a.models) {
    size_t vis = 0;
    for (const auto& o : ObjectsOf(am.model)) vis += !o.empty;
    meshes += am.model.meshes.size();
    std::printf("%6X %-16s %-9s %3zu objects (%zu visible)\n", am.id, am.model.name.c_str(), ZoneName(am.zone),
                am.model.meshes.size(), vis);
  }
  const ArenaGeometry g = GeometryOf(a);
  std::printf("%zu models, %zu meshes, %zu visible objects, %zu vertices, %zu indices\n", a.models.size(), meshes,
              MeshCount(a), g.verts, g.indices);
  return 0;
}

int ObjAdd(int argc, char** argv) {
  Arena a;
  std::string err;
  if (!a.Load(argv[2], &err)) { std::printf("%s: %s\n", argv[2], err.c_str()); return 1; }
  const int m = FindModel(a, argv[4]);
  if (m < 0) { std::printf("no model %s\n", argv[4]); return 1; }
  AddOptions opt;
  int i = 6;
  if (argc >= i + 3) {
    for (int k = 0; k < 3; ++k) opt.pos[k] = float(std::atof(argv[i + k]));
    i += 3;
  }
  if (argc > i) opt.scale = float(std::atof(argv[i++]));
  if (argc > i) opt.yaw_degrees = float(std::atof(argv[i++]));
  if (argc > i && !std::strcmp(argv[i], "cm")) opt.obj_unit_meters = 0.01f;
  AddReport rep;
  const bool ok = AddObjectFile(a, m, argv[5], opt, rep);
  std::printf("%s: %d objects added to %s (first %d), %d textures added\n", ok ? "added" : "FAILED", rep.meshes_added,
              a.models[size_t(m)].model.name.c_str(), rep.first_mesh, rep.textures_added);
  for (const auto& w : rep.warnings) std::printf("  warning: %s\n", w.c_str());
  for (const auto& e : rep.errors) std::printf("  error: %s\n", e.c_str());
  return ok ? SaveArena(a, argv[3]) : 1;
}

int ObjRemove(int argc, char** argv) {
  Arena a;
  std::string err;
  if (!a.Load(argv[2], &err)) { std::printf("%s: %s\n", argv[2], err.c_str()); return 1; }
  const int m = FindModel(a, argv[4]);
  if (m < 0) { std::printf("no model %s\n", argv[4]); return 1; }
  const int mesh = argc >= 6 ? std::atoi(argv[5]) : -1;
  if (!RemoveObject(a, m, mesh, &err)) { std::printf("FAILED: %s\n", err.c_str()); return 1; }
  std::printf("removed %s of %s\n", mesh < 0 ? "every object" : ("object " + std::to_string(mesh)).c_str(),
              a.models[size_t(m)].model.name.c_str());
  return SaveArena(a, argv[3]);
}

int ObjExport(char** argv) {
  Arena a;
  std::string err, path;
  if (!a.Load(argv[2], &err)) { std::printf("%s: %s\n", argv[2], err.c_str()); return 1; }
  const int m = FindModel(a, argv[3]);
  if (m < 0) { std::printf("no model %s\n", argv[3]); return 1; }
  std::vector<int> meshes;
  if (std::strcmp(argv[4], "all"))  // (a list: 0,2,3)
    for (const char* p = argv[4]; *p;) {
      meshes.push_back(std::atoi(p));
      while (*p && *p != ',') ++p;
      if (*p) ++p;
    }
  if (!ExportObjectObj(a, m, meshes, argv[5], &path, &err)) { std::printf("FAILED: %s\n", err.c_str()); return 1; }
  std::printf("wrote %s\n", path.c_str());
  return 0;
}

// Ring Kit on its own: ring <bgNN.pac> <ring spec file> <out.pac>
// The spec file holds manifest lines (ring.ropes=1 0 1, ring.tints=..., ring.rope<N>_texture=<picture>).
bool LoadRingSpec(const char* path, RingSpec& spec) {
  Bytes b;
  if (!ReadFile(path, b)) { std::printf("%s: cannot read\n", path); return false; }
  const std::string text(b.begin(), b.end());
  spec.FromManifest(text);
  for (int r = 0; r < 3; ++r) {
    const std::string key = "ring.rope" + std::to_string(r) + "_texture=";
    const size_t at = text.find(key);
    if (at == std::string::npos) continue;
    size_t end = text.find_first_of("\r\n", at);
    spec.ropes[r].texture = text.substr(at + key.size(), end == std::string::npos ? end : end - at - key.size());
  }
  return true;
}

bool ApplyRingFile(Arena& a, const char* spec_path) {
  RingSpec spec;
  if (!LoadRingSpec(spec_path, spec)) return false;
  RingReport rep;
  const bool ok = ApplyRing(a, spec, rep);
  std::printf("ring: %d models added, %d changed, %d textures added\n", rep.models_added, rep.models_changed,
              rep.textures_added);
  for (const auto& w : rep.warnings) std::printf("  warning: %s\n", w.c_str());
  const auto reduced = a.FitFile({});
  if (!reduced.empty()) std::printf("  %zu textures halved to fit\n", reduced.size());
  return ok;
}

int Ring(const char* pac, const char* spec_path, const char* out) {
  Arena a;
  std::string err;
  if (!a.Load(pac, &err)) { std::printf("%s: %s\n", pac, err.c_str()); return 1; }
  if (!ApplyRingFile(a, spec_path)) return 1;
  const Bytes data = a.Save(&err);
  if (!err.empty()) { std::printf("error: %s\n", err.c_str()); return 1; }
  if (!WriteFile(out, data)) return 1;
  std::printf("wrote %s (%zu bytes, shipped %zu)\n", out, data.size(), a.original_file);
  return 0;
}

// The Mod Maker's "Save as mod" without its window (tests):
// makemod <bgNN.pac> <edited.fbx | -> <banner picture> <name> <out.svrmod> [ring spec file]
int MakeMod(const char* pac, const char* fbx, const char* banner, const char* name, const char* out,
            const char* ring = nullptr) {
  Arena a;
  std::string err;
  if (!a.Load(pac, &err)) { std::printf("%s: %s\n", pac, err.c_str()); return 1; }
  if (std::strcmp(fbx, "-")) {
    ImportOptions opt;
    ImportReport rep;
    if (!ImportFbx(a, fbx, opt, rep)) {
      for (const auto& e : rep.errors) std::printf("error: %s\n", e.c_str());
      return 1;
    }
  }
  RingSpec spec;
  if (ring) {
    if (!ApplyRingFile(a, ring)) return 1;
    LoadRingSpec(ring, spec);
  }
  const Bytes data = a.Save(&err);
  if (!err.empty()) { std::printf("error: %s\n", err.c_str()); return 1; }
  Image img;
  if (!LoadImageFile(banner, img)) { std::printf("%s: cannot read\n", banner); return 1; }
  std::string id;
  for (const char* c = name; *c; ++c)
    if (std::isalnum(static_cast<unsigned char>(*c))) id.push_back(char(std::tolower(static_cast<unsigned char>(*c))));
    else if (!id.empty() && id.back() != '_') id.push_back('_');
  const std::string manifest =
      std::string("type=arena\nid=") + id + "\nname=" + name + "\nauthor=test\nversion=1.0\n" +
      (ring ? spec.ManifestLines() : std::string());
  std::vector<ZipEntry> files = {{"manifest.txt", Bytes(manifest.begin(), manifest.end())},
                                 {"arena.pac", data},
                                 {"banner.dds", DdsEncode(Resize(img, 256, 128), DxtFormat::kDxt5, false)}};
  if (!WriteFile(out, ZipWrite(files))) return 1;
  std::printf("wrote %s (id %s)\n", out, id.c_str());
  return 0;
}

int Wwe13(int argc, char** argv) {
  Bytes w, h;
  if (!ReadFile(argv[2], w)) { std::printf("%s: cannot read\n", argv[2]); return 1; }
  if (!ReadFile(argv[3], h)) { std::printf("%s: cannot read\n", argv[3]); return 1; }
  Wwe13Options opt;
  for (int i = 5; i < argc; ++i) {
    if (!std::strcmp(argv[i], "host-crowd")) opt.host_crowd = true;
    if (!std::strncmp(argv[i], "fit=", 4)) opt.fit = std::atof(argv[i] + 4);
    if (!std::strcmp(argv[i], "keep-textures")) opt.prune_textures = false;
    if (!std::strcmp(argv[i], "prune-textures")) opt.prune_textures = true;
    if (!std::strcmp(argv[i], "wwe13-ring")) opt.host_ring = false;
    if (!std::strcmp(argv[i], "host-ring-meshes")) opt.ring_meshes = false;
    if (!std::strcmp(argv[i], "rope-hi")) opt.rope_hi = true;
    if (!std::strcmp(argv[i], "no-corners")) opt.barrier_corners = false;  // WWE '13's barrier corners left out
    if (!std::strcmp(argv[i], "wwe13-aprons")) opt.host_aprons = false;  // WWE '13's apron meshes on SvR2011's bones
    if (!std::strcmp(argv[i], "no-shadows")) opt.no_shadows = true;
    if (!std::strcmp(argv[i], "no-shmap")) opt.no_shmap = true;
    if (!std::strncmp(argv[i], "stand-light=", 12)) opt.stand_light = std::atoi(argv[i] + 12);
    if (!std::strncmp(argv[i], "keep=", 5)) {  // keep=<id hex>:<mesh>
      unsigned id = 0;
      int k = 0;
      if (std::sscanf(argv[i] + 5, "%x:%d", &id, &k) == 2) opt.keep_mesh.push_back({id, k});
    }
    if (!std::strncmp(argv[i], "drop-mesh=", 10)) {  // drop-mesh=<id hex>:<mesh>
      unsigned id = 0;
      int k = 0;
      if (std::sscanf(argv[i] + 10, "%x:%d", &id, &k) == 2) opt.drop_mesh.push_back({id, k});
    }
    if (!std::strncmp(argv[i], "tiny=", 5)) {
      unsigned lo = 0, hi = 0;
      if (std::sscanf(argv[i] + 5, "%x-%x", &lo, &hi) == 2) opt.tiny.push_back({lo, hi});
    }
    if (!std::strncmp(argv[i], "shift=", 6) || !std::strncmp(argv[i], "turn=", 5)) {  // shift=lo-hi:dx,dy,dz / turn=lo-hi
      Wwe13Options::Shift sh;
      unsigned lo = 0, hi = 0;
      sh.turn = argv[i][0] == 't';
      const char* p = argv[i] + (sh.turn ? 5 : 6);
      if (std::sscanf(p, "%x-%x", &lo, &hi) == 2) {
        sh.lo = lo, sh.hi = hi;
        if (const char* c = std::strchr(p, ':')) std::sscanf(c + 1, "%f,%f,%f", &sh.d[0], &sh.d[1], &sh.d[2]);
        opt.shift.push_back(sh);
      }
    }
    if (!std::strncmp(argv[i], "draw-word=", 10)) opt.draw_word = std::atoi(argv[i] + 10);
    if (!std::strcmp(argv[i], "spot-shadows")) opt.no_spot_shadows = false;
    if (!std::strncmp(argv[i], "hide=", 5)) {  // hide=<lo hex>-<hi hex>
      unsigned lo = 0, hi = 0;
      if (std::sscanf(argv[i] + 5, "%x-%x", &lo, &hi) == 2) opt.hide.push_back({lo, hi});
    }
    if (!std::strncmp(argv[i], "mat-tone=", 9)) opt.mat_tone = float(std::atof(argv[i] + 9));
    if (!std::strcmp(argv[i], "wwe13-lights")) opt.role_lights = false;
    // tex=<file.dds>: an extra picture, named after the file
    if (!std::strncmp(argv[i], "tex=", 4)) {
      BundleTexture t;
      std::string p = argv[i] + 4, n = p.substr(p.find_last_of("/\\") + 1);
      t.name = n.substr(0, n.rfind('.'));
      t.ext = "dds";
      if (!ReadFile(p, t.data)) { std::printf("%s: cannot read\n", p.c_str()); return 1; }
      opt.extra_textures.push_back(std::move(t));
    }
    if (!std::strncmp(argv[i], "card=", 5) && !ReadFile(argv[i] + 5, opt.card)) {
      std::printf("%s: cannot read\n", argv[i] + 5);
      return 1;
    }
  }
  Wwe13Report rep;
  Bytes out;
  std::string err;
  if (!BuildArenaFromWwe13(w, h, opt, out, rep, &err)) { std::printf("error: %s\n", err.c_str()); return 1; }
  WriteFile(argv[4], out);
  std::printf("models %d, meshes %d (uv scroll %d, light maps dropped %d), entries %d (%d from the host, %d dropped), "
              "placeholders %d, ring parts %d\n",
              rep.conv.models, rep.conv.meshes, rep.conv.uvscroll, rep.conv.lightmaps_dropped, rep.entries,
              rep.from_host, rep.dropped, rep.placeholders, rep.ring_parts);
  for (const auto& n : rep.notes) std::printf("  %s\n", n.c_str());
  if (!rep.halved.empty()) {
    std::printf("  %zu textures halved to fit:", rep.halved.size());
    for (const auto& x : rep.halved) std::printf(" %s", x.c_str());
    std::printf("\n");
  }
  std::printf("wrote %s: %.2f MB (host %.2f MB)\n", argv[4], out.size() / 1048576.0, h.size() / 1048576.0);
  {  // (what the Arena page's save writes in the manifest for it: svrfmt/arena.h ArenaNeedsNew)
    Arena a, host;
    if (a.LoadData(out) && host.LoadData(h)) {
      a.original_file = host.original_file;
      const ArenaGeometry g = GeometryOf(a);
      std::printf("geometry %zu vertices %zu indices; needs: %s\n", g.verts, g.indices,
                  ArenaNeedsNew(a, out.size()) ? "yes (a newer game: needs=)" : "no");
    }
  }
  return 0;
}

// archive-test <pac>...: the pac editor's tree (svrfmt/archive) on each file - open + save unchanged must give
// the same bytes; then every container rebuilt (Touch all) must give the same bytes too, or (xcompressed, written
// back with stored chunks) the same content once decoded. "list <pac>" prints the tree two levels down.
static void ListNode(ArcNode& n, int depth, int max_depth) {
  for (size_t i = 0; i < n.kids.size() && i < 40; ++i) {
    ArcNode& c = n.kids[i];
    const bool cont = depth + 1 < max_depth && ArchiveExpand(c);
    std::printf("%*s%s%s%s  %s  %zu bytes%s\n", depth * 2, "", c.group.empty() ? "" : (c.group + "/").c_str(), c.name.c_str(),
                c.wrap == ArcWrap::Bpe ? " [BPE]" : c.wrap == ArcWrap::Zlib ? " [ZLIB]" : "", ArchiveKindOf(c).c_str(),
                c.stored.size(), cont ? ":" : "");
    if (cont) ListNode(c, depth + 1, max_depth);
  }
  if (n.kids.size() > 40) std::printf("%*s... %zu more\n", depth * 2, "", n.kids.size() - 40);
}

// The first place two trees differ once every container is opened ("" = the same).
static std::string TreeDiff(ArcNode& a, ArcNode& b, const std::string& path, int depth) {
  const bool ca = ArchiveExpand(a), cb = ArchiveExpand(b);
  if (ca != cb || a.kind != b.kind) return path + ": kind";
  if (!ca || depth <= 0) {
    const Bytes x = ArchiveData(a), y = ArchiveData(b);
    // (EPAC / HSPC sizes count 0x100s: an entry read back has zeros up to the next 0x100)
    const Bytes& s = x.size() <= y.size() ? x : y;
    const Bytes& l = x.size() <= y.size() ? y : x;
    if (!std::equal(s.begin(), s.end(), l.begin()) || !std::all_of(l.begin() + ptrdiff_t(s.size()), l.end(), [](uint8_t v) { return v == 0; })) {
      size_t d = 0;
      while (d < x.size() && d < y.size() && x[d] == y[d]) ++d;
      char m[96];
      std::snprintf(m, sizeof m, ": data at 0x%zX (%zu vs %zu bytes)", d, x.size(), y.size());
      return path + m;
    }
    return "";
  }
  if (a.kids.size() != b.kids.size()) return path + ": " + std::to_string(a.kids.size()) + " vs " + std::to_string(b.kids.size()) + " children";
  for (size_t i = 0; i < a.kids.size(); ++i) {
    ArcNode &x = a.kids[i], &y = b.kids[i];
    if (x.name != y.name || x.group != y.group || x.id != y.id || x.hash != y.hash || x.tail != y.tail || x.flags != y.flags)
      return path + "/" + x.name + ": header (" + y.name + ")";
    const std::string d = TreeDiff(x, y, path + "/" + x.group + (x.group.empty() ? "" : ":") + x.name, depth - 1);
    if (!d.empty()) return d;
  }
  return "";
}

// The first node whose stored bytes differ (trees already the same once unpacked).
static std::string StoredDiff(ArcNode& a, ArcNode& b, const std::string& path) {
  if (a.stored == b.stored) return "";
  if (a.parsed && b.parsed && a.kind != ArcKind::Leaf)
    for (size_t i = 0; i < a.kids.size() && i < b.kids.size(); ++i) {
      const std::string d = StoredDiff(a.kids[i], b.kids[i], path + "/" + a.kids[i].group + (a.kids[i].group.empty() ? "" : ":") + a.kids[i].name);
      if (!d.empty()) return d;
    }
  size_t d = 0;
  while (d < a.stored.size() && d < b.stored.size() && a.stored[d] == b.stored[d]) ++d;
  return path + " (" + std::to_string(a.stored.size()) + " vs " + std::to_string(b.stored.size()) + " bytes, from " + std::to_string(d) + ")";
}

// The deepest container (to depth 4) with at least two leaf children: the path to it (indexes).
static bool EditTarget(ArcNode& n, std::vector<size_t>& path, int depth) {
  if (depth > 4 || !ArchiveExpand(n)) return false;
  for (size_t i = 0; i < n.kids.size(); ++i) {
    path.push_back(i);
    if (EditTarget(n.kids[i], path, depth + 1)) return true;
    path.pop_back();
  }
  size_t leaves = 0;
  for (auto& k : n.kids) leaves += !ArchiveExpand(k);
  return depth > 0 && leaves >= 2 && n.kids.size() >= 2;
}

// archive-edit-test <pac>...: in the deepest list: the first leaf replaced (+ 8 bytes), the second removed, an
// entry added (EPAC: a new name, PACH / SHDC: the removed id again, HSPC: refused with the reason) and (EPAC) one
// renamed; saved, opened again: the same tree as the edited one in memory.
static int ArchiveEditTest(int n, char** files) {
  int bad = 0;
  for (int f = 0; f < n; ++f) {
    Bytes in;
    std::string err;
    Archive a;
    if (!ReadFile(files[f], in) || !ArchiveOpen(in, a, &err)) { std::printf("FAIL open %s: %s\n", files[f], err.c_str()); ++bad; continue; }
    std::vector<size_t> path;
    if (!EditTarget(a.root, path, 0)) {   // (no nested list: the top one)
      path.clear();
      if (a.root.kids.size() < 2) { std::printf("skip %s: fewer than 2 entries\n", files[f]); continue; }
    }
    ArcNode* p = &a.root;
    std::string where;
    for (size_t i : path) where += "/" + p->kids[i].group + (p->kids[i].group.empty() ? "" : ":") + p->kids[i].name, p = &p->kids[i];
    size_t leaf = 0;
    while (leaf < p->kids.size() && ArchiveExpand(p->kids[leaf])) ++leaf;
    if (leaf == p->kids.size()) { std::printf("skip %s: no plain entry in the top list\n", files[f]); continue; }
    Bytes data = ArchiveData(p->kids[leaf]);
    App(data, reinterpret_cast<const uint8_t*>("EDITTEST"), 8);
    ArchiveReplace(p->kids[leaf], data);
    const size_t gone = leaf + 1 < p->kids.size() ? leaf + 1 : (leaf ? 0 : 1);
    const ArcNode removed = p->kids[gone];
    ArchiveRemove(*p, gone);
    std::string what = "replaced " + p->kids[leaf > gone ? leaf - 1 : leaf].name + ", removed " + removed.name;
    const Bytes extra = {'N', 'E', 'W', '!', 1, 2, 3, 4};
    if (p->kind == ArcKind::Epac) {
      const std::string nm = ArchiveNameMax(*p) == 8 ? "ZZTEST1" : "ZZT1";
      if (!ArchiveAdd(*p, removed.group, nm, 0, extra, &err)) { std::printf("FAIL %s: add: %s\n", files[f], err.c_str()); ++bad; continue; }
      if (!ArchiveRename(*p, p->kids.size() - 1, ArchiveNameMax(*p) == 8 ? "ZZTEST2" : "ZZT2", &err)) {
        std::printf("FAIL %s: rename: %s\n", files[f], err.c_str());
        ++bad;
        continue;
      }
      what += ", added " + removed.group + "/" + nm + ", renamed the last one";
    } else if (p->kind == ArcKind::Hspc) {
      what += ArchiveAdd(*p, "", "", 1, extra, &err) ? ", ADDED (should be refused)" : ", add refused";
    } else if (!ArchiveAdd(*p, "", "", removed.id, extra, &err)) {
      std::printf("FAIL %s: add %s: %s\n", files[f], removed.name.c_str(), err.c_str());
      ++bad;
      continue;
    } else {
      what += ", added " + removed.name + " back (new data)";
    }
    const Bytes out = ArchiveSave(a);
    Archive b;
    std::string diff;
    if (!ArchiveOpen(out, b, &err)) diff = "the saved file doesn't open: " + err;
    else diff = TreeDiff(a.root, b.root, "", 8);
    const bool ok = diff.empty() && out != in;
    std::printf("%s %s: %s in %s; %zu -> %zu bytes%s%s\n", ok ? "ok  " : "FAIL", files[f], what.c_str(),
                where.empty() ? "the top list" : where.c_str(), in.size(), out.size(), diff.empty() ? "" : "; differs: ", diff.c_str());
    bad += !ok;
  }
  return bad ? 1 : 0;
}

static int ArchiveTest(int n, char** files, bool list) {
  int bad = 0;
  for (int i = 0; i < n; ++i) {
    Bytes in;
    std::string err;
    Archive a;
    if (!ReadFile(files[i], in) || !ArchiveOpen(in, a, &err)) {
      std::printf("FAIL open %s: %s\n", files[i], err.c_str());
      ++bad;
      continue;
    }
    if (list) {
      std::printf("%s: %s, %zu entries\n", files[i], a.format.c_str(), a.root.kids.size());
      ListNode(a.root, 0, 3);
      continue;
    }
    const bool same0 = ArchiveSave(a) == in;
    Bytes plain_in = in;
    if (a.xcompress) XcompressDecode(in, plain_in);
    ArchiveTouchAll(a.root);
    const Bytes out = ArchiveSave(a);
    Bytes plain_out = out;
    bool readable = true;
    if (a.xcompress) readable = XcompressDecode(out, plain_out);
    bool same1 = readable && plain_out == plain_in;
    std::string tree;
    if (!same1) {   // (a BPE container the rebuild packs again: the same once every level is unpacked?)
      Archive b, c;
      if (!ArchiveOpen(out, b, &err) || !ArchiveOpen(in, c, &err)) tree = "rebuilt file unreadable: " + err;
      else tree = TreeDiff(c.root, b.root, "", 8);
      if (tree.empty()) { std::printf("ok   %s: %s; unchanged save identical; rebuilt the same once unpacked (stored differently: %s)\n", files[i], a.format.c_str(), StoredDiff(c.root, b.root, "").c_str()); bad += !same0; continue; }
      std::printf("     tree: %s\n", tree.c_str());
    }
    size_t diff = 0;
    if (!same1) while (diff < plain_out.size() && diff < plain_in.size() && plain_out[diff] == plain_in[diff]) ++diff;
    std::printf("%s %s: %s; unchanged save %s; rebuilt %s%s\n", same0 && same1 ? "ok  " : "FAIL", files[i], a.format.c_str(),
                same0 ? "identical" : "DIFFERS", same1 ? (a.xcompress ? "the same once decoded" : "identical") : "DIFFERS",
                same1 ? "" : (" at 0x" + [&] { char b[24]; std::snprintf(b, sizeof b, "%zX (%zu vs %zu bytes)", diff, plain_out.size(), plain_in.size()); return std::string(b); }()).c_str());
    bad += !(same0 && same1);
  }
  return bad ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc >= 3 && !std::strcmp(argv[1], "archive-test")) return ArchiveTest(argc - 2, argv + 2, false);
  if (argc >= 3 && !std::strcmp(argv[1], "archive-list")) return ArchiveTest(argc - 2, argv + 2, true);
  if (argc >= 3 && !std::strcmp(argv[1], "archive-edit-test")) return ArchiveEditTest(argc - 2, argv + 2);
  if ((argc == 7 || argc == 8) && !std::strcmp(argv[1], "makemod"))
    return MakeMod(argv[2], argv[3], argv[4], argv[5], argv[6], argc == 8 ? argv[7] : nullptr);
  if (argc == 5 && !std::strcmp(argv[1], "ring")) return Ring(argv[2], argv[3], argv[4]);
  if (argc >= 5 && !std::strcmp(argv[1], "wwe13")) return Wwe13(argc, argv);
  if ((argc == 4 || argc == 5) && !std::strcmp(argv[1], "wwe13prop")) {  // <entry> <out> [scale]: an entrance prop (entrance_port)
    Bytes in, out;
    std::string err;
    if (!ReadFile(argv[2], in) || !ConvertProp13(in, out, argc == 5 ? float(std::atof(argv[4])) : 0.1f, &err)) {
      std::printf("%s: %s\n", argv[2], err.c_str());
      return 1;
    }
    return WriteFile(argv[3], out) ? 0 : 1;
  }
  if (argc == 4 && !std::strcmp(argv[1], "xdec")) {
    Bytes in, out;
    std::string err;
    if (!ReadFile(argv[2], in) || !XcompressDecode(in, out, &err)) { std::printf("%s: %s\n", argv[2], err.c_str()); return 1; }
    return WriteFile(argv[3], out) ? 0 : 1;
  }
  if (argc == 5 && !std::strcmp(argv[1], "import")) return Import(argv[2], argv[3], argv[4]);
  if ((argc == 3 || argc == 4) && !std::strcmp(argv[1], "objects")) return Objects(argc, argv);
  if (argc >= 6 && !std::strcmp(argv[1], "obj-add")) return ObjAdd(argc, argv);
  if ((argc == 5 || argc == 6) && !std::strcmp(argv[1], "obj-remove")) return ObjRemove(argc, argv);
  if (argc == 6 && !std::strcmp(argv[1], "obj-export")) return ObjExport(argv);
  if (argc == 4 && !std::strcmp(argv[1], "bpe")) return Bpe(argv[2], argv[3]);
  if ((argc == 11 || argc == 12) && !std::strcmp(argv[1], "move")) return Move(argc, argv);
  if (argc == 4 && !std::strcmp(argv[1], "tablelid")) return TableLid(argv[2], argv[3]);
  if ((argc == 4 || argc == 5) && !std::strcmp(argv[1], "lights")) return Lights(argv[2], argv[3], argc == 5 ? argv[4] : nullptr);
  if (argc >= 3 && !std::strcmp(argv[1], "bpetest")) return BpeTest(argv[2], argc >= 4 ? argv[3] : nullptr);
  if (argc >= 3 && !std::strcmp(argv[1], "roundtrip")) return Roundtrip(argc - 2, argv + 2);
  if (argc == 4 && !std::strcmp(argv[1], "export")) return Export(argv[2], argv[3]);
  if (argc == 4 && !std::strcmp(argv[1], "export-full")) return ExportFull(argv[2], argv[3]);
  if (argc == 4 && !std::strcmp(argv[1], "banner")) {
    // any picture -> the arena select banner: 256 x 128 DXT5, one level
    Image img;
    if (!LoadImageFile(argv[2], img)) { std::printf("%s: cannot read\n", argv[2]); return 1; }
    return WriteFile(argv[3], DdsEncode(Resize(img, 256, 128), DxtFormat::kDxt5, false)) ? 0 : 1;
  }
  if (argc == 6 && !std::strcmp(argv[1], "dds")) {
    // dds <picture> <w> <h> <out.dds>: DXT5, one level (VS screen pictures)
    Image img;
    if (!LoadImageFile(argv[2], img)) { std::printf("%s: cannot read\n", argv[2]); return 1; }
    return WriteFile(argv[5], DdsEncode(Resize(img, std::atoi(argv[3]), std::atoi(argv[4])), DxtFormat::kDxt5, false))
               ? 0 : 1;
  }
  if (argc == 5 && !std::strcmp(argv[1], "retexture")) {
    // retexture <bgNN.pac> <map.txt> <out.pac>: lines "<arena texture>=<picture>"
    // (a picture at its own size, power of two, in the texture's format and
    // mip layout); then FitFile and a size report
    Arena a;
    std::string err;
    if (!a.Load(argv[2], &err)) { std::printf("%s: %s\n", argv[2], err.c_str()); return 1; }
    Bytes mapb;
    if (!ReadFile(argv[3], mapb)) { std::printf("%s: cannot read\n", argv[3]); return 1; }
    std::string map(mapb.begin(), mapb.end()), line;
    size_t pos = 0, done = 0, missing = 0, before = 0, after = 0;
    std::vector<std::string> keep;
    while (pos < map.size()) {
      size_t nl = map.find('\n', pos);
      line = map.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      pos = nl == std::string::npos ? map.size() : nl + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      const size_t eq = line.find('=');
      if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
      const std::string name = line.substr(0, eq), pic = line.substr(eq + 1);
      bool found = false;
      for (auto& b : a.bundles)
        for (auto& t : b.textures) {
          if (t.name != name) continue;
          found = true;
          DdsInfo info;
          Image img;
          if (!DdsInfoOf(t.data, info) || !LoadImageFile(pic, img)) { std::printf("  %s: cannot use %s\n", name.c_str(), pic.c_str()); continue; }
          auto p2 = [](int v) { int p = 4; while (p < v && p < 2048) p *= 2; return p; };
          img = Resize(img, p2(img.w), p2(img.h));
          const DxtFormat f = info.format == DxtFormat::kArgb ? DxtFormat::kDxt5 : info.format;
          before += t.data.size();
          t.data = DdsEncode(img, f, info.mips > 1);
          after += t.data.size();
          b.changed = true;
          keep.push_back(name);
          ++done;
        }
      if (!found) { std::printf("  no texture %s in the arena\n", name.c_str()); ++missing; }
    }
    std::printf("retextured %zu (%zu not found): %.2f MB -> %.2f MB\n", done, missing, before / 1048576.0, after / 1048576.0);
    const auto halved = a.FitFile(keep);  // (the arena's own textures give way first)
    if (!halved.empty()) {
      std::printf("  over the room: %zu textures halved to fit:", halved.size());
      for (const auto& h : halved) std::printf(" %s", h.c_str());
      std::printf("\n");
    }
    const Bytes data = a.Save(&err);
    if (!err.empty()) { std::printf("error: %s\n", err.c_str()); return 1; }
    WriteFile(argv[4], data);
    std::printf("wrote %s: %.2f MB (shipped %.2f MB)\n", argv[4], data.size() / 1048576.0, a.original_file / 1048576.0);
    return 0;
  }
  if (argc == 5 && !std::strcmp(argv[1], "grow")) {
    // test: grow <bgNN.pac> <out.pac> <KB>: an unused texture of about that
    // many KB unpacked (a soft pattern, so it packs well) in the first set
    Arena a;
    std::string err;
    if (!a.Load(argv[2], &err) || a.bundles.empty()) { std::printf("%s: %s\n", argv[2], err.c_str()); return 1; }
    const int kb = std::atoi(argv[4]);
    int side = 64;
    while (side * side * 4 / 3 < kb * 1024 && side < 4096) side *= 2;  // DXT5: 1 byte per pixel (+ mips)
    Image img;
    img.w = img.h = side;
    img.rgba.resize(size_t(side) * side * 4);
    for (int y = 0; y < side; ++y)
      for (int x = 0; x < side; ++x) {
        uint8_t* p = &img.rgba[(size_t(y) * side + x) * 4];
        p[0] = uint8_t(x * 255 / side), p[1] = uint8_t(y * 255 / side), p[2] = uint8_t((x ^ y) & 0x40), p[3] = 255;
      }
    a.bundles[0].textures.push_back({"zz_grow", "dds", DdsEncode(img, DxtFormat::kDxt5, true)});
    a.bundles[0].changed = true;
    const Bytes data = a.Save(&err);
    if (!err.empty()) { std::printf("error: %s\n", err.c_str()); return 1; }
    WriteFile(argv[3], data);
    std::printf("wrote %s: %zu bytes (shipped %zu), texture %dx%d\n", argv[3], data.size(), a.original_file, side, side);
    return 0;
  }
  std::printf("usage: svrmod roundtrip <file.pac>... | export <bgNN.pac> <out dir> | import <bgNN.pac> <fbx> <out.pac>\n"
              "       | banner <picture> <out.dds> | bpe <raw> <out> | bpetest <file.pac> [out.pac]\n"
              "       | tablelid <arena.pac> <out.pac>\n");
  return 2;
}
