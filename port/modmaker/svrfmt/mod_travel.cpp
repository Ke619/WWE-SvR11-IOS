// WWE SmackDown vs. Raw 2011 - which mod files may travel online (mod_travel.h).
#include "mod_travel.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace svrfmt {
namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> Split(const std::string& s, char by) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == by) out.push_back(cur), cur.clear();
    else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

}  // namespace

bool ModFileAllowed(const std::string& rel, std::string* why) {
  auto no = [&](const char* w) {
    if (why) *why = w;
    return false;
  };
  if (rel.empty() || rel.size() > 180) return no("an empty or too long path");
  if (rel.front() == '/' || rel.find('\\') != std::string::npos) return no("not a relative path");
  for (unsigned char c : rel)
    if (c < 0x20 || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
      return no("a character a file name can't have (':' and the like)");
  const auto parts = Split(rel, '/');
  if (parts.size() > 8) return no("too deep");
  static const std::set<std::string> kDevices = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4",
                                                 "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                                 "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9", "conin$",
                                                 "conout$"};
  for (const std::string& p : parts) {
    if (p.empty() || p == "." || p == "..") return no("a '..' or empty part");
    if (p.back() == '.' || p.back() == ' ') return no("a name ending in a dot or space");
    if (kDevices.count(Lower(p.substr(0, p.find('.'))))) return no("a device name (CON, NUL ...)");
  }
  const std::string name = Lower(parts.back());
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos) return no("no file type");
  const std::string ext = name.substr(dot), stem = name.substr(0, dot);
  static const std::set<std::string> kKinds = {".pac", ".bik", ".mp4", ".mp3", ".wav", ".m4a", ".ogg", ".xma",
                                               ".dds", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".ymk", ".ymb",
                                               ".txt"};
  if (kKinds.count(ext)) return true;
  // (.bin: the moves' entrance event files - a superstar's moves/..., a move pack's own entrance/...)
  for (size_t k = 0; ext == ".bin" && k + 1 < parts.size(); ++k)
    if (const std::string d = Lower(parts[k]); d == "moves" || d == "entrance") return true;
  if ((ext == ".toml" || ext == ".ini") && stem == "manifest") return true;
  return no("a file type that isn't sent: only the game's and mods' own kinds");
}

bool CosmeticFile(const fs::path& file) {
  // (this PC's own state: the launcher's .applied, the "disabled" switch - never part of a mod's key)
  const std::string n = file.filename().string();
  if (!n.empty() && n[0] == '.') return true;
  if (Lower(n) == "disabled") return true;
  static const std::set<std::string> kCosmetic = {".png", ".jpg", ".jpeg", ".bmp", ".dds", ".tga", ".ogg",
                                                  ".wav", ".mp3", ".xma", ".bik", ".wmv", ".mp4", ".db", ".ini"};
  return kCosmetic.count(Lower(file.extension().string())) != 0;
}

std::string ManifestKeyText(const std::string& manifest) {
  std::string out;
  for (size_t at = 0; at < manifest.size();) {
    size_t end = manifest.find('\n', at);
    if (end == std::string::npos) end = manifest.size();
    std::string line = manifest.substr(at, end - at);
    at = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    const std::string key = line.substr(0, line.find('='));
    const auto is = [&](const char* name) {  // ("name", or "name.<n>" - a media pack's)
      const size_t n = std::char_traits<char>::length(name);
      return key.compare(0, n, name) == 0 && (key.size() == n || key[n] == '.');
    };
    if (is("theme_gain") || is("theme_gain_auto") || is("movie") || is("video")) continue;
    if (is("victory_song") || is("victory_gain") || is("victory_theme") || is("victory_theme_gain")) continue;  // (each player hears their own)
    out += line + "\n";
  }
  return out;
}

bool ModCanTravel(const fs::path& dir, std::string* unsendable) {
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec))
    if (e.is_regular_file(ec)) files.push_back(e.path());
  std::sort(files.begin(), files.end());  // (the same first file on every PC)
  for (const fs::path& f : files) {
    const std::string rel = fs::relative(f, dir, ec).generic_string();
    std::string why;
    if (!ModFileAllowed(rel, &why) && !CosmeticFile(f)) {
      if (unsendable) *unsendable = rel + " (" + why + ")";
      return false;
    }
  }
  return true;
}

}  // namespace svrfmt
