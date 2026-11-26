/**
 * @file src/library_sync.cpp
 * @brief Keeping the app list in step with the games on this PC.
 *
 * When turned on (Apps > Keep my games in sync), TunaStream adds:
 *  - Steam games played in the last N days (Steam's own LastPlayed record), and
 *  - games newly installed through the other launchers in the last N days (Epic, GOG, EA,
 *    Ubisoft, Amazon and Microsoft Store record no play times, so a new install is the signal).
 * Synced apps are marked "tuna-sync" in apps.json, and removed again when the game is uninstalled
 * or, for Steam, not played for N days. Apps added by hand are never touched, and a synced app the
 * user deletes stays deleted. Covers come the usual way (box_art): from Steam, by id or by name.
 * Nothing changes while someone is streaming.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

// local includes
#include "config.h"
#include "file_handler.h"
#include "game_library.h"
#include "library_sync.h"
#include "logging.h"
#include "pairing_backup.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace library_sync {
  namespace {
    constexpr std::size_t MAX_SYNCED = 40;

    std::mutex lock;
    std::condition_variable wake;
    bool wake_requested = false;
    std::atomic_bool started {false};

    std::int64_t now_s() {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    fs::path state_file() {
      return platf::appdata() / "library_sync.json";
    }

    // Settings and memory: what was seen when, what was synced, what the user removed
    nlohmann::json load_state() {
      std::ifstream in {state_file()};
      nlohmann::json j = nlohmann::json::object();
      if (in) {
        try {
          j = nlohmann::json::parse(in);
        } catch (const std::exception &) {
          j = nlohmann::json::object();
        }
      }
      if (!j.is_object()) {
        j = nlohmann::json::object();
      }
      if (!j.contains("enabled")) {
        j["enabled"] = false;
      }
      if (!j.contains("days")) {
        j["days"] = 30;
      }
      for (auto key : {"seen", "synced", "excluded"}) {
        if (!j.contains(key) || !j[key].is_object()) {
          j[key] = nlohmann::json::object();
        }
      }
      return j;
    }

    void save_state(const nlohmann::json &j) {
      std::error_code ec;
      auto path = state_file();
      auto partial = path;
      partial += ".part";
      {
        std::ofstream out {partial, std::ios::trunc};
        out << j.dump(2);
        if (!out) {
          return;
        }
      }
      fs::rename(partial, path, ec);
    }

    std::string lower(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char) std::tolower(c);
      });
      return s;
    }

    /**
     * Steam's last-played times, from each account's localconfig.vdf
     * (UserLocalConfigStore > Software > Valve > Steam > apps > <appid> > LastPlayed).
     */
    std::map<std::string, std::int64_t> steam_last_played() {
      std::map<std::string, std::int64_t> out;
      auto root = game_library::steam_dir();
      if (root.empty()) {
        return out;
      }
      std::error_code ec;
      for (const auto &user : fs::directory_iterator {root / "userdata", ec}) {
        std::ifstream in(user.path() / "config" / "localconfig.vdf", std::ios::binary);
        if (!in) {
          continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();

        // A small KeyValues reader that keeps track of where it is
        std::vector<std::string> path;
        std::string pending;  // a key waiting for its value or block
        bool have_pending = false;
        for (std::size_t i = 0; i < text.size(); ++i) {
          const char c = text[i];
          if (c == '"') {
            std::string token;
            for (++i; i < text.size() && text[i] != '"'; ++i) {
              if (text[i] == '\\' && i + 1 < text.size()) {
                ++i;
              }
              token.push_back(text[i]);
            }
            if (!have_pending) {
              pending = std::move(token);
              have_pending = true;
            } else {
              // key = value
              if (lower(pending) == "lastplayed" && path.size() >= 2 && lower(path[path.size() - 2]) == "apps") {
                try {
                  auto t = (std::int64_t) std::stoll(token);
                  auto &best = out[path.back()];
                  best = std::max(best, t);
                } catch (const std::exception &) {
                }
              }
              have_pending = false;
            }
          } else if (c == '{') {
            path.push_back(have_pending ? pending : std::string {});
            have_pending = false;
          } else if (c == '}') {
            if (!path.empty()) {
              path.pop_back();
            }
            have_pending = false;
          }
        }
      }
      return out;
    }

    /**
     * When each program last ran, by name, from Windows' Prefetch folder: Windows keeps a small
     * file per program there (NAME.EXE-1A2B3C4D.pf) and rewrites it each time the program
     * starts. Other launchers (Epic, GOG, EA, Ubisoft, Amazon, Xbox) keep no play times we can
     * read, so this is how their games' last play is known. Empty where Prefetch is off.
     */
    std::map<std::string, std::int64_t> prefetch_last_run() {
      std::map<std::string, std::int64_t> out;
      std::error_code ec;
#ifdef _WIN32
      const char *windir = std::getenv("SystemRoot");
      const fs::path folder = fs::path {windir ? windir : "C:\\Windows"} / "Prefetch";
      for (const auto &entry : fs::directory_iterator {folder, ec}) {
        const auto name = entry.path().filename().string();
        const auto dash = name.rfind('-');
        if (dash == std::string::npos || lower(entry.path().extension().string()) != ".pf") {
          continue;
        }
        const auto when = fs::last_write_time(entry.path(), ec);
        if (ec) {
          continue;
        }
        // The file's time, as the system clock tells it
        const auto as_system = std::chrono::system_clock::now() +
                               std::chrono::duration_cast<std::chrono::system_clock::duration>(when - fs::file_time_type::clock::now());
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(as_system.time_since_epoch()).count();
        auto &best = out[lower(name.substr(0, dash))];
        best = std::max(best, (std::int64_t) seconds);
      }
#endif
      return out;
    }

    /// Programs in a game's folder that aren't the game: setups, crash reporters, redistributables
    bool not_the_game(const std::string &exe) {
      for (const auto *word : {"unins", "setup", "crash", "redist", "vcredist", "dxsetup", "directx", "dotnet", "report", "helper", "updater", "launcherpatcher", "easyanticheat", "eac", "battleye", "ue4prereq", "prereq", "install"}) {
        if (exe.find(word) != std::string::npos) {
          return true;
        }
      }
      return false;
    }

    /// When [g] was last played as far as Windows knows: the newest run of any program in its folder
    std::int64_t game_last_run(const game_library::game_t &g, const std::map<std::string, std::int64_t> &runs) {
      if (runs.empty()) {
        return 0;
      }
      std::set<std::string> exes;
      auto add = [&](const fs::path &p) {
        auto name = lower(p.filename().string());
        if (name.size() > 4 && name.ends_with(".exe") && !not_the_game(name)) {
          exes.insert(name);
        }
      };
      if (!g.cmd.empty()) {
        std::string cmd = g.cmd;
        std::erase(cmd, '"');
        if (const auto end = lower(cmd).find(".exe"); end != std::string::npos) {
          add(fs::path {cmd.substr(0, end + 4)});
        }
      }
      std::error_code ec;
      if (!g.working_dir.empty() && fs::is_directory(g.working_dir, ec)) {
        // The game's folder and one level down (Binaries\Win64 and the like), within reason
        int looked = 0;
        for (auto it = fs::recursive_directory_iterator {g.working_dir, fs::directory_options::skip_permission_denied, ec};
             it != fs::recursive_directory_iterator {} && looked < 4000; it.increment(ec), ++looked) {
          if (ec) {
            break;
          }
          if (it.depth() > 3) {
            it.disable_recursion_pending();
            continue;
          }
          if (it->is_regular_file(ec)) {
            add(it->path());
          }
        }
      }
      std::int64_t best = 0;
      for (const auto &exe : exes) {
        if (auto r = runs.find(exe); r != runs.end()) {
          best = std::max(best, r->second);
        }
      }
      return best;
    }

    nlohmann::json last_result = nlohmann::json::object();

    void sync_once(bool forced) {
      auto state = load_state();
      if (!state.value("enabled", false)) {
        return;
      }
      if (proc::proc.running() || rtsp_stream::session_count() > 0) {
        // Reloading the app list would end a game: next time
        return;
      }

      const auto now = now_s();
      const std::int64_t window = std::clamp(state.value("days", 30), 1, 365) * 86400LL;
      const bool first_run = state["seen"].empty();

      std::map<std::string, game_library::game_t> installed;
      for (auto &g : game_library::find_installed_games()) {
        if (g.source == "program") {
          continue;  // Start menu programs are added by hand only
        }
        installed.emplace(g.source + ":" + g.id, g);
      }
      if (installed.empty()) {
        return;
      }
      const auto played = steam_last_played();
      const auto runs = prefetch_last_run();
      std::map<std::string, std::int64_t> other_played;
      for (const auto &[key, g] : installed) {
        if (g.source != "steam") {
          other_played[key] = game_last_run(g, runs);
        }
      }

      // New installs are noticed from the second sync on: what's here on the first is old
      for (auto &[key, g] : installed) {
        if (!state["seen"].contains(key)) {
          state["seen"][key] = first_run ? 0 : now;
        }
      }

      auto apps_tree = nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()));
      auto &apps = apps_tree["apps"];
      std::set<std::string> synced_present;
      std::set<std::string> names;
      for (auto &app : apps) {
        names.insert(lower(app.value("name", "")));
        if (app.contains("tuna-sync")) {
          synced_present.insert(app.value("tuna-sync", ""));
        }
      }

      // A synced app that's gone from the list was removed by the user: it stays removed
      for (auto it = state["synced"].begin(); it != state["synced"].end();) {
        if (!synced_present.count(it.key())) {
          state["excluded"][it.key()] = now;
          it = state["synced"].erase(it);
        } else {
          ++it;
        }
      }

      auto recent = [&](const std::string &key, const game_library::game_t &g) -> std::int64_t {
        if (g.source == "steam") {
          auto p = played.find(g.id);
          return p != played.end() ? p->second : 0;
        }
        // Other launchers: when Windows last ran the game, or when it was newly installed
        auto p = other_played.find(key);
        return std::max(p != other_played.end() ? p->second : 0, state["seen"].value(key, (std::int64_t) 0));
      };
      // Whether we know when this game was last played (Steam always; others when Windows ran it)
      auto known = [&](const std::string &key, const game_library::game_t &g) {
        if (g.source == "steam") {
          return true;
        }
        auto p = other_played.find(key);
        return p != other_played.end() && p->second > 0;
      };

      // Remove: uninstalled, or (Steam) not played for the whole window
      std::vector<std::string> removed;
      for (auto it = apps.begin(); it != apps.end();) {
        if (!it->contains("tuna-sync")) {
          ++it;
          continue;
        }
        const auto key = it->value("tuna-sync", "");
        auto g = installed.find(key);
        const bool gone = g == installed.end();
        const bool stale = !gone && known(key, g->second) && recent(key, g->second) < now - window;
        if (gone || stale) {
          removed.push_back(it->value("name", ""));
          names.erase(lower(it->value("name", "")));
          state["synced"].erase(key);
          it = apps.erase(it);
        } else {
          ++it;
        }
      }

      // Add: the most recent first, up to the limit
      std::vector<std::pair<std::int64_t, std::string>> candidates;
      for (auto &[key, g] : installed) {
        const auto when = recent(key, g);
        if (when < now - window || when == 0 || state["excluded"].contains(key) || synced_present.count(key) || names.count(lower(g.name))) {
          continue;
        }
        candidates.emplace_back(when, key);
      }
      std::sort(candidates.rbegin(), candidates.rend());
      std::vector<std::string> added;
      for (auto &[when, key] : candidates) {
        if (state["synced"].size() >= MAX_SYNCED) {
          break;
        }
        const auto &g = installed.at(key);
        nlohmann::json app {
          {"name", g.name},
          {"cmd", g.cmd},
          {"working-dir", g.working_dir},
          {"detached", g.detached.empty() ? nlohmann::json::array() : nlohmann::json::array({g.detached})},
          {"image-path", ""},
          {"auto-detach", true},
          {"wait-all", true},
          {"exit-timeout", 5},
          {"elevated", false},
          {"exclude-global-prep-cmd", false},
          {"exclude-global-state-cmd", false},
          {"prep-cmd", nlohmann::json::array()},
          {"state-cmd", nlohmann::json::array()},
          {"scale-factor", 100},
          {"allow-client-commands", true},
          {"virtual-display", false},
          {"terminate-on-pause", false},
          {"tuna-sync", key},
        };
        proc::migrate_apps(&apps_tree, &app);
        state["synced"][key] = now;
        names.insert(lower(g.name));
        added.push_back(g.name);
      }

      if (!added.empty() || !removed.empty()) {
        file_handler::write_file(config::stream.file_apps.c_str(), apps_tree.dump(4));
        pairing_backup::save();
        proc::refresh(config::stream.file_apps, false);
        BOOST_LOG(info) << "Game library sync: added "sv << added.size() << ", removed "sv << removed.size();
      }
      save_state(state);

      std::lock_guard lg {lock};
      last_result = {{"at", now}, {"added", added}, {"removed", removed}, {"forced", forced}};
    }

    void loop() {
      std::this_thread::sleep_for(20s);
      while (true) {
        bool forced = false;
        try {
          sync_once(false);
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Game library sync: "sv << e.what();
        }
        std::unique_lock lk {lock};
        wake.wait_for(lk, 30min, [] {
          return wake_requested;
        });
        forced = wake_requested;
        wake_requested = false;
        (void) forced;
      }
    }
  }  // namespace

  void start() {
    if (started.exchange(true)) {
      return;
    }
    std::thread {loop}.detach();
  }

  nlohmann::json status() {
    auto state = load_state();
    nlohmann::json out;
    out["enabled"] = state.value("enabled", false);
    out["days"] = state.value("days", 30);
    out["synced"] = state["synced"].size();
    out["excluded"] = state["excluded"].size();
    std::lock_guard lg {lock};
    out["last"] = last_result;
    return out;
  }

  void configure(const nlohmann::json &settings) {
    auto state = load_state();
    if (settings.contains("enabled")) {
      state["enabled"] = settings.value("enabled", false);
    }
    if (settings.contains("days")) {
      state["days"] = std::clamp(settings.value("days", 30), 1, 365);
    }
    if (settings.value("forgetRemoved", false)) {
      state["excluded"] = nlohmann::json::object();
    }
    save_state(state);
    run_soon();
  }

  void run_soon() {
    std::lock_guard lg {lock};
    wake_requested = true;
    wake.notify_one();
  }
}  // namespace library_sync
