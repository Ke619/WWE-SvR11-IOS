// WWE SmackDown vs. Raw 2011 - custom touch-control icons (see touch_icons.h).

#include "touch_icons.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <system_error>

#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "util.h"

namespace svr2011 {

namespace {

namespace fs = std::filesystem;

constexpr uintmax_t kMaxFileBytes = 2u * 1024 * 1024;  // a button icon is small
constexpr int kMaxSide = 1024;                          // pixels
constexpr size_t kMaxCached = 64;                       // pictures kept in memory
constexpr size_t kMaxListed = 200;                      // pictures the editor cycles through
constexpr size_t kMaxNameBytes = 120;

struct Entry {
  std::unique_ptr<rex::ui::ImmediateTexture> texture;  // null: failed
  float width = 0, height = 0;
};

rex::ui::ImmediateDrawer* g_drawer = nullptr;
fs::path g_folder;

// The textures belong to the drawer, which may already be gone when static
// destructors run at exit: the cache is never destroyed.
std::map<std::string, Entry>& Cache() {
  static auto* cache = new std::map<std::string, Entry>();
  return *cache;
}

// File names are UTF-8 in the layout file and in the editor.
fs::path FromUtf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string ToUtf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

using svr2011::Lower;

// Reads `name` from the icons folder into a texture; false (with a log line)
// when it cannot.
bool Load(const std::string& name, Entry* out) {
  if (!g_drawer || g_folder.empty() || !TouchIconNameValid(name)) return false;
  const fs::path path = g_folder / FromUtf8(name);
  std::error_code ec;
  const uintmax_t size = fs::file_size(path, ec);
  if (ec || size == 0 || size > kMaxFileBytes) {
    if (!ec) REXLOG_WARN("touch icons: {} is empty or over {} bytes", name, kMaxFileBytes);
    return false;
  }
  std::vector<unsigned char> bytes(static_cast<size_t>(size));
  {
    std::ifstream f(path, std::ios::binary);
    if (!f || !f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return false;
  }
  // The size first: a small file can claim a huge picture.
  int w = 0, h = 0, n = 0;
  if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n) || w < 1 || h < 1 || w > kMaxSide ||
      h > kMaxSide) {
    REXLOG_WARN("touch icons: {} is not a PNG of at most {} x {} pixels", name, kMaxSide, kMaxSide);
    return false;
  }
  stbi_uc* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
  if (!pixels) {
    REXLOG_WARN("touch icons: could not read {}", name);
    return false;
  }
  out->texture = g_drawer->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false,
                                         pixels);
  stbi_image_free(pixels);
  if (!out->texture) return false;
  out->width = float(w);
  out->height = float(h);
  return true;
}

}  // namespace

void TouchIconsInit(rex::ui::ImmediateDrawer* drawer, const std::filesystem::path& user_data) {
  g_drawer = drawer;
  g_folder = user_data / "touch_icons";
  std::error_code ec;
  fs::create_directories(g_folder, ec);
}

const std::filesystem::path& TouchIconsFolder() { return g_folder; }

bool TouchIconNameValid(const std::string& name) {
  if (name.empty() || name.size() > kMaxNameBytes || name.front() == '.' || name.find("..") != std::string::npos)
    return false;
  for (char c : name) {
    if (static_cast<unsigned char>(c) < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|')
      return false;
  }
  return name.size() > 4 && Lower(name.substr(name.size() - 4)) == ".png";
}

std::vector<std::string> TouchIconList() {
  std::vector<std::string> names;
  std::error_code ec;
  if (g_folder.empty()) return names;
  for (fs::directory_iterator it(g_folder, ec), end; !ec && it != end && names.size() < kMaxListed; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    const std::string name = ToUtf8(it->path().filename());
    if (TouchIconNameValid(name)) names.push_back(name);
  }
  std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
    const std::string la = Lower(a), lb = Lower(b);
    return la != lb ? la < lb : a < b;
  });
  return names;
}

bool TouchIconGet(const std::string& name, TouchIcon* out) {
  if (name.empty()) return false;
  auto& cache = Cache();
  auto it = cache.find(name);
  if (it == cache.end()) {
    if (cache.size() >= kMaxCached) return false;
    Entry e;
    if (!Load(name, &e)) e = Entry{};
    it = cache.emplace(name, std::move(e)).first;
  }
  if (!it->second.texture) return false;
  if (out) {
    out->texture = it->second.texture.get();
    out->width = it->second.width;
    out->height = it->second.height;
  }
  return true;
}

void TouchIconsRetry() {
  // (a loaded picture stays: the GPU may still be drawing it)
  auto& cache = Cache();
  for (auto it = cache.begin(); it != cache.end();) {
    if (!it->second.texture) it = cache.erase(it);
    else ++it;
  }
}

}  // namespace svr2011
