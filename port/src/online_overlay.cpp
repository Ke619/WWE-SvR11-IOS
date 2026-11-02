// WWE SmackDown vs. Raw 2011 - the ONLINE overlay (online_overlay.h).

#include "online_overlay.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fstream>

#include <imgui.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/kernel/xam/module.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "generated/default/svr2011_init.h"
#include "json_lite.h"
#include "keyboard_typing.h"
#include "online_files.h"
#include "online.h"
#include "online_net.h"
#include "graphics_page.h"
#include "p2p.h"
#include "mod_transfer.h"
#include "online_mods.h"
#include "script_input.h"
#include "frame_rate.h"
#include "util.h"

// The game's own Xbox LIVE invite path (it restarts itself to join): off - the
// restarted game says "a sign-in change has occurred" (see the notes below).
REXCVAR_DEFINE_BOOL(online_invite_popups, true, "Online",
                    "A pop-up when a friend invites you (the online overlay's INVITE POP-UPS row)");
REXCVAR_DEFINE_BOOL(online_relay_warning, true, "Online",
                    "A note in the lobby when an online match will go through the relay (how to connect directly); "
                    "off: never (its DON'T SHOW AGAIN)");
REXCVAR_DEFINE_BOOL(online_invite_native, false, "Online",
                    "Invites accepted through the game's own Xbox LIVE path (restarts the game; experimental)");

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kTitleId = 0x5451085D;                   // (the game's)
constexpr uint32_t kLiveInviteAccepted = 0x02000002;        // XN_LIVE_INVITE_ACCEPTED (data: the user index)
constexpr uint32_t kSystemUi = 0x00000009;                  // XN_SYS_UI (data: shown)


std::string Base64(const uint8_t* d, size_t n) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < n ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    out += k[v >> 18 & 63], out += k[v >> 12 & 63];
    out += i + 1 < n ? k[v >> 6 & 63] : '=';
    out += i + 2 < n ? k[v & 63] : '=';
  }
  return out;
}

std::vector<uint8_t> Unbase64(const std::string& s) {
  std::vector<uint8_t> out;
  uint32_t v = 0;
  int bits = 0;
  for (char c : s) {
    int x = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
          : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (x < 0) continue;
    v = v << 6 | uint32_t(x), bits += 6;
    if (bits >= 8) out.push_back(uint8_t(v >> (bits - 8))), bits -= 8;
  }
  return out;
}

void Put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
void Put64(uint8_t* p, uint64_t v) {
  Put32(p, uint32_t(v >> 32)), Put32(p + 4, uint32_t(v));
}

using svr2011::Upper;

// -- the server's list, polled in the background ------------------------------------

struct Friend {
  std::string name, status;  // status: playing / online / offline
  bool mutual = false;
};
struct Invite {
  std::string from, xuid, session, kind;
};
struct State {
  bool signed_in = false, answered = false;
  std::string error;
  std::vector<Friend> friends;
  std::vector<std::string> added_you;
  std::vector<Invite> invites;
};

rex::input::InputSystem* g_input = nullptr;
std::filesystem::path g_relaunch_file;  // UserData\relaunch.bin
rex::system::KernelState* g_kernel = nullptr;
ImFont* g_menu_font = nullptr;
ImFont* g_title_font = nullptr;

std::mutex g_mutex;
State g_state;
std::string g_notice, g_notice2;  // (OnlineOverlayNotice: picked up by the toast, under g_mutex)
int g_notice_secs = 0;
std::string g_message;  // the last action's outcome (shown on the overlay)
Clock::time_point g_message_at{};
std::set<std::string> g_invited;  // friends invited to the current session (its id + name)
std::condition_variable g_poll;
bool g_poll_now = false;

std::atomic<bool> g_toggle{false}, g_open{false}, g_wait_release{false};
std::atomic<uint16_t> g_guest_pad{0};  // (OnlineOverlayPad)
std::atomic<bool> g_relay_dialog{false};  // the relay warning is up (it holds the input)
std::atomic<bool> g_mods_dialog{false};   // the host's mods question is up (it holds the input too)
bool LobbyShown();  // (below: a session's lobby is on screen - the join done)
void RestartAndRejoin();  // (below: the mods question's RESTART AND REJOIN)
void ResumeRejoin();      // (below: a copy started by it - back to the session)
std::atomic<int> g_leave_yes{0};  // (the mods question's DON'T JOIN / LEAVE: then the lobby's "Leave this session?" YES)
std::atomic<int> g_press_b{0};            // its BACK: the game reads B for these many more pad reads

// Touches while it's open (a phone): taps, as fractions of the window, for the
// next frame; every touch is the overlay's then (not the on-screen pad's).
rex::ui::Window* g_window = nullptr;
std::mutex g_tap_mutex;
std::vector<ImVec2> g_taps;
struct TouchDown {
  float x, y;
};
std::map<uint32_t, TouchDown> g_downs;

class OverlayTouch final : public rex::ui::WindowInputListener {
 public:
  void OnTouchEvent(rex::ui::TouchEvent& e) override {
    using A = rex::ui::TouchEvent::Action;
    if (!g_open || !g_window) return;
    const float w = float(g_window->GetActualPhysicalWidth()), h = float(g_window->GetActualPhysicalHeight());
    if (w <= 0 || h <= 0) return;
    const float x = e.x() / w, y = e.y() / h;
    std::lock_guard lock(g_tap_mutex);
    if (e.action() == A::kDown) {
      g_downs[e.pointer_id()] = {x, y};
    } else if (e.action() == A::kUp) {
      auto it = g_downs.find(e.pointer_id());
      // (a tap: lifted near where it went down)
      if (it != g_downs.end() && std::abs(it->second.x - x) < 0.04f && std::abs(it->second.y - y) < 0.05f)
        g_taps.push_back(ImVec2(x, y));
      g_downs.erase(e.pointer_id());
    } else if (e.action() == A::kCancel) {
      g_downs.erase(e.pointer_id());
    }
    e.set_handled(true);
  }
};
OverlayTouch g_touch;
std::atomic<bool> g_from_game{false};  // opened by the game's INVITE FRIENDS (XN_SYS_UI sent)

// An invite accepted: what the game's XInviteGetAcceptedInfo gets.
struct Accepted {
  uint8_t info[60] = {};
  uint64_t inviter = 0;
  Clock::time_point at{};
};
std::optional<Accepted> g_accepted;
Clock::time_point g_relaunched_at{};  // (this copy: started by an invite's relaunch)

void Say(const std::string& m) {
  std::lock_guard lock(g_mutex);
  g_message = m;
  g_message_at = Clock::now();
}

bool SignedIn() {
  return rex::cvar::Query<bool>("online_enabled") && !rex::cvar::Query<std::string>("online_token").empty();
}

void SayGoodbye();

void Poll() {
  if (!SignedIn()) {
    std::lock_guard lock(g_mutex);
    g_state = State{};
    return;
  }
  // (?here=1: this game runs, signed in - its friends can invite it from anywhere in the game (it walks there);
  // &mode=online: in online mode - its friends see it Online. Leaving online mode says bye: friends see it
  // in the game, not online)
  static bool was_online = false;
  const bool online = P2POnlineMode();
  if (was_online && !online) SayGoodbye();
  was_online = online;
  // (&mode=online: counted "in online mode" - the server counts only games that say so; &place=: where in
  // the game, the dashboard's - older servers ignore it)
  std::string kind;
  const std::string place = PresencePlace(&kind);
  auto r = net::ServerRequest("GET", std::string(online ? "/api/friends?here=1&mode=online" : "/api/friends?here=1") +
                                         "&place=" + place + (kind.empty() ? "" : "&kind=" + kind) + "&client=2");
  State s;
  s.signed_in = true;
  Json j;
  // (2.1.6 servers: an account without its re-activation code / a confirmed email is refused - 403 and why)
  if (r && r->status == 403 && JsonReader{r->body}.Value(j) && j["error"]) SetOnlineAccountBlock(j.Str("error"));
  if (r && r->status == 200 && JsonReader{r->body}.Value(j) && j["friends"]) {
    s.answered = true;
    SetOnlineAccountBlock("");
    if (const Json* c = j["config"]; c && (*c)["mods_online"]) SetOnlineModsAllowed((*c)["mods_online"]->b);
    if (const Json* c = j["config"]; c && (*c)["match_network"]) P2PSetServerNetwork((*c)["match_network"]->s);
    P2PSetServerNetcode([&] {  // (a server without it: lockstep)
      const Json* c = j["config"];
      return c && (*c)["netcode"] ? (*c)["netcode"]->s : std::string("lockstep");
    }());
    if (const Json* c = j["config"]; c && (*c)["news"]) SetOnlineNews((*c)["news"]->s);
    for (const Json& f : j["friends"]->items) s.friends.push_back({f.Str("name"), f.Str("status"),
                                                                   f["mutual"] && f["mutual"]->b});
    if (const Json* a = j["added_you"])
      for (const Json& n : a->items) s.added_you.push_back(n.s);
    if (const Json* a = j["invites"])
      for (const Json& v : a->items) s.invites.push_back({v.Str("from"), v.Str("xuid"), v.Str("session"), v.Str("kind")});
  } else {
    OnlineServerUnanswered();  // (mods_online unknown: online matches with mods stay off - online_files.h)
    if (!r) P2PSetServerNetwork("auto"), P2PSetServerNetcode("lockstep");  // (no server: today's way)
    s.error = !r ? "THE SERVER DIDN'T ANSWER" : r->status == 404 ? "THIS SERVER HAS NO FRIENDS LIST YET"
                                                                 : "THE SERVER REFUSED (" + std::to_string(r->status) + ")";
  }
  std::lock_guard lock(g_mutex);
  g_state = std::move(s);
}

void PollThread() {
  for (;;) {
    Poll();
    std::unique_lock lock(g_mutex);
    g_poll.wait_for(lock, g_open ? std::chrono::seconds(5) : std::chrono::seconds(20), [] { return g_poll_now; });
    g_poll_now = false;
  }
}

void SayGoodbye() {
  if (SignedIn()) net::ServerRequest("GET", "/api/friends?bye=1");
}

void PollSoon() {
  std::lock_guard lock(g_mutex);
  g_poll_now = true;
  g_poll.notify_all();
}

// -- actions (each on a thread of its own) -------------------------------------------

void SendInvite(std::string name, std::string status) {
  uint8_t info[60];
  uint32_t slots = 0;
  if (!P2PSessionInfo(info, &slots)) return Say("YOU'RE NOT IN A SESSION: CREATE OR JOIN ONE FIRST");
  const std::string key = Base64(info, 8) + name;
  const std::string kind = slots > 6 ? "Royal Rumble" : "Match";
  std::thread([name, status, key, kind, session = Base64(info, 60)] {
    auto r = net::ServerRequest("POST", "/api/invite",
                                "{\"to\": " + Quote(name) + ", \"session\": \"" + session + "\", \"kind\": " + Quote(kind) + "}",
                                {{"Content-Type", "application/json"}});
    if (r && r->status == 200) {
      {
        std::lock_guard lock(g_mutex);
        g_invited.insert(key);
      }
      REXLOG_INFO("online overlay: invited {} ({})", name, status);
      // (not in the game: the server keeps it a while - they see it if they start the game soon)
      Say("INVITE SENT TO " + Upper(name) + (status == "offline" ? " - THEY SEE IT WHEN THEY START THE GAME" : ""));
    } else {
      Json j;
      std::string error;
      if (r && JsonReader{r->body}.Value(j)) error = j.Str("error");
      REXLOG_WARN("online overlay: invite to {} refused: {}", name, r ? std::to_string(r->status) + " " + error : "no answer");
      Say(error.empty() ? "THE INVITE DIDN'T GO" : Upper(error));
    }
  }).detach();
}

void Decline(const std::string& from) {
  {
    std::lock_guard lock(g_mutex);
    auto& v = g_state.invites;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Invite& i) { return i.from == from; }), v.end());
  }
  std::thread([from] {
    net::ServerRequest("POST", "/api/invite", "{\"decline\": " + Quote(from) + "}", {{"Content-Type", "application/json"}});
  }).detach();
}

void RequestInviteWalk(bool rumble, const std::string& from);  // (below: the game walks there)

