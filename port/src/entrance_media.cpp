// WWE SmackDown vs. Raw 2011 - Community Creations: a Created Superstar's
// entrance song and movie go with it (see entrance_media.h).
//
// Where the game keeps a Superstar's entrance:
//   - the main save (SaveData.dat, in memory at **0x82E407B4, the file less
//     its 0x1C-byte header) has a 0x6BC-byte profile record per Created
//     Superstar at +0x13F9C (slot k: NNCreateSuperStar.cas, k = NN); the
//     roster's own Superstars have 0x420-byte ones (sub_8257BB48 returns
//     either's shared part: record + 0x104 for a Created Superstar);
//   - the shared part is the move-set (0x1C0 bytes) and the entrance (284
//     bytes, record + 0x2C4): its movie as a u16 menu row and a u16 movie id
//     at +0x10 / +0x12 (row 254, HIGHLIGHT REEL, with ids 700-899: a user
//     movie, user_movies.cpp) and its USER PLAYLIST song by name at +0xCC (40
//     UTF-16BE characters with the terminator);
//   - the .cas has a copy of the record at 0x1483C4. A Community Creations
//     upload sends the .cas with the movie row reset to 255 (the game's
//     "Entrance Movie Highlight Reel" reset); a download's record is the
//     .cas's copy.
//
// Upload: the relay passes the .cas the server took (online_net.h); its
// record is matched with the records in memory for the slot, whose entrance
// says which song and movie it uses. The song goes to the server as it is,
// the movie as an MP4 (cached in Custom Movies\.cc), and PUT
// /api/entrance/<file> ties them to the Superstar.
// Download: the relay passes the file id; GET /api/entrance/<file> says what
// comes with it. The names are settled at once (a playlist or movie of the
// same name with a different file: "<name> (2)"; the same file: it is used)
// and the files fetched in the background. When the game starts saving the
// Superstar (sub_824DCA98, online.cpp) its .cas's entrance gets the local
// song name and movie (row 254, the movie's id - reserved even while the
// movie is still being written).
#include "entrance_media.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include "movie_transcode.h"
#include "music.h"
#include "online_net.h"
#include "user_movies.h"
#include "../modmaker/svrfmt/bytes.h"

namespace svr2011 {

namespace {

constexpr uint32_t kSaveData = 0x82E407B4;  // -> the main save in memory
constexpr uint32_t kCawRecords = 0x13F9C, kCawRecordSize = 0x6BC, kCawSlots = 50;
constexpr uint32_t kCasSize = 1347532, kCasRecord = 0x1483C4;
constexpr uint32_t kRecordCompare = 0x54C;  // (the part of the record the .cas copy keeps)
constexpr uint32_t kEntrance = 0x2C4;
constexpr uint32_t kMovieRow = 0x10, kMovieId = 0x12, kSongName = 0xCC, kSongChars = 40;
constexpr uint16_t kHighlightReelRow = 254;
constexpr int kFirstUserMovie = 700, kLastUserMovie = 899;
// (the server makes songs MP3s at the game's rate and keeps 4 minutes of a movie)
constexpr uint64_t kMaxSong = 256ull << 20, kMaxMovie = 256ull << 20;

rex::memory::Memory* g_memory = nullptr;

// -- small helpers ------------------------------------------------------------------

using svrfmt::Rd32, svrfmt::Rd16, svrfmt::Wr16;

std::u16string SongName(const uint8_t* entrance) {
  std::u16string name;
  for (uint32_t i = 0; i < kSongChars; ++i) {
    const char16_t c = char16_t(Rd16(entrance + kSongName + 2 * i));
    if (!c) break;
    name.push_back(c);
  }
  return name;
}

void SetSongName(uint8_t* entrance, const std::u16string& name) {
  for (uint32_t i = 0; i < kSongChars; ++i) {
    Wr16(entrance + kSongName + 2 * i, i < name.size() && i + 1 < kSongChars ? uint16_t(name[i]) : 0);
  }
}

// UTF-8 in a std::string (a path's name, the server's JSON) and back.
std::string U8(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
std::filesystem::path FromU8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
std::string Utf8(const std::u16string& s) { return U8(std::filesystem::path(s)); }
std::u16string Utf16(const std::string& s) { return FromU8(s).u16string(); }

std::optional<std::string> ReadFile(const std::filesystem::path& file, uint64_t max) {
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(file, ec);
  if (ec || size == 0 || size > max) return std::nullopt;
  std::ifstream in(file, std::ios::binary);
  std::string data(size, '\0');
  if (!in.read(data.data(), std::streamsize(size))) return std::nullopt;
  return data;
}

// SHA-256 (the server keeps songs and movies by it).
std::string Sha256(const std::string& data) {
  static constexpr uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  std::string msg = data;
  const uint64_t bits = uint64_t(data.size()) * 8;
  msg.push_back(char(0x80));
  while (msg.size() % 64 != 56) msg.push_back('\0');
  for (int i = 7; i >= 0; --i) msg.push_back(char(bits >> (8 * i)));
  for (size_t off = 0; off < msg.size(); off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const uint8_t*>(msg.data() + off + 4 * i);
      w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
  }
  std::string hex;
  for (uint32_t v : h) hex += fmt::format("{:08x}", v);
  return hex;
}

// -- JSON (the few fields the server sends and takes) ------------------------------

std::string JsonQuote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\', out += char(c);
    } else if (c < 0x20) {
      out += fmt::format("\\u{:04x}", c);
    } else {
      out += char(c);
    }
  }
  return out + "\"";
}

