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
        return state["seen"].value(key, (std::int64_t) 0);
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
        const bool stale = !gone && g->second.source == "steam" && recent(key, g->second) < now - window;
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