void Accept(const Invite& invite) {
  const auto info = Unbase64(invite.session);
  if (info.size() != 60) return Say("THAT INVITE IS BROKEN");
  // (mods on while the server has them off online: not into a session - the invite stays on the list)
  if (!ModsOnlineMessage().empty())
#ifdef __ANDROID__
    return Say("YOU HAVE MODS ENABLED - TURN THEM OFF IN THE APP'S MODS TAB TO PLAY ONLINE");
#else
    return Say("YOU HAVE MODS ENABLED - RELAUNCH WITH START WITHOUT MODS TO PLAY ONLINE");
#endif
  Decline(invite.from);  // (taken: off the list)
  if (!REXCVAR_GET(online_invite_native)) {
    // (the session first in the game's search, its private slots open to this
    // game; the game is walked there - from a menu now, else once at one)
    P2PExpectInvite(info.data());
    const bool rumble = invite.kind == "Royal Rumble";
    Say("TAKING YOU TO " + Upper(invite.from) + "'S SESSION (BACK: STOP)");
    RequestInviteWalk(rumble, invite.from);
    std::thread([info] { P2PReachHost(info.data()); }).detach();
    g_toggle = g_open.load();  // (closed: the game takes over)
    return;
  }
  Say("JOINING " + Upper(invite.from) + "'S SESSION...");
  std::thread([info, invite] {
    const bool there = P2PReachHost(info.data());
    {
      std::lock_guard lock(g_mutex);
      Accepted a;
      std::memcpy(a.info, info.data(), 60);
      a.inviter = std::strtoull(invite.xuid.c_str(), nullptr, 16);
      a.at = Clock::now();
      g_accepted = a;
    }
    if (!there) Say(Upper(invite.from) + " ISN'T ANSWERING - TRYING ANYWAY");
    REXLOG_INFO("online overlay: invite from {} accepted (host {})", invite.from, there ? "answered" : "silent");
    if (g_kernel) g_kernel->BroadcastNotification(kLiveInviteAccepted, 0);
  }).detach();
  g_toggle = g_open.load();  // (closed: the game takes over)
}

void AddFriend(const std::string& name) {
  std::thread([name] {
    auto r = net::ServerRequest("POST", "/api/friends", "{\"add\": " + Quote(name) + "}", {{"Content-Type", "application/json"}});
    Json j;
    if (r && r->status == 200) {
      Say(Upper(name) + " ADDED");
      PollSoon();
    } else if (r && JsonReader{r->body}.Value(j) && !j.Str("error").empty()) {
      Say(Upper(j.Str("error")));
    } else {
      Say("THE SERVER DIDN'T ANSWER");
    }
  }).detach();
}

// -- the overlay -----------------------------------------------------------------------

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text) {
  dl->AddText(font ? font : ImGui::GetFont(), size, at, colour, text);
}
ImVec2 TextSize(ImFont* font, float size, const char* text) {
  return (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

// -- The relay warning --------------------------------------------------------
// A match session that plays through the relay (p2p.h P2PRelayPath: direct failed, or the server's match
// network says relay) gets a note in its lobby - once per session, never in a match, never a block: CONTINUE,
// DON'T SHOW AGAIN (online_relay_warning off) or BACK (the lobby's B: its own EXIT SESSION). A local dialog
// only: nothing is sent, so the players stay in step.

struct RelayTexts {
  uint32_t language;
  const char *title, *lead, *forced, *side0, *side1, *fix, *side2;
  const char* buttons[3];
};
// (the relay warning's words, by user_language; ROUTER PORT (UPNP): the GRAPHICS page's row, in English there)
const RelayTexts kRelayTexts[] = {
    {1,
     "CONNECTED THROUGH THE RELAY",
     "You're connected through the relay server, so the match may lag.",
     " The online server sends every match through the relay right now. You can still play now.",
     " A direct connection didn't work for you or the other player.",
     " Your router blocks direct connections.",
     " For a direct connection, open UDP port {port} on your router (port forward) or turn on ROUTER PORT (UPNP) in OPTIONS > GRAPHICS, then rejoin. You can still play now.",
     " Your port is open: the other player's router blocks direct connections. They can open the game's UDP port on their router or turn on ROUTER PORT (UPNP). You can still play now.",
     {"CONTINUE", "DON'T SHOW AGAIN", "BACK"}},
    {4,
     "CONNEXION VIA LE RELAIS",
     "Vous \xC3\xAAtes connect\xC3\xA9 via le serveur relais, le match peut donc ralentir.",
     " Le serveur en ligne fait passer tous les matchs par le relais en ce moment. Vous pouvez quand m\xC3\xAAme jouer maintenant.",
     " La connexion directe n'a pas march\xC3\xA9, chez vous ou chez l'autre joueur.",
     " Votre routeur bloque les connexions directes.",
     " Pour une connexion directe, ouvrez le port UDP {port} sur votre routeur (redirection de port) ou activez ROUTER PORT (UPNP) dans OPTIONS > GRAPHISMES, puis rejoignez la session. Vous pouvez quand m\xC3\xAAme jouer maintenant.",
     " Votre port est ouvert : le routeur de l'autre joueur bloque les connexions directes. Il peut ouvrir le port UDP du jeu sur son routeur ou activer ROUTER PORT (UPNP). Vous pouvez quand m\xC3\xAAme jouer maintenant.",
     {"CONTINUER", "NE PLUS AFFICHER", "RETOUR"}},
    {3,
     "\xC3\x9C" "BER DEN RELAY VERBUNDEN",
     "Du bist \xC3\xBC" "ber den Relay-Server verbunden, daher kann das Match ruckeln.",
     " Der Online-Server leitet gerade alle Matches \xC3\xBC" "ber den Relay. Du kannst trotzdem jetzt spielen.",
     " Die direkte Verbindung hat bei dir oder beim anderen Spieler nicht geklappt.",
     " Dein Router blockiert direkte Verbindungen.",
     " F\xC3\xBCr eine direkte Verbindung \xC3\xB6" "ffne UDP-Port {port} an deinem Router (Portweiterleitung) oder schalte ROUTER PORT (UPNP) unter OPTIONEN > GRAFIK ein und tritt dann erneut bei. Du kannst trotzdem jetzt spielen.",
     " Dein Port ist offen: Der Router des anderen Spielers blockiert direkte Verbindungen. Er kann den UDP-Port des Spiels an seinem Router \xC3\xB6" "ffnen oder ROUTER PORT (UPNP) einschalten. Du kannst trotzdem jetzt spielen.",
     {"WEITER", "NICHT MEHR ZEIGEN", "ZUR\xC3\x9C" "CK"}},
    {5,
     "CONECTADO MEDIANTE EL REL\xC3\x89",
     "Est\xC3\xA1s conectado mediante el servidor de rel\xC3\xA9, as\xC3\xAD que la partida puede ir con retraso.",
     " El servidor en l\xC3\xADnea env\xC3\xAD" "a ahora todas las partidas por el rel\xC3\xA9. Puedes jugar igualmente.",
     " La conexi\xC3\xB3n directa no ha funcionado para ti o para el otro jugador.",
     " Tu router bloquea las conexiones directas.",
     " Para una conexi\xC3\xB3n directa, abre el puerto UDP {port} en tu router (redirecci\xC3\xB3n de puertos) o activa ROUTER PORT (UPNP) en OPCIONES > GR\xC3\x81" "FICOS y vuelve a unirte. Puedes jugar igualmente.",
     " Tu puerto est\xC3\xA1 abierto: el router del otro jugador bloquea las conexiones directas. Puede abrir el puerto UDP del juego en su router o activar ROUTER PORT (UPNP). Puedes jugar igualmente.",
     {"CONTINUAR", "NO VOLVER A MOSTRAR", "ATR\xC3\x81S"}},
    {6,
     "CONNESSO TRAMITE IL RELAY",
     "Sei connesso tramite il server relay, quindi la partita potrebbe avere lag.",
     " Il server online fa passare ora tutte le partite dal relay. Puoi giocare comunque.",
     " La connessione diretta non ha funzionato per te o per l'altro giocatore.",
     " Il tuo router blocca le connessioni dirette.",
     " Per una connessione diretta, apri la porta UDP {port} sul tuo router (port forwarding) o attiva ROUTER PORT (UPNP) in OPZIONI > GRAFICA, poi rientra. Puoi giocare comunque.",
     " La tua porta \xC3\xA8 aperta: il router dell'altro giocatore blocca le connessioni dirette. Pu\xC3\xB2 aprire la porta UDP del gioco sul suo router o attivare ROUTER PORT (UPNP). Puoi giocare comunque.",
     {"CONTINUA", "NON MOSTRARE PI\xC3\x99", "INDIETRO"}},
};

const RelayTexts& RelayWords() {
  const uint32_t language = rex::cvar::Query<uint32_t>("user_language");
  for (const RelayTexts& t : kRelayTexts)
    if (t.language == language) return t;
  return kRelayTexts[0];
}

std::string RelayBody(const P2PRelayPathInfo& info) {
  const RelayTexts& t = RelayWords();
  std::string s = t.lead;
  if (info.kind == P2PRelayPathInfo::kForced) return s + t.forced;
  if (info.side == 2) return s + t.side2;
  std::string fix = t.fix;
  if (const size_t at = fix.find("{port}"); at != std::string::npos) fix.replace(at, 6, std::to_string(info.port));
  return s + (info.side == 1 ? t.side1 : t.side0) + fix;
}

uint16_t PadButtons() {
  using namespace rex::input;
  X_INPUT_STATE s = {};
  if (!g_input || !g_input->HeldState(0, &s)) return 0;
  uint16_t b = s.gamepad.buttons;
  constexpr int kDead = 16000;
  if (s.gamepad.thumb_ly > kDead) b |= X_INPUT_GAMEPAD_DPAD_UP;
  if (s.gamepad.thumb_ly < -kDead) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
  return b;
}

class OnlineOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit OnlineOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum Kind { kInviteRow, kFriendRow, kAddedRow, kAddRow, kPopupRow };
  struct Row {
    Kind kind;
    int index;  // (in its list)
  };
  void Close();
  void Toast(ImGuiIO& io);
  bool RelayDialog(ImGuiIO& io);  // (true: up - nothing else this frame)
  // The host's mods this game lacks (mod_transfer.h): DOWNLOAD / DON'T JOIN; through the relay, after a failed
  // download: STAY / LEAVE; done: OK. (true: up)
  bool ModsDialog(ImGuiIO& io);
  void ModsProgress(ImGuiIO& io);  // (a download: bottom right)
  enum ModsMode { kModsAsk, kModsRelay, kModsFailed, kModsDone };

  int sel_ = 0, top_ = 0;
  std::string typed_;  // ADD FRIEND
  bool grid_ = false;  // ADD FRIEND's letter grid (a controller: Y)
  int gx_ = 0, gy_ = 0;
  uint16_t prev_buttons_ = 0;
  bool wait_release_ = false, combo_down_ = false;
  Clock::time_point toggled_at_{};  // (BACK + RB: one toggle per press)
  bool invite_mode_ = false;  // opened by the lobby's X INVITE FRIENDS: the friends only, mid-screen
  Clock::time_point opened_at_{}, repeat_at_{};
  std::set<std::string> seen_invites_;  // (the notice: once each)
  std::string toast_, toast_line2_;
  Clock::time_point toast_until_{};
  bool relay_open_ = false, relay_wait_ = false;
  int relay_sel_ = 0;
  uint16_t relay_prev_ = 0;
  uint64_t relay_warned_ = 0;  // (the session it was shown for: once each)
  std::string relay_body_;
  Clock::time_point relay_opened_at_{};
  bool mods_open_ = false, mods_wait_ = false;
  ModsMode mods_mode_ = kModsAsk;
  int mods_sel_ = 0;
  uint16_t mods_prev_ = 0;
  ModLobbyOffer mods_offer_;
  std::string mods_title_, mods_body_;
  Clock::time_point mods_opened_at_{};
  int mods_phase_ = 0;  // (the download's phase last seen: its end opens the dialog again)
};

void OnlineOverlay::Close() {
  g_open = false;
  grid_ = false;
  SetOverlayTyping(false);
  g_wait_release = true;
  if (g_from_game.exchange(false) && g_kernel) g_kernel->BroadcastNotification(kSystemUi, 0);
}

void OnlineOverlay::Toast(ImGuiIO& io) {
  State s;
  {
    std::lock_guard lock(g_mutex);
    s.invites = g_state.invites;
  }
  // A new invite: "<NAME> INVITED YOU" and how to answer, for 10 s at the top
  // of the screen (INVITE POP-UPS on the overlay turns it off).
  const bool popups = rex::cvar::Query<bool>("online_invite_popups");
  for (const Invite& i : s.invites) {
    if (!seen_invites_.insert(i.from + i.session).second) continue;
    REXLOG_INFO("online overlay: invite from {} ({}){}", i.from, i.kind, popups ? "" : " - pop-ups off: on the list only");
    if (!popups) continue;
    toast_ = Upper(i.from) + " INVITED YOU";
    toast_line2_ = "TO A " + Upper(i.kind.empty() ? "match" : i.kind) + "  -  " +
                   Upper(rex::cvar::Query<std::string>("bind_online_menu")) + " OR BACK + RB TO ANSWER";
    toast_until_ = Clock::now() + std::chrono::seconds(10);
  }
  {
    std::lock_guard lock(g_mutex);
    if (!g_notice.empty()) {
      toast_ = std::move(g_notice), toast_line2_ = std::move(g_notice2);
      toast_until_ = Clock::now() + std::chrono::seconds(g_notice_secs);
      g_notice.clear(), g_notice2.clear();
    }
  }
  if (g_open || Clock::now() > toast_until_ || toast_.empty()) return;
  const float sc = std::min(io.DisplaySize.x / 1280.0f, io.DisplaySize.y / 720.0f), size = 24 * sc, size2 = 18 * sc;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 sz = TextSize(g_menu_font, size, toast_.c_str()), sz2 = TextSize(g_menu_font, size2, toast_line2_.c_str());
  const float w = std::max(sz.x, sz2.x) + 48 * sc, h = sz.y + sz2.y + 34 * sc;
  const ImVec2 a((io.DisplaySize.x - w) * 0.5f, 24 * sc), b(a.x + w, a.y + h);
  dl->AddRectFilled(a, b, IM_COL32(8, 8, 12, 240), 8 * sc);
  dl->AddRectFilled(a, ImVec2(a.x + 8 * sc, b.y), IM_COL32(200, 30, 32, 255), 8 * sc);
  dl->AddRect(a, b, IM_COL32(235, 235, 240, 255), 8 * sc, 0, 2 * sc);
  Text(dl, g_menu_font, size, ImVec2((a.x + b.x - sz.x) * 0.5f, a.y + 12 * sc), IM_COL32(255, 255, 255, 255), toast_.c_str());
  Text(dl, g_menu_font, size2, ImVec2((a.x + b.x - sz2.x) * 0.5f, a.y + 18 * sc + sz.y), IM_COL32(255, 200, 60, 255),
       toast_line2_.c_str());
}

bool OnlineOverlay::RelayDialog(ImGuiIO& io) {
  using namespace rex::input;
  if (!relay_open_) {
    if (g_open || !REXCVAR_GET(online_relay_warning)) return false;
    const P2PRelayPathInfo info = P2PRelayPath();
    if ((info.kind != P2PRelayPathInfo::kRelayed && info.kind != P2PRelayPathInfo::kForced) || !info.session ||
        info.session == relay_warned_)
      return false;
    relay_warned_ = info.session;
    relay_body_ = RelayBody(info);
    relay_sel_ = 0;
    relay_open_ = true;
    g_relay_dialog = true;
    relay_wait_ = true;
    relay_opened_at_ = Clock::now();
    REXLOG_INFO("online overlay: relay warning shown (session {:016X}, {}, side {}, port {})", info.session,
                info.kind == P2PRelayPathInfo::kForced ? (info.forced_by_server ? "the server's relay" : "forced")
                                                       : "direct failed",
                info.side, info.port);
  }
  const RelayTexts& t = RelayWords();
  const float sc = std::min(io.DisplaySize.x / 1280.0f, io.DisplaySize.y / 720.0f);
  const float title_size = 30 * sc, size = 21 * sc, pad = 28 * sc, w = 820 * sc;
  ImFont* font = g_menu_font;
  ImFont* tfont = g_title_font ? g_title_font : g_menu_font;
  const float wrap = w - 2 * pad;
  const ImVec2 body = (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, wrap, relay_body_.c_str());
  const float button_h = 44 * sc, h = pad + title_size + 18 * sc + body.y + 26 * sc + button_h + pad;
  const ImVec2 a((io.DisplaySize.x - w) * 0.5f, (io.DisplaySize.y - h) * 0.5f), b(a.x + w, a.y + h);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 140));
  dl->AddRectFilled(a, b, IM_COL32(10, 10, 14, 245), 8 * sc);
  dl->AddRectFilled(a, ImVec2(a.x + 8 * sc, b.y), IM_COL32(230, 160, 30, 255), 8 * sc);
  dl->AddRect(a, b, IM_COL32(235, 235, 240, 255), 8 * sc, 0, 2 * sc);
  Text(dl, tfont, title_size, ImVec2(a.x + pad, a.y + pad), IM_COL32(255, 255, 255, 255), t.title);
  dl->AddText(font ? font : ImGui::GetFont(), size, ImVec2(a.x + pad, a.y + pad + title_size + 18 * sc),
              IM_COL32(225, 225, 230, 255), relay_body_.c_str(), nullptr, wrap);
  // the buttons, in a row (CONTINUE first: the default)
  float bw[3], total = 0;
  for (int k = 0; k < 3; ++k) total += bw[k] = TextSize(font, size, t.buttons[k]).x + 40 * sc;
  total += 2 * 16 * sc;
  float x = b.x - pad - total;
  const float y = b.y - pad - button_h;
  int hovered = -1;
  ImVec2 box[3][2];
  for (int k = 0; k < 3; ++k) {
    box[k][0] = ImVec2(x, y), box[k][1] = ImVec2(x + bw[k], y + button_h);
    if (io.MousePos.x >= x && io.MousePos.x < x + bw[k] && io.MousePos.y >= y && io.MousePos.y < y + button_h) hovered = k;
    x += bw[k] + 16 * sc;
  }
  // input: LEFT / RIGHT (or UP / DOWN) move, A chooses, B is BACK, START is CONTINUE; the mouse
  const uint16_t buttons = PadButtons();
  if (relay_wait_) {  // (a button still down from the lobby: not a choice)
    if (buttons == 0 && Clock::now() - relay_opened_at_ > std::chrono::milliseconds(300)) relay_wait_ = false;
    relay_prev_ = buttons;
  }
  const uint16_t pressed = relay_wait_ ? 0 : uint16_t(buttons & ~relay_prev_);
  relay_prev_ = buttons;
  if (pressed & (X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_UP)) relay_sel_ = (relay_sel_ + 2) % 3;
  if (pressed & (X_INPUT_GAMEPAD_DPAD_RIGHT | X_INPUT_GAMEPAD_DPAD_DOWN)) relay_sel_ = (relay_sel_ + 1) % 3;
  if (hovered >= 0 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) relay_sel_ = hovered;
  int chosen = -1;
  if (pressed & X_INPUT_GAMEPAD_A) chosen = relay_sel_;
  if (pressed & X_INPUT_GAMEPAD_START) chosen = 0;
  if (pressed & X_INPUT_GAMEPAD_B) chosen = 2;
  if (hovered >= 0 && io.MouseClicked[0] && !relay_wait_) chosen = hovered;
  for (int k = 0; k < 3; ++k) {
    const bool on = k == relay_sel_;
    dl->AddRectFilled(box[k][0], box[k][1], on ? IM_COL32(200, 30, 32, 255) : IM_COL32(40, 40, 48, 255), 6 * sc);
    dl->AddRect(box[k][0], box[k][1], IM_COL32(235, 235, 240, on ? 255 : 120), 6 * sc, 0, 1.5f * sc);
    const ImVec2 ts = TextSize(font, size, t.buttons[k]);
    Text(dl, font, size, ImVec2((box[k][0].x + box[k][1].x - ts.x) * 0.5f, (box[k][0].y + box[k][1].y - ts.y) * 0.5f),
         IM_COL32(255, 255, 255, 255), t.buttons[k]);
  }
  if (chosen >= 0) {
    static const char* const kNames[] = {"continue", "don't show again", "back"};
    REXLOG_INFO("online overlay: relay warning - {}", kNames[chosen]);
    if (chosen == 1) {
      rex::cvar::SetFlagByName("online_relay_warning", "false");
      SaveConfigSetting("online_relay_warning", "false");
    }
    if (chosen == 2) g_press_b = 8;  // (the lobby's B, a few frames: its EXIT SESSION question)
    relay_open_ = false;
    g_relay_dialog = false;
    g_wait_release = true;  // (the choosing button: not to the game too)
  }
  return true;
}