// The string at `at` (its opening quote), unescaped to UTF-8.
std::string JsonString(const std::string& json, size_t at) {
  std::u16string units;
  std::string out;
  auto flush = [&] {
    if (!units.empty()) out += Utf8(units), units.clear();
  };
  for (size_t i = at + 1; i < json.size() && json[i] != '"'; ++i) {
    if (json[i] != '\\') {
      flush();
      out += json[i];
      continue;
    }
    const char e = json[++i];
    if (e == 'u' && i + 4 < json.size()) {
      units.push_back(char16_t(std::stoul(json.substr(i + 1, 4), nullptr, 16)));
      i += 4;
      continue;
    }
    flush();
    out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
  }
  flush();
  return out;
}

// object.key's string value, where object is a top-level key ("" = top level).
std::string JsonGet(const std::string& json, const std::string& object, const std::string& key) {
  size_t from = 0, to = json.size();
  if (!object.empty()) {
    const size_t at = json.find("\"" + object + "\"");
    if (at == std::string::npos) return {};
    from = json.find('{', at);
    to = json.find('}', from);
    if (from == std::string::npos || to == std::string::npos) return {};
  }
  const size_t k = json.find("\"" + key + "\"", from);
  if (k == std::string::npos || k > to) return {};
  const size_t q = json.find('"', json.find(':', k));
  return q == std::string::npos || q > to ? std::string() : JsonString(json, q);
}

// The song playlist `name` plays (music.h), empty if there is none.
std::filesystem::path PlaylistSong(const std::u16string& name) { return UserMusicSong(Utf8(name)); }

// "<name> (n)" within the entrance's 39 characters.
std::u16string Numbered(const std::u16string& name, int n, size_t max_chars) {
  if (n <= 1) return name.substr(0, max_chars);
  const std::u16string suffix = Utf16(fmt::format(" ({})", n));
  return name.substr(0, max_chars - suffix.size()) + suffix;
}

// -- uploads -------------------------------------------------------------------------

// The file in a multipart/form-data body.
std::string MultipartFile(const std::string& content_type, const std::string& body) {
  const size_t b = content_type.find("boundary=");
  if (b == std::string::npos) return {};
  std::string boundary = content_type.substr(b + 9);
  if (!boundary.empty() && boundary.front() == '"') boundary = boundary.substr(1, boundary.find('"', 1) - 1);
  const std::string delimiter = "--" + boundary;
  size_t part = body.find(delimiter);
  while (part != std::string::npos) {
    const size_t head_end = body.find("\r\n\r\n", part);
    if (head_end == std::string::npos) break;
    const std::string head = body.substr(part, head_end - part);
    const size_t next = body.find("\r\n" + delimiter, head_end + 4);
    if (next == std::string::npos) break;
    if (head.find("filename=") != std::string::npos) return body.substr(head_end + 4, next - head_end - 4);
    part = next + 2;
  }
  return {};
}