namespace {
std::string Megabytes(uint64_t n) {
  char b[32];
  std::snprintf(b, sizeof b, n < (10ull << 20) ? "%.1f MB" : "%.0f MB", n / 1048576.0);
  return b;
}
// "A, B, C +2" (a line's worth)
std::string NameList(const std::vector<std::string>& v) {
  std::string s;
  size_t k = 0;
  for (; k < v.size(); ++k) {
    if (s.size() + v[k].size() > 90 && k) break;
    s += (k ? ", " : "") + v[k];
  }
  if (k < v.size()) s += " +" + std::to_string(v.size() - k);
  return s;
}
}  // namespace

bool OnlineOverlay::ModsDialog(ImGuiIO& io) {
  using namespace rex::input;
  auto open = [&](ModsMode mode, std::string title, std::string body) {
    mods_mode_ = mode, mods_title_ = std::move(title), mods_body_ = std::move(body);
    mods_sel_ = 0, mods_open_ = true, mods_wait_ = true, mods_opened_at_ = Clock::now();
    g_mods_dialog = true;
    REXLOG_INFO("online overlay: mods question ({}): {}", int(mode), mods_title_);
  };
  if (!mods_open_) {
    // (a download that ended: done or failed - said once)
    const ModDownloadState d = ModDownload();
    const int phase = int(d.phase);
    if (phase != mods_phase_) {
      const bool was_running = mods_phase_ == ModDownloadState::kAsking || mods_phase_ == ModDownloadState::kFetching;
      mods_phase_ = phase;
      if (was_running && d.phase == ModDownloadState::kDone)
        open(kModsDone, "THE MODS ARE DOWNLOADED",
             std::to_string(d.mods) + (d.mods == 1 ? " mod is" : " mods are") + " on this PC for this session (" +
                 Megabytes(d.total) + "). The game loads them when it starts again: RESTART AND REJOIN starts it "
                 "again and takes you back to this session. They're removed when the game closes.");
      else if (was_running && d.phase == ModDownloadState::kFailed && d.error != "Cancelled.")
        open(kModsFailed, "THE MODS DIDN'T DOWNLOAD", d.error + "\n\nYou can stay in the session, but you can't "
                                                     "play with the mods you don't have.");
    }
    ModLobbyOffer offer;
    // (only once the lobby is up: the question holds the pad, and the game's join needs it until then)
    if (!mods_open_ && !g_open && LobbyShown() && TakeModLobbyOffer(&offer)) {
      mods_offer_ = offer;
      if (offer.relayed) {
        open(kModsRelay, "THIS MATCH USES MODS YOU DON'T HAVE",
             "You don't have: " + NameList(offer.lacking) + ".\n\nMods only come straight from the host's game, and "
             "your connection to the host goes through the relay, so they can't be downloaded. You can stay, but "
             "you can't play with them.");
      } else if (offer.mods.empty()) {
        open(kModsRelay, "THIS MATCH USES MODS THE HOST CAN'T SEND",
             NameList(offer.unsendable) + "\n\nOnly the game's and mods' own kinds of files travel. You can stay, "
             "but you can't play with them.");
      } else {
        std::vector<std::string> names;
        for (const ModOffer& m : offer.mods) names.push_back(m.name.empty() ? m.id : m.name);
        const std::string cant = offer.unsendable.empty() ? std::string()
                                                          : "\n\nThe host can't send: " + NameList(offer.unsendable) + ".";
        open(kModsAsk,
             "THIS MATCH USES " + std::to_string(offer.mods.size()) + (offer.mods.size() == 1 ? " MOD" : " MODS") +
                 " YOU DON'T HAVE (" + Megabytes(offer.bytes) + ")",
             NameList(names) + "\n\nDownload " + (offer.mods.size() == 1 ? "it" : "them") +
                 " from the host? They come straight from the host's game and are removed when you leave "
                 "online. If you don't, you leave this session." + cant);
      }
    }
    if (!mods_open_) return false;
  }
  static const char* const kAsk[] = {"DOWNLOAD", "DON'T JOIN"};
  static const char* const kStay[] = {"STAY", "LEAVE"};
  static const char* const kDone[] = {"RESTART AND REJOIN", "LATER"};
  const char* const* labels = mods_mode_ == kModsAsk ? kAsk : mods_mode_ == kModsDone ? kDone : kStay;
  const int count = 2;
  const float sc = std::min(io.DisplaySize.x / 1280.0f, io.DisplaySize.y / 720.0f);
  const float title_size = 28 * sc, size = 21 * sc, pad = 28 * sc, w = 820 * sc;
  ImFont* font = g_menu_font;
  ImFont* tfont = g_title_font ? g_title_font : g_menu_font;
  const float wrap = w - 2 * pad;
  const ImVec2 body = (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, wrap, mods_body_.c_str());
  const ImVec2 tsz = (tfont ? tfont : ImGui::GetFont())->CalcTextSizeA(title_size, FLT_MAX, wrap, mods_title_.c_str());
  const float button_h = 44 * sc, h = pad + tsz.y + 18 * sc + body.y + 26 * sc + button_h + pad;
  const ImVec2 a((io.DisplaySize.x - w) * 0.5f, (io.DisplaySize.y - h) * 0.5f), b(a.x + w, a.y + h);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 140));
  dl->AddRectFilled(a, b, IM_COL32(10, 10, 14, 245), 8 * sc);
  dl->AddRectFilled(a, ImVec2(a.x + 8 * sc, b.y), mods_mode_ == kModsAsk || mods_mode_ == kModsDone
                                                      ? IM_COL32(200, 30, 32, 255) : IM_COL32(230, 160, 30, 255), 8 * sc);
  dl->AddRect(a, b, IM_COL32(235, 235, 240, 255), 8 * sc, 0, 2 * sc);
  dl->AddText(tfont ? tfont : ImGui::GetFont(), title_size, ImVec2(a.x + pad, a.y + pad), IM_COL32(255, 255, 255, 255),
              mods_title_.c_str(), nullptr, wrap);
  dl->AddText(font ? font : ImGui::GetFont(), size, ImVec2(a.x + pad, a.y + pad + tsz.y + 18 * sc),
              IM_COL32(225, 225, 230, 255), mods_body_.c_str(), nullptr, wrap);
  float bw[2], total = 0;
  for (int k = 0; k < count; ++k) total += bw[k] = TextSize(font, size, labels[k]).x + 40 * sc;
  total += (count - 1) * 16 * sc;
  float x = b.x - pad - total;
  const float y = b.y - pad - button_h;
  int hovered = -1;
  ImVec2 box[2][2];
  for (int k = 0; k < count; ++k) {
    box[k][0] = ImVec2(x, y), box[k][1] = ImVec2(x + bw[k], y + button_h);
    if (io.MousePos.x >= x && io.MousePos.x < x + bw[k] && io.MousePos.y >= y && io.MousePos.y < y + button_h) hovered = k;
    x += bw[k] + 16 * sc;
  }
  // input: LEFT / RIGHT move, A chooses, B is the second (DON'T JOIN / LEAVE; OK alone: OK); the mouse
  const uint16_t buttons = PadButtons();
  if (mods_wait_) {  // (a button still down from the lobby: not a choice)
    if (buttons == 0 && Clock::now() - mods_opened_at_ > std::chrono::milliseconds(300)) mods_wait_ = false;
    mods_prev_ = buttons;
  }
  const uint16_t pressed = mods_wait_ ? 0 : uint16_t(buttons & ~mods_prev_);
  mods_prev_ = buttons;
  if (pressed & (X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_RIGHT |
                 X_INPUT_GAMEPAD_DPAD_DOWN))
    mods_sel_ = (mods_sel_ + 1) % count;
  if (hovered >= 0 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) mods_sel_ = hovered;
  int chosen = -1;
  if (pressed & X_INPUT_GAMEPAD_A) chosen = mods_sel_;
  if (pressed & X_INPUT_GAMEPAD_B) chosen = count - 1;
  if (hovered >= 0 && io.MouseClicked[0] && !mods_wait_) chosen = hovered;
  for (int k = 0; k < count; ++k) {
    const bool on = k == mods_sel_;
    dl->AddRectFilled(box[k][0], box[k][1], on ? IM_COL32(200, 30, 32, 255) : IM_COL32(40, 40, 48, 255), 6 * sc);
    dl->AddRect(box[k][0], box[k][1], IM_COL32(235, 235, 240, on ? 255 : 120), 6 * sc, 0, 1.5f * sc);
    const ImVec2 ts = TextSize(font, size, labels[k]);
    Text(dl, font, size, ImVec2((box[k][0].x + box[k][1].x - ts.x) * 0.5f, (box[k][0].y + box[k][1].y - ts.y) * 0.5f),
         IM_COL32(255, 255, 255, 255), labels[k]);
  }
  if (chosen >= 0) {
    REXLOG_INFO("online overlay: mods question - {}", labels[chosen]);
    if (mods_mode_ == kModsAsk) AnswerModLobbyOffer(mods_offer_, chosen == 0);
    // (DON'T JOIN / LEAVE: the lobby's B, a few frames - its EXIT SESSION question)
    if (mods_mode_ != kModsDone && chosen == 1) g_press_b = 8, g_leave_yes = 1;
    if (mods_mode_ == kModsDone && chosen == 0) RestartAndRejoin();
    mods_open_ = false;
    g_mods_dialog = false;
    g_wait_release = true;  // (the choosing button: not to the game too)
  }
  return true;
}