// The slot whose record the uploaded .cas carries, -1 if none.
int UploadedSlot(const uint8_t* base, const std::string& cas) {
  const uint32_t save = Rd32(base, kSaveData);
  if (!save) return -1;
  const auto* copy = reinterpret_cast<const uint8_t*>(cas.data() + kCasRecord);
  for (uint32_t k = 0; k < kCawSlots; ++k) {
    const uint8_t* rec = base + save + kCawRecords + k * kCawRecordSize;
    const uint32_t row = kEntrance + kMovieRow;  // (the upload resets the movie row)
    if (std::memcmp(rec, copy, row) == 0 &&
        std::memcmp(rec + row + 2, copy + row + 2, kRecordCompare - row - 2) == 0) {
      return int(k);
    }
  }
  return -1;
}

// Sends a song or movie unless the server has it; its sha, or "" if it failed.
std::string SendMedia(const char* kind, const std::string& data) {
  const std::string sha = Sha256(data);
  if (auto r = net::ServerRequest("HEAD", "/api/media/" + sha); r && r->status == 200) return sha;
  auto r = net::ServerRequest("POST", fmt::format("/api/media?kind={}", kind), data,
                              {{"Content-Type", "application/octet-stream"}});
  if (!r || r->status != 200) {
    REXLOG_WARN("online: entrance {} not sent ({})", kind,
                r ? fmt::format("HTTP {}: {}", r->status, r->body.substr(0, 120)) : "no answer");
    return {};
  }
  return sha;
}

void SendEntrance(int fileid, int slot, std::u16string song_name, std::filesystem::path movie) {
  rex::thread::set_current_thread_name("entrance send");
  std::string music_json, movie_json;
  if (!song_name.empty()) {
    const auto song = PlaylistSong(song_name);
    if (song.empty()) {
      REXLOG_WARN("online: Superstar {}'s entrance song \"{}\" isn't in {}", slot + 1, Utf8(song_name),
                  U8(UserMusicFolder()));
    } else if (auto data = ReadFile(song, kMaxSong)) {
      if (const std::string sha = SendMedia("music", *data); !sha.empty()) {
        music_json = fmt::format("{{\"playlist\": {}, \"file\": {}, \"sha\": \"{}\"}}", JsonQuote(Utf8(song_name)),
                                 JsonQuote(U8(song.filename())), sha);
      }
    } else {
      REXLOG_WARN("online: entrance song {} not sent (over {} MB?)", U8(song.filename()), kMaxSong >> 20);
    }
  }
  if (!movie.empty()) {
    std::error_code ec;
    const auto cache = UserMoviesFolder() / ".cc";
    std::filesystem::create_directories(cache, ec);
    const auto stamp = std::filesystem::last_write_time(movie, ec).time_since_epoch().count();
    auto mp4 = cache / fmt::format("{}.{}.{}.mp4", U8(movie.stem()),
                                   std::filesystem::file_size(movie, ec), stamp);
    // (an HD user movie: its .mp4 as it is - the .bik is its stub or a fallback; user_movies.cpp)
    if (const auto hd = std::filesystem::path(movie).replace_extension(".mp4");
        std::filesystem::is_regular_file(hd, ec) && std::filesystem::file_size(hd, ec) <= kMaxMovie)
      mp4 = hd;
    if (!std::filesystem::exists(mp4, ec) && !BikToMp4(movie, mp4, kMaxMovie)) {
      REXLOG_WARN("online: entrance movie {} not sent (couldn't shrink it)", U8(movie.filename()));
    } else if (auto data = ReadFile(mp4, kMaxMovie)) {
      if (const std::string sha = SendMedia("movie", *data); !sha.empty()) {
        movie_json = fmt::format("{{\"name\": {}, \"sha\": \"{}\"}}", JsonQuote(U8(movie.stem())), sha);
      }
    }
  }
  if (music_json.empty() && movie_json.empty()) return;
  std::string body = "{";
  if (!music_json.empty()) body += "\"music\": " + music_json;
  if (!movie_json.empty()) body += std::string(music_json.empty() ? "" : ", ") + "\"movie\": " + movie_json;
  body += "}";
  auto r = net::ServerRequest("PUT", fmt::format("/api/entrance/{}", fileid), body,
                              {{"Content-Type", "application/json"}});
  REXLOG_INFO("online: Superstar {}'s entrance (song: {}, movie: {}) {}", slot + 1, music_json.empty() ? "-" : "yes",
              movie_json.empty() ? "-" : "yes", r && r->status == 200 ? "sent" : "not sent");
}