void OnlineOverlay::ModsProgress(ImGuiIO& io) {
  const ModDownloadState d = ModDownload();
  if (d.phase != ModDownloadState::kAsking && d.phase != ModDownloadState::kFetching) return;
  const float sc = std::min(io.DisplaySize.x / 1280.0f, io.DisplaySize.y / 720.0f);
  const float size = 18 * sc, size2 = 15 * sc, pad = 14 * sc, w = 420 * sc, bar_h = 10 * sc;
  std::string line1 = d.phase == ModDownloadState::kAsking ? "ASKING THE HOST FOR ITS MODS..."
                                                           : "DOWNLOADING MODS (" + std::to_string(d.mod) + " OF " +
                                                                 std::to_string(d.mods) + ")";
  char line2[96];
  std::snprintf(line2, sizeof line2, "%s OF %s  -  %.1f MB/S", Megabytes(d.done).c_str(), Megabytes(d.total).c_str(),
                d.rate / 1048576.0);
  const float h = pad + size + 6 * sc + size2 + 10 * sc + bar_h + pad;
  const ImVec2 b(io.DisplaySize.x - 24 * sc, io.DisplaySize.y - 24 * sc), a(b.x - w, b.y - h);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(a, b, IM_COL32(8, 8, 12, 230), 8 * sc);
  dl->AddRectFilled(a, ImVec2(a.x + 6 * sc, b.y), IM_COL32(200, 30, 32, 255), 8 * sc);
  dl->AddRect(a, b, IM_COL32(235, 235, 240, 200), 8 * sc, 0, 1.5f * sc);
  Text(dl, g_menu_font, size, ImVec2(a.x + pad + 6 * sc, a.y + pad), IM_COL32(255, 255, 255, 255), line1.c_str());
  Text(dl, g_menu_font, size2, ImVec2(a.x + pad + 6 * sc, a.y + pad + size + 6 * sc), IM_COL32(255, 200, 60, 255), line2);
  const ImVec2 ba(a.x + pad + 6 * sc, b.y - pad - bar_h), bb(b.x - pad, b.y - pad);
  dl->AddRectFilled(ba, bb, IM_COL32(40, 40, 48, 255), 4 * sc);
  const float f = d.total ? std::min(1.0f, float(double(d.done) / double(d.total))) : 0.0f;
  if (f > 0) dl->AddRectFilled(ba, ImVec2(ba.x + (bb.x - ba.x) * f, bb.y), IM_COL32(200, 30, 32, 255), 4 * sc);
}

void OnlineOverlay::OnDraw(ImGuiIO& io) {
  using namespace rex::input;
  // BACK + RB opens / closes it (from any screen; closed, the pad as the game
  // reads it - HeldState is only kept while a page holds the pad).
  {
    X_INPUT_STATE st = {};
    bool known = false;  // (just opened: no held state yet - the combo is still down)
    if (OnlineOverlayHoldsInput()) {
      known = g_input && g_input->HeldState(0, &st);
    } else {
      st.gamepad.buttons = g_guest_pad.load();
      known = true;
    }
    if (known) {
      const uint16_t b = st.gamepad.buttons;
      const bool combo = (b & X_INPUT_GAMEPAD_BACK) && (b & X_INPUT_GAMEPAD_RIGHT_SHOULDER);
      // (one press: the held state read just after opening can miss the
      // combo for a frame - it then seemed pressed again, and closed it)
      if (combo && !combo_down_ && Clock::now() - toggled_at_ > std::chrono::milliseconds(500)) g_toggle = true;
      combo_down_ = combo;
    }
  }
  if (g_toggle.exchange(false)) {
    REXLOG_INFO("online overlay: {}", g_open ? "closed" : "opened");
    toggled_at_ = Clock::now();
    if (g_open) {
      Close();
    } else {
      g_open = true;
      invite_mode_ = g_from_game.load();
      wait_release_ = true;
      opened_at_ = Clock::now();
      sel_ = top_ = 0;
      PollSoon();
    }
  }
  Toast(io);
  if (RelayDialog(io)) return;
  ModsProgress(io);
  if (ModsDialog(io)) return;
  if (!g_open) {
    if (g_wait_release && PadButtons() == 0) g_wait_release = false;
    return;
  }

  State st;
  std::string message;
  bool fresh_message;
  std::set<std::string> invited;
  {
    std::lock_guard lock(g_mutex);
    st = g_state;
    message = g_message;
    fresh_message = Clock::now() - g_message_at < std::chrono::seconds(15);
    invited = g_invited;
  }
  uint8_t info[60];
  uint32_t slots = 0;
  const bool in_session = P2PSessionInfo(info, &slots);
  const std::string session_key = in_session ? Base64(info, 8) : std::string();

  // (invite mode - the lobby's X INVITE FRIENDS: the friends to invite only,
  // the ones online first)
  std::vector<Row> rows;
  if (invite_mode_) {
    for (int pass = 0; pass < 2; ++pass)
      for (int i = 0; i < int(st.friends.size()); ++i)
        if ((st.friends[i].status != "offline") == (pass == 0)) rows.push_back({kFriendRow, i});
  } else {
    for (int i = 0; i < int(st.invites.size()); ++i) rows.push_back({kInviteRow, i});
    for (int i = 0; i < int(st.friends.size()); ++i) rows.push_back({kFriendRow, i});
    for (int i = 0; i < int(st.added_you.size()); ++i) rows.push_back({kAddedRow, i});
    if (st.signed_in) rows.push_back({kAddRow, 0});
    if (st.signed_in) rows.push_back({kPopupRow, 0});
  }
  const int n = int(rows.size());

  // Input: controller (edges, repeat for held directions), keys, the mouse.
  const uint16_t buttons = PadButtons();
  const auto now = Clock::now();
  if (wait_release_) {
    if (buttons == 0 && now - opened_at_ > std::chrono::milliseconds(150)) wait_release_ = false;
    prev_buttons_ = buttons;
  }
  const uint16_t pressed = wait_release_ ? 0 : uint16_t(buttons & ~prev_buttons_);
  constexpr uint16_t kDirs = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN;
  uint16_t act = pressed;
  if (pressed & kDirs) {
    repeat_at_ = now + std::chrono::milliseconds(400);
  } else if (!wait_release_ && (buttons & kDirs) && now >= repeat_at_) {
    act |= buttons & kDirs;
    repeat_at_ = now + std::chrono::milliseconds(120);
  }
  if (!wait_release_) prev_buttons_ = buttons;
  auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
  int move = 0;
  if ((act & X_INPUT_GAMEPAD_DPAD_UP) || key(ImGuiKey_UpArrow)) move = -1;
  if ((act & X_INPUT_GAMEPAD_DPAD_DOWN) || key(ImGuiKey_DownArrow)) move = 1;
  bool choose = (pressed & X_INPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
  bool other = (pressed & X_INPUT_GAMEPAD_X) != 0;  // (decline)
  bool back = (pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_START)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  // ADD FRIEND with a controller: Y opens a letter grid (the D-pad moves, A
  // types, X deletes, B closes it; OK adds the friend).
  static const char* const kGrid[4] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789_-. "};
  const bool on_add = n && sel_ < n && rows[sel_].kind == kAddRow;
  if (grid_ && on_add) {
    const int cols = gy_ < 4 ? 10 : 2;
    if (pressed & X_INPUT_GAMEPAD_DPAD_LEFT) gx_ = (gx_ + cols - 1) % cols;
    if (pressed & X_INPUT_GAMEPAD_DPAD_RIGHT) gx_ = (gx_ + 1) % cols;
    if (move) gy_ = (gy_ + move + 5) % 5, gx_ = std::min(gx_, gy_ < 4 ? 9 : 1);
    bool ok = false;
    if (pressed & X_INPUT_GAMEPAD_A) {
      if (gy_ < 4) {
        if (typed_.size() < 15) typed_ += kGrid[gy_][gx_];
      } else if (gx_ == 0) {
        if (!typed_.empty()) typed_.pop_back();
      } else {
        ok = true, grid_ = false;
      }
    }
    if ((pressed & X_INPUT_GAMEPAD_X) && !typed_.empty()) typed_.pop_back();
    if (pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_Y)) grid_ = false;
    move = 0, other = false, back = false;
    choose = ok;
  } else {
    grid_ = false;
    if (on_add && (pressed & X_INPUT_GAMEPAD_Y)) grid_ = true, gx_ = gy_ = 0;
  }
  if (n) sel_ = (sel_ + move + n) % n;
  sel_ = std::clamp(sel_, 0, std::max(0, n - 1));
  // ADD FRIEND: typing (the keyboard)
  const bool typing = n && rows[sel_].kind == kAddRow;
  SetOverlayTyping(typing);  // (the window's text input; a phone's keyboard: keyboard_typing.h)
  if (typing) {
    for (ImWchar c : io.InputQueueCharacters) {
      if (typed_.size() < 15 && c < 128 && (std::isalnum(int(c)) || c == ' ' || c == '_' || c == '.' || c == '-'))
        typed_ += char(c);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true) && !typed_.empty()) typed_.pop_back();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !typing) other = true;

  // Layout in the game's 1280 x 720 frame, letterboxed: a panel at the right.
  float fw = io.DisplaySize.x, fh = fw * 9.0f / 16.0f;
  if (fh > io.DisplaySize.y) fh = io.DisplaySize.y, fw = fh * 16.0f / 9.0f;
  const float ox = (io.DisplaySize.x - fw) * 0.5f, oy = (io.DisplaySize.y - fh) * 0.5f, s = fh / 720.0f;
  auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };
  const ImU32 kWhite = IM_COL32(255, 255, 255, 255), kGrey = IM_COL32(170, 170, 178, 255),
              kDim = IM_COL32(120, 120, 128, 255), kGreen = IM_COL32(110, 210, 90, 255),
              kGold = IM_COL32(255, 200, 60, 255), kRed = IM_COL32(200, 30, 32, 255);
  // (the full overlay at the right; invite mode a smaller panel mid-screen)
  const float L = invite_mode_ ? 330 : 690, R = invite_mode_ ? 950 : 1250, T = invite_mode_ ? 120 : 30,
              B = invite_mode_ ? 600 : 690, kListTop = T + 120;
  constexpr float kRowH = 52;
  const int visible = int((B - 90 - kListTop) / kRowH);
  if (sel_ < top_) top_ = sel_;
  if (sel_ >= top_ + visible) top_ = sel_ - visible + 1;
  top_ = std::clamp(top_, 0, std::max(0, n - visible));

  // The mouse: a row under it is the selection; a click chooses it.
  const ImVec2 mouse = io.MousePos;
  for (int k = 0; k < visible && top_ + k < n; ++k) {
    const ImVec2 a = P(L + 16, kListTop + k * kRowH), b = P(R - 16, kListTop + k * kRowH + kRowH - 6);
    if (mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) {
      if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) sel_ = top_ + k;
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) choose = true;
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) other = true;
    }
  }

  // Taps (a phone): a row is chosen; outside the panel, it closes.
  bool tapped_out = false;
  {
    std::vector<ImVec2> taps;
    {
      std::lock_guard lock(g_tap_mutex);
      taps.swap(g_taps);
    }
    for (const ImVec2& t : taps) {
      const ImVec2 at(t.x * io.DisplaySize.x, t.y * io.DisplaySize.y);
      const ImVec2 pa = P(L, T), pb = P(R, B);
      if (at.x < pa.x || at.x > pb.x || at.y < pa.y || at.y > pb.y) {
        tapped_out = true;
        continue;
      }
      for (int k = 0; k < visible && top_ + k < n; ++k) {
        const ImVec2 a = P(L + 16, kListTop + k * kRowH), b = P(R - 16, kListTop + k * kRowH + kRowH - 6);
        if (at.x >= a.x && at.x < b.x && at.y >= a.y && at.y < b.y) sel_ = top_ + k, choose = true;
      }
    }
  }

  // Actions.
  if (n && (choose || other)) {
    const Row row = rows[sel_];
    if (row.kind == kInviteRow) {
      if (choose) Accept(st.invites[row.index]);
      else Decline(st.invites[row.index].from), Say("INVITE DECLINED");
    } else if (row.kind == kFriendRow && choose) {
      const Friend& f = st.friends[row.index];
      if (!in_session) Say("CREATE OR JOIN A SESSION FIRST, THEN INVITE");
      else SendInvite(f.name, f.status);  // (any friend: an offline one sees it when their game polls next)
    } else if (row.kind == kAddedRow && choose) {
      AddFriend(st.added_you[row.index]);
    } else if (row.kind == kPopupRow && choose) {
      const bool on = !rex::cvar::Query<bool>("online_invite_popups");
      rex::cvar::SetFlagByName("online_invite_popups", on ? "true" : "false");
      SaveConfigSetting("online_invite_popups", on ? "true" : "false");
      Say(on ? "INVITE POP-UPS ON" : "INVITE POP-UPS OFF (INVITES STILL SHOW HERE)");
    } else if (row.kind == kAddRow && choose) {
      if (typed_.size() >= 3) AddFriend(typed_), typed_.clear();
      else Say("TYPE THE PLAYER'S NAME (KEYBOARD, OR Y FOR LETTERS), THEN ENTER");
    }
  }

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
  dl->AddRectFilled(P(L, T), P(R, B), IM_COL32(8, 8, 12, 245), 10 * s);
  dl->AddRectFilledMultiColor(P(L + 2, T + 2), P(R - 2, T + 200), IM_COL32(40, 40, 48, 120), IM_COL32(40, 40, 48, 120),
                              IM_COL32(8, 8, 12, 0), IM_COL32(8, 8, 12, 0));
  dl->AddRect(P(L, T), P(R, B), IM_COL32(235, 235, 240, 255), 10 * s, 0, 2.5f * s);
  // header
  dl->AddRectFilled(P(L + 6, T + 6), P(R - 6, T + 44), IM_COL32(24, 24, 30, 255), 6 * s);
  dl->AddRectFilled(P(L + 6, T + 6), P(L + 12, T + 44), kRed);
  Text(dl, g_title_font, 32 * s, P(L + 24, T + 8), kWhite, invite_mode_ ? "INVITE FRIENDS" : "ONLINE");
  {
    const std::string who = st.signed_in ? Upper(rex::cvar::Query<std::string>("online_name")) : "NOT SIGNED IN";
    const ImVec2 sz = TextSize(g_menu_font, 20 * s, who.c_str());
    Text(dl, g_menu_font, 20 * s, ImVec2(P(R - 20, 0).x - sz.x, P(0, T + 15).y), kGrey, who.c_str());
  }
  // the session line
  {
    std::string line;
    ImU32 colour = kGrey;
    if (!st.signed_in) {
      line = "SIGN IN ON THE LAUNCHER'S ONLINE TAB";
    } else if (in_session) {
      line = std::string(slots > 6 ? "ROYAL RUMBLE" : "MATCH") + " SESSION: A INVITES AN ONLINE FRIEND";
      colour = kGold;
    } else {
      line = "CREATE OR JOIN A SESSION TO INVITE FRIENDS";
    }
    Text(dl, g_menu_font, 19 * s, P(L + 24, T + 62), colour, line.c_str());
    if (!st.error.empty()) Text(dl, g_menu_font, 19 * s, P(L + 24, T + 88), kRed, st.error.c_str());
    else if (st.signed_in && !st.answered) Text(dl, g_menu_font, 19 * s, P(L + 24, T + 88), kDim, "LOADING...");
  }

  // the list
  for (int k = 0; k < visible && top_ + k < n; ++k) {
    const int i = top_ + k;
    const Row row = rows[i];
    const float y = kListTop + k * kRowH;
    const bool on = i == sel_;
    dl->AddRectFilled(P(L + 16, y), P(R - 16, y + kRowH - 6), on ? IM_COL32(150, 20, 24, 255) : IM_COL32(26, 26, 32, 255),
                      6 * s);
    if (on) dl->AddRect(P(L + 16, y), P(R - 16, y + kRowH - 6), kWhite, 6 * s, 0, 1.5f * s);
    std::string name, right;
    ImU32 rc = kGrey;
    switch (row.kind) {
      case kInviteRow: {
        const Invite& v = st.invites[row.index];
        name = Upper(v.from);
        right = "INVITES YOU (" + Upper(v.kind.empty() ? "match" : v.kind) + ")";
        rc = kGold;
        break;
      }
      case kFriendRow: {
        const Friend& f = st.friends[row.index];
        name = Upper(f.name);
        if (f.status == "playing") right = "IN AN ONLINE MATCH", rc = kGreen;
        else if (f.status == "online") right = "ONLINE", rc = kGreen;
        else if (f.status == "ingame") right = "IN THE GAME", rc = kGreen;
        else right = f.mutual ? "OFFLINE" : "OFFLINE (HASN'T ADDED YOU)", rc = kDim;
        if (in_session && invited.count(session_key + f.name)) right = "INVITED", rc = kGold;
        break;
      }
      case kAddedRow:
        name = Upper(st.added_you[row.index]);
        right = "ADDED YOU", rc = kGrey;
        break;
      case kAddRow:
        name = "ADD FRIEND: " + Upper(typed_) + (on && (int(ImGui::GetTime() * 2) & 1) ? "_" : "");
        rc = kDim;
        break;
      case kPopupRow: {
        const bool pop = rex::cvar::Query<bool>("online_invite_popups");
        name = "INVITE POP-UPS";
        right = pop ? "ON" : "OFF", rc = pop ? kGreen : kDim;
        break;
      }
    }
    Text(dl, g_menu_font, 22 * s, P(L + 30, y + 11), kWhite, name.c_str());
    const ImVec2 sz = TextSize(g_menu_font, 18 * s, right.c_str());
    Text(dl, g_menu_font, 18 * s, ImVec2(P(R - 30, 0).x - sz.x, P(0, y + 14).y), rc, right.c_str());
  }
  if (st.signed_in && st.answered && st.friends.empty() && st.invites.empty() && st.added_you.empty()) {
    Text(dl, g_menu_font, 19 * s, P(L + 24, kListTop + kRowH + 8), kDim,
         invite_mode_ ? "NO FRIENDS YET: ADD THEM ON THE ONLINE MENU (BACK + RB)" : "NO FRIENDS YET: ADD ONE BELOW OR ON THE LAUNCHER");
  }
  if (top_ > 0) Text(dl, g_menu_font, 18 * s, P(R - 40, kListTop - 26), kGrey, "^");
  if (top_ + visible < n) Text(dl, g_menu_font, 18 * s, P(R - 40, B - 94), kGrey, "v");

  // ADD FRIEND's letter grid (a controller), over the list's bottom
  if (grid_) {
    const float kCell = 46, gx0 = L + 20, gy0 = B - 330;
    dl->AddRectFilled(P(gx0 - 10, gy0 - 10), P(gx0 + 10 * kCell + 10, gy0 + 5 * kCell + 10), IM_COL32(16, 16, 22, 250), 6 * s);
    dl->AddRect(P(gx0 - 10, gy0 - 10), P(gx0 + 10 * kCell + 10, gy0 + 5 * kCell + 10), IM_COL32(90, 90, 100, 255), 6 * s);
    for (int r = 0; r < 5; ++r) {
      const int cols = r < 4 ? 10 : 2;
      for (int c = 0; c < cols; ++c) {
        const float w = r < 4 ? kCell : kCell * 5;
        const ImVec2 a = P(gx0 + c * w + 2, gy0 + r * kCell + 2), b = P(gx0 + (c + 1) * w - 2, gy0 + (r + 1) * kCell - 2);
        const bool here = r == gy_ && c == gx_;
        dl->AddRectFilled(a, b, here ? kRed : IM_COL32(34, 34, 40, 255), 4 * s);
        char label[8] = {};
        if (r < 4) label[0] = kGrid[r][c] == ' ' ? '_' : kGrid[r][c];
        const char* t = r < 4 ? (kGrid[r][c] == ' ' ? "SPC" : label) : (c == 0 ? "DEL" : "OK");
        const ImVec2 sz = TextSize(g_menu_font, 20 * s, t);
        Text(dl, g_menu_font, 20 * s, ImVec2((a.x + b.x - sz.x) * 0.5f, (a.y + b.y - sz.y) * 0.5f), kWhite, t);
      }
    }
  }

  // the outcome of the last action, and the buttons
  if (fresh_message) Text(dl, g_menu_font, 19 * s, P(L + 24, B - 78), kGold, message.c_str());
  std::string help = "B CLOSE";
  if (n) {
    switch (rows[sel_].kind) {
      case kInviteRow: help = "A ACCEPT    X DECLINE    B CLOSE"; break;
      case kFriendRow: help = in_session ? "A INVITE    B CLOSE" : "B CLOSE"; break;
      case kAddedRow: help = "A ADD BACK    B CLOSE"; break;
      case kPopupRow: help = "A CHANGE    B CLOSE"; break;
      case kAddRow:
        help = grid_ ? "D-PAD MOVE    A TYPE    X DELETE    B DONE" : "TYPE A NAME (Y: LETTERS)    ENTER ADD    ESC CLOSE";
        break;
    }
  }
  if (rex::cvar::Query<bool>("touch_controls")) help = "TAP A ROW    TAP OUTSIDE TO CLOSE";  // (a phone)
  Text(dl, g_menu_font, 18 * s, P(L + 24, B - 44), kGrey, help.c_str());
  if (tapped_out || (back && !(typing && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !typed_.empty()))) Close();
  else if (back) typed_.clear();
}