void OnUpload(int fileid, const std::string& content_type, const std::string& body) {
  const std::string cas = MultipartFile(content_type, body);
  if (cas.size() != kCasSize || !g_memory) return;
  auto* base = g_memory->TranslateVirtual<uint8_t*>(0);
  const int slot = UploadedSlot(base, cas);
  if (slot < 0) {
    REXLOG_WARN("online: uploaded Superstar (file {}) not found among this save's Superstars", fileid);
    return;
  }
  const uint32_t save = Rd32(base, kSaveData);
  const uint8_t* entrance = base + save + kCawRecords + slot * kCawRecordSize + kEntrance;
  const std::u16string song = SongName(entrance);
  std::filesystem::path movie;
  const int id = Rd16(entrance + kMovieId);
  if (Rd16(entrance + kMovieRow) == kHighlightReelRow && id >= kFirstUserMovie && id <= kLastUserMovie) {
    movie = UserMovieFile(id);
  }
  if (song.empty() && movie.empty()) return;
  std::thread(SendEntrance, fileid, slot, song, movie).detach();
}

// -- downloads -------------------------------------------------------------------------

struct Incoming {
  int fileid = 0;
  bool settled = false;           // (the names below are decided)
  std::u16string song;            // the entrance's song name here ("": as it came)
  int movie_id = 0;               // its movie here (0: none)
};
std::mutex g_mutex;
std::condition_variable g_settled;
Incoming g_incoming;

// Custom Movies\.cc\sources.txt: "<sha><TAB><file>" for movies made from a
// download, so the same movie downloaded again is the same file.
std::map<std::string, std::string> MovieSources() {
  std::map<std::string, std::string> sources;
  std::ifstream in(UserMoviesFolder() / ".cc" / "sources.txt");
  for (std::string line; std::getline(in, line);) {
    const size_t tab = line.find('\t');
    if (tab != std::string::npos) sources[line.substr(0, tab)] = line.substr(tab + 1);
  }
  return sources;
}