// -- the game restarting itself to join (online_overlay.h) ------------------------------

rex::kernel::xam::XamModule::LoaderData* LoaderData() {
  if (!g_kernel) return nullptr;
  auto xam = g_kernel->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
  return xam ? &xam->loader_data() : nullptr;
}

// relaunch.bin: "SVRL", u32 launch data size, the data, the invite (60-byte
// XSESSION_INFO + the inviter's XUID).
bool SaveRelaunch() {
  auto* loader = LoaderData();
  if (!loader || g_relaunch_file.empty()) return false;
  std::optional<Accepted> a;
  {
    std::lock_guard lock(g_mutex);
    a = g_accepted;
  }
  std::string out = "SVRL";
  const uint32_t size = loader->launch_data_present ? uint32_t(loader->launch_data.size()) : 0;
  out.append(reinterpret_cast<const char*>(&size), 4);
  out.append(reinterpret_cast<const char*>(loader->launch_data.data()), size);
  if (a) {
    out.append(reinterpret_cast<const char*>(a->info), 60);
    out.append(reinterpret_cast<const char*>(&a->inviter), 8);
  }
  {
    std::string hex;
    for (uint32_t i = 0; i < size; ++i) {
      char h[4];
      std::snprintf(h, sizeof h, "%02X", loader->launch_data[i]);
      hex += h;
      if (i % 4 == 3) hex += ' ';
    }
    REXLOG_INFO("online overlay: launch data {}", hex);
  }
  std::ofstream f(g_relaunch_file, std::ios::binary | std::ios::trunc);
  f.write(out.data(), std::streamsize(out.size()));
  return bool(f);
}

void LoadRelaunch() {
  std::error_code ec;
  if (g_relaunch_file.empty() || !std::filesystem::exists(g_relaunch_file, ec)) return;
  const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(g_relaunch_file, ec);
  std::ifstream f(g_relaunch_file, std::ios::binary);
  std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  f.close();
  std::filesystem::remove(g_relaunch_file, ec);
  uint32_t size = 0;
  if (age > std::chrono::minutes(3) || d.size() < 8 || d.compare(0, 4, "SVRL") != 0) return;
  std::memcpy(&size, d.data() + 4, 4);
  if (8 + size > d.size()) return;
  if (auto* loader = LoaderData(); loader && size) {
    loader->launch_data.assign(d.begin() + 8, d.begin() + 8 + size);
    // An invite (word 0 = 2): word 2 is the player who took it - the game's
    // invite object never learns it here (-1), and the game would then wait
    // for "player -1" to sign in. It's player 1 (index 0).
    auto& v = loader->launch_data;
    if (size >= 12 && v[3] == 2 && v[8] == 0xFF && v[9] == 0xFF && v[10] == 0xFF && v[11] == 0xFF)
      v[8] = v[9] = v[10] = v[11] = 0;
    loader->launch_data_present = true;
  }
  if (d.size() >= 8 + size + 68) {
    Accepted a;
    std::memcpy(a.info, d.data() + 8 + size, 60);
    std::memcpy(&a.inviter, d.data() + 8 + size + 60, 8);
    a.at = Clock::now();
    {
      std::lock_guard lock(g_mutex);
      g_accepted = a;
      g_relaunched_at = a.at;
    }
    // (the host again, once P2P is up: so its packets get through)
    std::thread([a] {
      std::this_thread::sleep_for(std::chrono::seconds(15));
      P2PReachHost(a.info);
    }).detach();
  }
  REXLOG_INFO("online overlay: restarted to join an invite ({} bytes of launch data)", size);
}

// A new copy of the game, with this one's command line, folder and settings.
bool StartNewCopy(const std::wstring& extra = {}) {
#ifdef _WIN32
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
  std::wstring cmd = GetCommandLineW();
  if (!extra.empty() && cmd.find(extra) == std::wstring::npos) cmd += L" " + extra;
  SetEnvironmentVariableW(L"SVR2011_RELAUNCHED", L"1");  // (inherited)
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  return false;  // (Android: not yet - the activity would have to restart itself)
#endif
}

}  // namespace

void InstallOnlineOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, rex::input::InputSystem* input,
                          rex::system::KernelState* kernel, const std::filesystem::path& user_data) {
  g_window = window;
  if (window) {  // (before the on-screen pad's listener: 900)
    window->app_context().CallInUIThread([window] { window->AddInputListener(&g_touch, 950); });
  }
  g_input = input;
  g_kernel = kernel;
  g_relaunch_file = user_data / "relaunch.bin";
  LoadRelaunch();
  ResumeRejoin();  // (a copy RESTART AND REJOIN started: back to the session)
  new OnlineOverlay(drawer);  // lives for the whole run
  rex::ui::RegisterBind("bind_online_menu", "F1", "Online menu (friends, invites)", [] { g_toggle = true; });
  std::thread(PollThread).detach();
}

void SetOnlineOverlayFonts(ImFont* menu, ImFont* title) {
  g_menu_font = menu;
  g_title_font = title;
}

void OnlineOverlayNotice(const std::string& line1, const std::string& line2, int seconds) {
  std::lock_guard lock(g_mutex);
  g_notice = line1, g_notice2 = line2, g_notice_secs = seconds;
}