void FetchSong(std::filesystem::path file, std::string sha) {
  auto r = net::ServerRequest("GET", "/api/media/" + sha);
  if (!r || r->status != 200 || Sha256(r->body) != sha) {
    REXLOG_WARN("online: entrance song not downloaded");
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::ofstream(file, std::ios::binary).write(r->body.data(), std::streamsize(r->body.size()));
  REXLOG_INFO("online: entrance song saved as {}", U8(file));
}

void FetchMovie(std::string file, std::string sha) {
  const auto folder = UserMoviesFolder();
  std::error_code ec;
  std::filesystem::create_directories(folder / ".cc", ec);
  const auto mp4 = folder / ".cc" / (sha + ".mp4");
  bool ok = false;
  if (auto r = net::ServerRequest("GET", "/api/media/" + sha); r && r->status == 200 && Sha256(r->body) == sha) {
    std::ofstream(mp4, std::ios::binary).write(r->body.data(), std::streamsize(r->body.size()));
    ok = Mp4ToBik(mp4, folder / file);
    std::filesystem::remove(mp4, ec);
  }
  if (ok) {
    std::ofstream(folder / ".cc" / "sources.txt", std::ios::app) << sha << '\t' << file << '\n';
  }
  FinishUserMovie(file);
  REXLOG_INFO("online: entrance movie {} {}", file, ok ? "saved" : "not downloaded");
}

void SettleDownload(int fileid) {
  rex::thread::set_current_thread_name("entrance download");
  Incoming in;
  in.fileid = fileid;
  std::vector<std::thread> fetches;
  auto r = net::ServerRequest("GET", fmt::format("/api/entrance/{}", fileid));
  if (r && r->status == 200) {
    const std::string& json = r->body;
    // the song: Music\<playlist>\<file>, or "<playlist> (2)"... if that name
    // plays a different song here
    if (const std::string sha = JsonGet(json, "music", "sha"); !sha.empty()) {
      const std::u16string playlist = Utf16(JsonGet(json, "music", "playlist"));
      const std::string file = JsonGet(json, "music", "file");
      for (int n = 1; n < 100 && !playlist.empty() && !file.empty(); ++n) {
        const std::u16string name = Numbered(playlist, n, kSongChars - 1);
        const auto song = PlaylistSong(name);
        if (song.empty()) {
          fetches.emplace_back(FetchSong, UserMusicFolder() / std::filesystem::path(name) / FromU8(file), sha);
          in.song = name;
          break;
        }
        if (auto data = ReadFile(song, kMaxSong); data && Sha256(*data) == sha) {
          in.song = name;  // (this one: the same song)
          break;
        }
      }
    }
    // the movie: Custom Movies\<name>.bik (plain letters), made from the MP4
    if (const std::string sha = JsonGet(json, "movie", "sha"); !sha.empty() && CanTranscodeMovies()) {
      std::string name = JsonGet(json, "movie", "name");
      for (char& c : name) {
        if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) c = '_';
      }
      const auto sources = MovieSources();
      std::error_code ec;
      std::string file;
      if (auto it = sources.find(sha); it != sources.end() && std::filesystem::exists(UserMoviesFolder() / it->second, ec)) {
        file = it->second;  // (downloaded before)
      } else {
        for (int n = 1; n < 100 && !name.empty(); ++n) {
          const std::string f = (n == 1 ? name : fmt::format("{} ({})", name, n)) + ".bik";
          if (!std::filesystem::exists(UserMoviesFolder() / f, ec) &&
              !std::filesystem::exists(UserMoviesFolder() / (f + ".part"), ec)) {
            file = f;
            break;
          }
        }
        if (!file.empty() && (in.movie_id = ReserveUserMovie(file)) != 0) fetches.emplace_back(FetchMovie, file, sha);
      }
      if (!file.empty() && !in.movie_id) in.movie_id = ReserveUserMovie(file);
    }
  }
  in.settled = true;
  {
    std::lock_guard lock(g_mutex);
    if (g_incoming.fileid == fileid) g_incoming = in;
  }
  g_settled.notify_all();
  if (!in.song.empty() || in.movie_id) {
    REXLOG_INFO("online: file {}'s entrance: song \"{}\", movie {}", fileid, Utf8(in.song), in.movie_id);
  }
  for (auto& t : fetches) t.join();
}

void OnTransfer(bool upload, int fileid, const std::string& content_type, const std::string& body) {
  if (upload) {
    OnUpload(fileid, content_type, body);
    return;
  }
  if (body.size() != kCasSize) return;  // (Superstars only)
  {
    std::lock_guard lock(g_mutex);
    g_incoming = Incoming{fileid};
  }
  std::thread(SettleDownload, fileid).detach();
}

}  // namespace

void InstallEntranceMedia(rex::memory::Memory* memory) {
  g_memory = memory;
  net::SetTransferListener(OnTransfer);
}

void PrepareDownloadedEntrance(uint8_t* base, uint32_t cas) {
  Incoming in;
  {
    std::unique_lock lock(g_mutex);
    if (!g_incoming.fileid) return;
    // (the server answers in well under a second; the player picked a slot since)
    g_settled.wait_for(lock, std::chrono::seconds(5), [] { return g_incoming.settled; });
    in = g_incoming;
    g_incoming = {};
  }
  if (!in.settled || !cas) return;
  uint8_t* entrance = base + cas + kCasRecord + kEntrance;
  if (!in.song.empty() && in.song != SongName(entrance)) SetSongName(entrance, in.song);
  if (in.movie_id) {
    Wr16(entrance + kMovieRow, kHighlightReelRow);
    Wr16(entrance + kMovieId, uint16_t(in.movie_id));
  }
  REXLOG_INFO("online: downloaded Superstar's entrance: song \"{}\", movie {}", Utf8(SongName(entrance)),
              in.movie_id ? fmt::format("{} ({})", in.movie_id, U8(UserMovieFile(in.movie_id).filename()))
                          : std::string("as it came"));
}

}  // namespace svr2011