void OnlineOverlaySayGoodbye() {
  // (on a thread of its own: the window closes after 1.5 s at most, answered or not)
  auto done = std::make_shared<std::atomic<bool>>(false);
  std::thread([done] {
    SayGoodbye();
    *done = true;
  }).detach();
  for (int i = 0; i < 30 && !*done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void ToggleOnlineOverlay() { g_toggle = true; }

bool OnlineOverlayHoldsInput() {
  return g_open.load() || g_wait_release.load() || g_relay_dialog.load() || g_mods_dialog.load();
}

uint16_t OnlineRawPad() { return g_guest_pad.load(); }

// -- an accepted invite: the game walks the player into the session ----------
//
// Accepted anywhere, the invite's session comes first in the game's session
// search (P2PExpectInvite); then the walk below presses the buttons - on the
// game's pad reads (OnlineOverlayPad), never the PC's keyboard or mouse - from
// the main menu (any of its groups: B to the top) or the online menus to
// ONLINE > MATCH (ROYAL RUMBLE) > PLAYER MATCH > CUSTOM MATCH > SEARCH FOR A
// MATCH, the first session (the invited one) and JOIN THIS SESSION? YES. In
// a match, an editor or a session's lobby it waits until the player is back
// at a menu. BACK on the pad stops it; it gives up after a while on a screen
// it doesn't know (then the old way: the toast says where to go).
//
// Where the cursor is: each menu row has a description (string.pac), looked
// up once when the cursor moves onto it - the latest of a screen's rows'
// descriptions is the row. Where the player is: the main-menu object's group
// (menu_hooks.cpp: < 1000 a main-menu group, else the screen opened from it -
// 0x5EC the online menus).
namespace {
using namespace rex::input;
enum Desc : uint32_t {
  kOnMatch = 20218, kOnRumble = 20240, kOnCc = 20241, kOnInfo = 20221, kOnBoards = 20220, kOnShop = 20296,
  kMaRanked = 20216, kMaPlayer = 20217,
  kPmQuick = 20200, kPmCustom = 20201, kPmCreate = 20202, kPmPrivate = 20203,
  kCmType = 20211, kCmContent = 20212, kCmSearch = 20215,
  kNoSessions = 50900, kJoinThis = 20564,
};
constexpr uint32_t kWatched[] = {kOnMatch, kOnRumble, kOnCc,      kOnInfo,    kOnBoards,  kOnShop,   kMaRanked,
                                 kMaPlayer, kPmQuick, kPmCustom,  kPmCreate,  kPmPrivate, kCmType,   kCmContent,
                                 kCmSearch, kNoSessions, kJoinThis};
constexpr uint32_t kOnlineRows[] = {kOnMatch, kOnRumble, kOnCc, kOnInfo, kOnBoards, kOnShop};
constexpr uint32_t kMatchRows[] = {kMaRanked, kMaPlayer};
constexpr uint32_t kPlayerRows[] = {kPmQuick, kPmCustom, kPmCreate, kPmPrivate};
constexpr uint32_t kCustomRows[] = {kCmType, kCmContent, kCmSearch};
constexpr uint32_t kOnlineScreen = 0x5EC;  // (the main-menu object's group while in the online menus)
constexpr uint32_t kBarList = 207;         // the session list's help bar
constexpr uint32_t kBarJoining = 204;
constexpr uint32_t kBarJoinBox = 201;      // JOIN THIS SESSION? (YES / NO)
constexpr uint32_t kBarMessage = 2;        // a message box (A SELECT)

std::atomic<int64_t> g_seen[std::size(kWatched)];  // (Clock ticks of each one's last lookup)
std::atomic<uint32_t> g_help_bar{0};
std::atomic<int64_t> g_help_bar_at{0};
std::atomic<int64_t> g_lobby_bar_at{0};  // (the last time a session's lobby bar - 1200..1239 - came up)

bool LobbyBar(uint32_t bar) { return bar >= 1200 && bar < 1240; }

int64_t Ticks() { return Clock::now().time_since_epoch().count(); }

// A session's lobby on screen (its help bars are 1200..1239), for a second and a half: the join is done.
bool LobbyShown() {  // (the overlay's thread)
  static uint32_t last = 0;
  static int64_t since = 0;
  const uint32_t bar = g_help_bar.load();
  if (bar != last) last = bar, since = Ticks();
  return bar >= 1200 && bar < 1240 &&
         Ticks() - since > std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(1500)).count();
}

int64_t SeenAt(uint32_t id) {
  for (size_t k = 0; k < std::size(kWatched); ++k)
    if (kWatched[k] == id) return g_seen[k].load();
  return 0;
}

// The row of a screen the cursor is on (0: none seen) and when it was seen.
template <size_t N>
uint32_t Latest(const uint32_t (&rows)[N], int64_t* at = nullptr) {
  uint32_t best = 0;
  int64_t t = 0;
  for (uint32_t id : rows)
    if (const int64_t s = SeenAt(id); s > t) t = s, best = id;
  if (at) *at = t;
  return best;
}

template <size_t N>
bool Has(const uint32_t (&rows)[N], uint32_t id) {
  return std::find(std::begin(rows), std::end(rows), id) != std::end(rows);
}

// The walk (game thread; Accept() on the overlay's asks for it).
std::mutex g_walk_mutex;
struct WalkRequest {
  bool rumble = false;
  std::string from;
  Clock::time_point until;
};
std::optional<WalkRequest> g_walk_request;

enum class Step { kIdle, kWait, kMainTop, kMainOnline, kOnline, kMatch, kPlayer, kCustom, kSearching, kList, kJoin, kJoining };
struct Walk {
  Step step = Step::kIdle;
  bool rumble = false;
  std::string from;
  Clock::time_point until;          // (the invite's time)
  Clock::time_point step_at;        // (this step's start)
  Clock::time_point next;           // (no new press before)
  uint16_t press = 0;
  Clock::time_point press_end;
  int presses = 0;                  // (in this step)
  int searches = 0;
  int retries = 0;
  bool told_wait = false;
} g_walk;

const char* StepName(Step s) {
  switch (s) {
    case Step::kWait: return "waiting for a menu";
    case Step::kMainTop: return "main menu";
    case Step::kMainOnline: return "main menu: ONLINE";
    case Step::kOnline: return "ONLINE menu";
    case Step::kMatch: return "MATCH menu";
    case Step::kPlayer: return "PLAYER MATCH menu";
    case Step::kCustom: return "CUSTOM MATCH";
    case Step::kSearching: return "searching";
    case Step::kList: return "session list";
    case Step::kJoin: return "JOIN THIS SESSION?";
    case Step::kJoining: return "joining";
    default: return "idle";
  }
}

void Goto(Step s) {
  g_walk.step = s;
  g_walk.step_at = Clock::now();
  g_walk.presses = 0;
  REXLOG_INFO("online overlay: invite walk: {}", StepName(s));
}

void Press(uint16_t b, int gap_ms = 350) {
  const auto now = Clock::now();
  g_walk.press = b;
  g_walk.press_end = now + std::chrono::milliseconds(120);
  g_walk.next = g_walk.press_end + std::chrono::milliseconds(gap_ms);
  ++g_walk.presses;
}

std::string WhereToGo(const Walk& w) {
  return Upper(w.from) + "'S SESSION: ONLINE > " + (w.rumble ? "ROYAL RUMBLE" : "MATCH") +
         " > PLAYER MATCH > CUSTOM MATCH > SEARCH";
}

void Stop(const std::string& why) {
  REXLOG_INFO("online overlay: invite walk stopped ({})", why);
  if (!why.empty()) Say(why);
  g_walk = Walk{};
}

bool SeenSinceStep(uint32_t id) { return SeenAt(id) >= g_walk.step_at.time_since_epoch().count(); }

// The screen chosen hasn't come up: after 5 s, back to the step that chose
// it (its A again), at most 3 times.
void Retry(Step back) {
  if (Clock::now() - g_walk.step_at < std::chrono::seconds(5)) return;
  if (++g_walk.retries > 3) return Stop("COULDN'T GET THERE - " + WhereToGo(g_walk));
  REXLOG_INFO("online overlay: invite walk: the next screen didn't come up - again");
  Goto(back);
}

// Moves a screen's cursor to `want` (its rows' descriptions in `rows`), then A
// and the next step. False while it moves.
template <size_t N>
void ChooseRow(const uint32_t (&rows)[N], uint32_t want, Step then) {
  const uint32_t at = Latest(rows);
  if (!at) return;  // (the screen hasn't shown a row yet)
  // (a screen that has just come up ignores the pad while it slides in)
  if (Clock::now() - g_walk.step_at < std::chrono::milliseconds(1500)) return;
  if (at != want) {
    if (g_walk.presses > int(N) * 2 + 2) return Stop("COULDN'T GET THERE - " + WhereToGo(g_walk));
    const int from = int(std::find(std::begin(rows), std::end(rows), at) - std::begin(rows));
    const int to = int(std::find(std::begin(rows), std::end(rows), want) - std::begin(rows));
    Press(to > from ? X_INPUT_GAMEPAD_DPAD_DOWN : X_INPUT_GAMEPAD_DPAD_UP);
    return;
  }
  Press(X_INPUT_GAMEPAD_A, 600);
  Goto(then);
}

void WalkTick() {
  using namespace std::chrono;
  const auto now = Clock::now();
  if (g_walk.step == Step::kIdle) return;
  if (now > g_walk.until) return Stop("");  // (the invite is too old)
  if (g_walk.press) {
    if (now < g_walk.press_end) return;
    g_walk.press = 0;
  }
  if (now < g_walk.next) return;
  uint32_t group = 0, row = 0;
  const bool menu = ScriptMenuState(&group, &row);
  const uint32_t bar = g_help_bar.load();
  // (a session's lobby: its bars are 1200..1239 - one per match size and
  // cursor - and the joining box's)
  const bool in_lobby = group == kOnlineScreen && (LobbyBar(bar) || bar == kBarJoining);
  const auto in_step = now - g_walk.step_at;
  switch (g_walk.step) {
    case Step::kWait: {
      // (in the online menus: from the screen the cursor shows)
      int64_t t_on = 0, t_ma = 0, t_pm = 0, t_cm = 0;
      Latest(kOnlineRows, &t_on), Latest(kMatchRows, &t_ma), Latest(kPlayerRows, &t_pm), Latest(kCustomRows, &t_cm);
      const int64_t newest = std::max({t_on, t_ma, t_pm, t_cm});
      // (a match, an editor, a session's lobby: until the player is at a menu)
      const bool at_menu = menu && (group < 1000 || group == kOnlineScreen) && !in_lobby;
      // (after an online match the game comes back to its PLAYER MATCH menu without a menu select: the menu
      // object keeps the group it had before - an online menu's row looked up since the last lobby says the
      // player is in the online menus)
      const bool after_match = !at_menu && InMatch() && newest > g_lobby_bar_at.load() && !LobbyBar(bar) &&
                               bar != kBarJoining;
      const bool drivable = at_menu || after_match;
      if (!drivable) {
        if (!g_walk.told_wait) {
          Say("INVITE ACCEPTED: BACK AT A MENU, THE GAME TAKES YOU TO " + Upper(g_walk.from) + "'S SESSION");
          g_walk.told_wait = true;
        }
        g_walk.next = now + milliseconds(500);
        return;
      }
      if (g_walk.told_wait && in_step > seconds(1)) {  // (said "back at a menu": a moment to read it)
        Say("TAKING YOU TO " + Upper(g_walk.from) + "'S SESSION (BACK: STOP)");
        g_walk.next = now + seconds(2);
        g_walk.told_wait = false;
        return;
      }
      if (at_menu && group < 1000) return Goto(Step::kMainTop);
      if (after_match) REXLOG_INFO("online overlay: invite walk: the online menus after a match (no menu select)");
      if (bar == kBarList) return Goto(Step::kList);
      if (!newest) return Goto(Step::kOnline);
      if (newest == t_cm) return Goto(Step::kCustom);
      if (newest == t_pm) return Goto(Step::kPlayer);
      if (newest == t_ma) return Goto(Step::kMatch);
      return Goto(Step::kOnline);
    }
    case Step::kMainTop:
      if (!menu || group >= 1000) return Goto(Step::kWait);
      if (group != 1) {  // (a sub-menu: back to the top)
        if (g_walk.presses > 8) return Stop("COULDN'T GET THERE - " + WhereToGo(g_walk));
        return Press(X_INPUT_GAMEPAD_B, 800);
      }
      return Goto(Step::kMainOnline);
    case Step::kMainOnline: {
      if (group == kOnlineScreen) return Goto(Step::kOnline);
      if (!menu || group != 1) {
        if (in_step > seconds(60)) return Stop("COULDN'T GET THERE - " + WhereToGo(g_walk));
        return;  // (ONLINE chosen: its screen loading)
      }
      int rows = 0;
      const int want = ScriptMenuRow(1, "ONLINE", &rows);
      if (want < 0) return Stop("COULDN'T GET THERE - " + WhereToGo(g_walk));
      if (int(row) != want) {
        if (g_walk.presses > rows * 2 + 2) return Stop("COULDN'T GET THERE - " + WhereToGo(g_walk));
        return Press(int(row) < want ? X_INPUT_GAMEPAD_DPAD_DOWN : X_INPUT_GAMEPAD_DPAD_UP);
      }
      if (g_walk.presses < 100) Press(X_INPUT_GAMEPAD_A, 1500), g_walk.presses = 100;  // (once)
      return;
    }
    case Step::kOnline:
      if (in_step > seconds(60)) return Stop("COULDN'T GET THERE - " + WhereToGo(g_walk));
      if (!SeenSinceStep(Latest(kOnlineRows)) && in_step < seconds(3)) return;  // (its rows shown)
      return ChooseRow(kOnlineRows, g_walk.rumble ? kOnRumble : kOnMatch, Step::kMatch);
    case Step::kMatch: {
      // (the MATCH / ROYAL RUMBLE menu - the same two rows: once it shows them;
      // still the ONLINE menu after a while: its A was missed, again)
      int64_t t_ma = 0, t_on = 0;
      Latest(kMatchRows, &t_ma), Latest(kOnlineRows, &t_on);
      if (t_ma < t_on || !t_ma) return Retry(Step::kOnline);
      return ChooseRow(kMatchRows, kMaPlayer, Step::kPlayer);
    }
    case Step::kPlayer: {
      int64_t t_pm = 0, t_ma = 0;
      Latest(kPlayerRows, &t_pm), Latest(kMatchRows, &t_ma);
      if (t_pm < t_ma || !t_pm) return Retry(Step::kMatch);
      return ChooseRow(kPlayerRows, kPmCustom, Step::kCustom);
    }
    case Step::kCustom: {
      int64_t t_cm = 0, t_pm = 0;
      Latest(kCustomRows, &t_cm), Latest(kPlayerRows, &t_pm);
      if (t_cm < t_pm || !t_cm) return Retry(Step::kPlayer);
      return ChooseRow(kCustomRows, kCmSearch, Step::kSearching);
    }
    case Step::kSearching:
      if (bar == kBarList && g_help_bar_at.load() >= g_walk.step_at.time_since_epoch().count())
        return Goto(Step::kList);
      if (SeenSinceStep(kNoSessions) && bar == kBarMessage) {  // ("No game sessions were found. Search again?": YES)
        if (++g_walk.searches > 5) return Stop(Upper(g_walk.from) + "'S SESSION WASN'T FOUND - IT MAY HAVE ENDED");
        Press(X_INPUT_GAMEPAD_A, 800);
        return Goto(Step::kSearching);
      }
      if (in_step > seconds(45)) return Stop("COULDN'T GET THERE - " + WhereToGo(g_walk));
      return;
    case Step::kList:
      if (in_step < milliseconds(800)) return;  // (the list drawn)
      if (g_walk.presses) {
        // ("JOIN THIS SESSION?": its own help bar - its text is looked up
        // before, with the menus)
        if (bar == kBarJoinBox && g_help_bar_at.load() >= g_walk.step_at.time_since_epoch().count())
          return Goto(Step::kJoin);
        if (in_step > seconds(15)) return Stop("COULDN'T GET THERE - " + WhereToGo(g_walk));
        return;
      }
      return Press(X_INPUT_GAMEPAD_A, 1500);  // (the first session: the invited one)
    case Step::kJoin:
      if (in_step < milliseconds(500)) return;
      Press(X_INPUT_GAMEPAD_A, 1500);  // (YES)
      return Goto(Step::kJoining);
    case Step::kJoining:
      // (the lobby's bar alone: after an online match the menu object's group isn't the online menus')
      if (LobbyBar(bar) && g_help_bar_at.load() >= g_walk.step_at.time_since_epoch().count()) {
        REXLOG_INFO("online overlay: invite walk: in {}'s session", g_walk.from);
        Say("JOINED " + Upper(g_walk.from) + "'S SESSION");
        g_walk = Walk{};
        return;
      }
      if (in_step > seconds(45)) return Stop("COULDN'T JOIN " + Upper(g_walk.from) + "'S SESSION");
      return;
    default:
      return;
  }
}

// (Accept(): the walk, from the next pad read)
// RESTART AND REJOIN (the fetched mods load at a start: mod_transfer.h OnlineModsLoad): the session to go back
// to into OnlineMods\rejoin.bin ("SVRJ", its 60-byte XSESSION_INFO, the host's name), the session left as DON'T
// JOIN does, then a new copy with --online_mods_load=true and this one ends. The new copy reads the file
// (ResumeRejoin) and walks back as an accepted invite does (the session first in its searches).
uint64_t SessionId(const uint8_t* info) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = v << 8 | info[i];
  return v;
}

void RestartAndRejoin() {
  uint8_t info[60];
  uint32_t slots = 0;
  if (!P2PSessionInfo(info, &slots)) return Say("NOT IN A SESSION ANY MORE - JOIN IT AGAIN AFTER A RESTART");
  std::string host = OnlineModsHostName();
  if (host.empty()) host = "the host";
  const std::filesystem::path file = OnlineModsRoot() / "rejoin.bin";
  std::string out = "SVRJ";
  out.append(reinterpret_cast<const char*>(info), 60);
  out.push_back(char(slots > 6));
  out.push_back(char(std::min<size_t>(host.size(), 64)));
  out.append(host.substr(0, 64));
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  if (FILE* f = std::fopen(file.string().c_str(), "wb")) {
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
  } else {
    return Say("COULDN'T SAVE THE SESSION TO GO BACK TO");
  }
  REXLOG_INFO("online overlay: restart and rejoin - {}'s session {:016X}", host, SessionId(info));
  g_press_b = 8, g_leave_yes = 1;  // (out of the session first: the host keeps no stale slot for this game)
  std::thread([] {
    for (int i = 0; i < 100 && P2PSessionHost() != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool started = StartNewCopy(L"--online_mods_load=true");
    REXLOG_INFO("online overlay: restart and rejoin - {}", started ? "a new copy started, this one ends" : "couldn't start a new copy");
    if (!started) return Say("COULDN'T START THE GAME AGAIN - START IT YOURSELF, THEN JOIN THE SESSION");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // (the log written)
    std::_Exit(0);
  }).detach();
}

void ResumeRejoin() {
  if (!OnlineModsLoad()) return;
  const std::filesystem::path file = OnlineModsRoot() / "rejoin.bin";
  std::ifstream in(file, std::ios::binary);
  std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  std::error_code ec;
  std::filesystem::remove(file, ec);
  if (b.size() < 66 || b.compare(0, 4, "SVRJ") != 0) return;
  std::vector<uint8_t> info(b.begin() + 4, b.begin() + 64);
  const bool rumble = b[64] != 0;
  const std::string host = b.substr(66, size_t(uint8_t(b[65])));
  REXLOG_INFO("online overlay: back to {}'s session {:016X} with its mods loaded", host, SessionId(info.data()));
  P2PExpectInvite(info.data());
  RequestInviteWalk(rumble, host);
  std::thread([info] {
    std::this_thread::sleep_for(std::chrono::seconds(20));  // (the network up)
    P2PReachHost(info.data());
  }).detach();
  OnlineOverlayNotice("THE MODS ARE LOADED", "AT A MENU, THE GAME TAKES YOU BACK TO " + Upper(host) + "'S SESSION", 15);
}

void RequestInviteWalk(bool rumble, const std::string& from) {
  std::lock_guard lock(g_walk_mutex);
  g_walk_request = WalkRequest{rumble, from, Clock::now() + std::chrono::minutes(15)};
}
}  // namespace

void OnlineNoteString(uint32_t id) {
  for (size_t k = 0; k < std::size(kWatched); ++k) {
    if (kWatched[k] != id) continue;
    static const bool log = std::getenv("SVR2011_TEST_LOBBY_LOG") != nullptr;
    const int64_t now = Ticks(), was = g_seen[k].exchange(now);
    if (log && now - was > std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(500)).count())
      REXLOG_INFO("online overlay: string {} shown", id);
    return;
  }
}

// -- where the player is (the presence poll's &place=) ------------------------------------

std::atomic<int64_t> g_activity_at[size_t(OnlineActivity::kCount)];

void NoteOnlineActivity(OnlineActivity what) { g_activity_at[size_t(what)] = Ticks(); }

std::string PresencePlace(std::string* kind) {
  const auto lately = [](OnlineActivity a, int seconds) {
    const int64_t at = g_activity_at[size_t(a)].load();
    return at && Ticks() - at < std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(seconds)).count();
  };
  const int role = P2PSessionRole();  // (a match session: 1 hosts it, 2 joined)
  if (InMatch()) {
    if (role || P2POnlineMode()) {
      if (const auto ranked = P2PRanked(); ranked && kind) *kind = *ranked ? "ranked" : "player";
      return "match_online";
    }
    return "match_offline";
  }
  if (role) return role == 1 ? "lobby_host" : "lobby_join";
  uint32_t group = 0, row = 0;
  const bool menu = ScriptMenuState(&group, &row);
  if (P2POnlineMode()) {
    // (Community Creations and the leaderboards: the server requests they make, the last 30 s)
    if (lately(OnlineActivity::kCcUpload, 30)) return "cc_upload";
    if (lately(OnlineActivity::kCcDownload, 30)) return "cc_download";
    if (lately(OnlineActivity::kCcBrowse, 30)) return "cc";
    if (lately(OnlineActivity::kLeaderboards, 30)) return "leaderboards";
    if (lately(OnlineActivity::kSearch, 30)) return "matchmaking";
    // (else the ONLINE menu's row the cursor was on last, and whether the MATCH screens came after it)
    int64_t online_at = 0, match_at = 0, player_at = 0, custom_at = 0;
    const uint32_t online_row = Latest(kOnlineRows, &online_at);
    Latest(kMatchRows, &match_at), Latest(kPlayerRows, &player_at), Latest(kCustomRows, &custom_at);
    if (std::max({match_at, player_at, custom_at}) > online_at || online_row == kOnRumble) return "matchmaking";
    if (online_row == kOnCc) return "cc";
    if (online_row == kOnBoards) return "leaderboards";
    return "online_menu";
  }
  if (!menu) return "menus";
  if (group < 1000) {  // (a main-menu group: the suites' own menus, MY WWE, OPTIONS, the match setup)
    switch (group) {
      case 0xD: return "create_story";      // (NEW | LIST | ONLINE)
      case 0xE: return "create_superstar";  // (NEW | EDIT | SUPERSTAR THREADS | PAINT TOOL | ONLINE)
      case 0xF: return "create_finisher";
      case 4: return "create_menu";         // (CREATE MODES)
      case 5: return "my_wwe";
      case 0x10: return "practice";
      case 0x11: case 0x19: case 0x20: case 0x21: case 0x25: return "options";
      default: break;
    }
    if (group == 2 || (group >= 6 && group <= 0xC) || group == 0x18 || (group >= 0x1A && group <= 0x1F) ||
        (group >= 0x22 && group <= 0x27))
      return "match_setup";
    return "menus";
  }
  // (a screen opened from the main menu: what was picked last - by its place, not its label)
  uint32_t g = 0, r = 0;
  ScriptMenuSelects(&g, &r);
  if (g == 1) return r == 1 ? "universe" : r == 2 ? "rtwm" : r == 6 ? "shop" : "menus";
  if (g == 4) {
    static const char* const create[] = {"create_story", "create_superstar", "create_finisher", "create_entrance",
                                         "highlight_reel", "create_moveset"};
    return r < std::size(create) ? create[r] : "menus";
  }
  if (g == 0xD) return r == 2 ? "cc" : "create_story";          // (NEW | LIST | ONLINE)
  if (g == 0xE) return r == 2 ? "threads" : r == 3 ? "paint_tool" : r == 4 ? "cc" : "create_superstar";
  if (g == 0xF) return "create_finisher";
  if (g == 5) return r == 2 ? "practice" : "my_wwe";
  if (g == 0x11) return "options";
  if (g == 2 || (g >= 6 && g <= 0xC) || g == 0x18 || (g >= 0x1A && g <= 0x1F) || (g >= 0x22 && g <= 0x27))
    return "match_setup";
  return "menus";
}

void OnlineNoteHelpBar(uint32_t id) {
  g_help_bar = id;
  g_help_bar_at = Ticks();
  if (LobbyBar(id)) g_lobby_bar_at = Ticks();
}

uint16_t OnlineOverlayPad(uint16_t buttons) {
  using namespace rex::input;
  g_guest_pad = buttons;  // (the overlay's combo check: the real buttons)
  static const bool on = rex::cvar::Query<bool>("online_enabled");
  if (!on) return buttons;
  if (g_press_b > 0) {  // (the relay warning's BACK: the lobby's own B)
    if (--g_press_b == 0) return 0;
    return g_press_b > 2 ? uint16_t(X_INPUT_GAMEPAD_B) : 0;
  }
  if (g_leave_yes > 0) {  // (its box opens on NO: LEFT to YES, A - for up to ~5 s of pad reads, then left alone)
    static int reads = 0;
    const int step = g_leave_yes.load();
    if (step == 1) {
      if (g_help_bar.load() == kBarMessage) g_leave_yes = 2, reads = 0;
      else if (++reads > 300) g_leave_yes = 0, reads = 0;
      return buttons;
    }
    // (the box takes no input while it opens: a wait, then the press held a while, then a gap)
    ++reads;
    const int wait = step == 2 ? 45 : 20;
    if (reads > wait + 10 + 20) {
      reads = 0;
      if (step == 3) REXLOG_INFO("online overlay: mods question - YES to the lobby's \"Leave this session?\"");
      g_leave_yes = step == 3 ? 0 : 3;
    }
    return reads > wait && reads <= wait + 10 ? uint16_t(step == 2 ? X_INPUT_GAMEPAD_DPAD_LEFT : X_INPUT_GAMEPAD_A) : 0;
  }
  // An accepted invite's walk: its presses instead of the pad's; BACK stops it.
  {
    std::optional<WalkRequest> req;
    {
      std::lock_guard lock(g_walk_mutex);
      req.swap(g_walk_request);
    }
    if (req) {
      g_walk = Walk{};
      g_walk.rumble = req->rumble, g_walk.from = req->from, g_walk.until = req->until;
      Goto(Step::kWait);
    }
    if (g_walk.step != Step::kIdle) {
      if ((buttons & X_INPUT_GAMEPAD_BACK) && !OnlineOverlayHoldsInput()) {
        Stop("STOPPED - " + WhereToGo(g_walk));
      } else {
        WalkTick();
        if (g_walk.step != Step::kIdle && g_walk.step != Step::kWait) return g_walk.press;
      }
    }
  }
  // (game thread only) The game gets BACK as it is - its help screens
  // (Create-a-Superstar's, the menus') are open only while BACK is held; held
  // back until release, they closed at once. Only the overlay's BACK + RB is
  // kept from it, from the moment both are down until both are let go.
  static bool chord = false;
  const bool back = buttons & X_INPUT_GAMEPAD_BACK, rb = buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (back && rb) chord = true;
  if (!chord) return buttons;
  if (!back && !rb) chord = false;
  return uint16_t(buttons & ~(X_INPUT_GAMEPAD_BACK | X_INPUT_GAMEPAD_RIGHT_SHOULDER));
}

}  // namespace svr2011

// The game's INVITE FRIENDS (the lobby's X): XamShowGameInviteUI (its import
// thunk) opens the overlay instead of the Guide's invite screen.
REX_HOOK_RAW(sub_82904DB0) {
  (void)base;
  if (!svr2011::OnlineOverlayHoldsInput()) {
    svr2011::g_from_game = true;
    if (svr2011::g_kernel) svr2011::g_kernel->BroadcastNotification(svr2011::kSystemUi, 1);
    svr2011::ToggleOnlineOverlay();
  }
  ctx.r3.u64 = 0;  // ERROR_SUCCESS
}

// XInviteGetAcceptedInfo(user, XINVITE_INFO*) (the game's; it asks XLIVEBASE
// 0x58023): an invite accepted on the overlay - the inviter, this player, the
// title, the session (XSESSION_INFO) and "from a game invite".
REX_HOOK_RAW(sub_8298BA80) {
  std::optional<svr2011::Accepted> a;
  {
    std::lock_guard lock(svr2011::g_mutex);
    if (svr2011::g_accepted && svr2011::Clock::now() - svr2011::g_accepted->at > std::chrono::minutes(5))
      svr2011::g_accepted.reset();
    a = svr2011::g_accepted;
  }
  if (!a || !ctx.r4.u32) {
    __imp__sub_8298BA80(ctx, base);
    return;
  }
  uint8_t* out = base + ctx.r4.u32;
  std::memset(out, 0, 84);
  svr2011::Put64(out, a->inviter);
  svr2011::Put64(out + 8, std::strtoull(rex::cvar::Query<std::string>("online_xuid").c_str(), nullptr, 16));
  svr2011::Put32(out + 16, svr2011::kTitleId);
  std::memcpy(out + 20, a->info, 60);
  svr2011::Put32(out + 80, 1);
  REXLOG_INFO("online overlay: the game took the invite (XInviteGetAcceptedInfo)");
  ctx.r3.u64 = 0;
}

// The game launching a title (sub_8215ACE8: XamLoaderLaunchTitle(path, flags)):
// with a path - itself, to join an invite from its boot - a new copy of the
// game is started first (the launch data and the invite saved for it).
REX_HOOK_RAW(sub_8215ACE8) {
  bool invite = false;
  {
    std::lock_guard lock(svr2011::g_mutex);
    invite = svr2011::g_accepted.has_value();
  }
  if (ctx.r3.u32 && invite && REXCVAR_GET(online_invite_native)) {
    const char* path = reinterpret_cast<const char*>(base + ctx.r3.u32);
    const bool saved = svr2011::SaveRelaunch();
    const bool started = saved && svr2011::StartNewCopy();
    REXLOG_INFO("online overlay: the game relaunches itself ({}): {}", path,
                started ? "a new copy started" : saved ? "couldn't start a new copy" : "couldn't save its launch data");
  }
  __imp__sub_8215ACE8(ctx, base);
}

// XamShowSigninUI (its import thunk). The SDK's says "sign-in changed"
// (XN_SYS_SIGNINCHANGED) every time; booting into an invite, the game takes
// that as the invited player signing out ("returned to the Title Screen
// because a sign-in change has occurred"). For a few minutes after an
// invite's relaunch the player is simply still signed in: the UI closes.
REX_HOOK_RAW(sub_82904D30) {
  bool joining = false;
  {
    std::lock_guard lock(svr2011::g_mutex);
    joining = svr2011::g_accepted && svr2011::g_relaunched_at != svr2011::Clock::time_point{} &&
              svr2011::Clock::now() - svr2011::g_relaunched_at < std::chrono::minutes(5);
  }
  if (!joining) {
    __imp__sub_82904D30(ctx, base);
    return;
  }
  REXLOG_INFO("online overlay: sign-in UI while joining an invite: still signed in");
  if (svr2011::g_kernel) svr2011::g_kernel->BroadcastNotification(svr2011::kSystemUi, 0);
  ctx.r3.u64 = 0;  // ERROR_SUCCESS
}

// XamUserGetSigninState (its import thunk), asked once at boot by the game's
// launch-data check (sub_82574760: an invite relaunch - launch data word 0 =
// 2, word 2 = the player): only "not signed in" (0) lets it go on with the
// invite (it then waits for that player's sign-in); signed in already, it
// says "returned to the Title Screen because a sign-in change has occurred".
REX_HOOK_RAW(sub_82904AA8) {
  bool joining = false;
  if (ctx.lr == 0x8257478C) {  // (the check's call)
    std::lock_guard lock(svr2011::g_mutex);
    joining = svr2011::g_accepted && svr2011::g_relaunched_at != svr2011::Clock::time_point{};
  }
  if (!joining) {
    __imp__sub_82904AA8(ctx, base);
    return;
  }
  REXLOG_INFO("online overlay: the game's invite check at boot (user {})", ctx.r3.u32);
  ctx.r3.u64 = 0;
}

