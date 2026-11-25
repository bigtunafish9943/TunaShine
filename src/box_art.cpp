/**
 * @file src/box_art.cpp
 * @brief Cover art for apps that don't have their own picture, taken from Steam.
 *
 * Moonlight clients show each app's picture (/appasset) as a game tile. Apps added from the
 * list of installed games have no picture, so they all showed the same box. For Steam games,
 * the cover is looked up in this order, and copied into the covers folder the first time:
 *   1. a custom cover set in the Steam library (userdata/<user>/config/grid/<appid>p.png or .jpg)
 *   2. the cover Steam itself shows in the library (appcache/librarycache)
 *   3. Steam's store servers (library_600x900 artwork)
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <thread>

// lib includes
#include <curl/curl.h>
#include <nlohmann/json.hpp>

// local includes
#include "box_art.h"
#include "game_library.h"
#include "logging.h"
#include "platform/common.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace box_art {
  namespace {
    std::mutex lock;
    /// When each Steam app id last failed, so a missing cover isn't asked for on every request
    std::map<std::string, std::chrono::steady_clock::time_point> failed;
    /// Names of the games installed through Steam (normalised) to their app ids
    std::map<std::string, std::string> steam_names;
    std::chrono::steady_clock::time_point steam_names_read;
    bool prefetching = false;

    constexpr auto RETRY_AFTER = 1h;
    constexpr auto REREAD_STEAM_AFTER = 10min;
    constexpr size_t MAX_COVER_BYTES = 8 * 1024 * 1024;

    fs::path cover_dir() {
      return platf::appdata() / "covers";
    }

    /// Letters and digits only, lower case: "Cyberpunk 2077®" and "cyberpunk 2077" match
    std::string normalise(const std::string &name) {
      std::string out;
      for (unsigned char c : name) {
        if (std::isalnum(c)) {
          out.push_back((char) std::tolower(c));
        }
      }
      return out;
    }

    bool is_picture(const std::string &bytes) {
      if (bytes.size() < 1024) {
        return false;
      }
      const bool jpeg = (unsigned char) bytes[0] == 0xFF && (unsigned char) bytes[1] == 0xD8;
      const bool png = bytes.compare(0, 8, "\x89PNG\r\n\x1a\n"s) == 0;
      return jpeg || png;
    }

    std::optional<std::string> read_picture(const fs::path &path) {
      std::error_code ec;
      auto size = fs::file_size(path, ec);
      if (ec || size > MAX_COVER_BYTES) {
        return std::nullopt;
      }
      std::ifstream in(path, std::ios::binary);
      std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (!is_picture(bytes)) {
        return std::nullopt;
      }
      return bytes;
    }

    /// A steam:// link or -applaunch in any of the app's commands
    std::string steam_id_in_commands(const proc::ctx_t &app) {
      static const std::regex link {R"(steam://(?:rungameid|run|launch)/(\d+))", std::regex::icase};
      static const std::regex applaunch {R"(-applaunch\s+(\d+))", std::regex::icase};
      std::vector<std::string> commands {app.cmd};
      commands.insert(commands.end(), app.detached.begin(), app.detached.end());
      for (const auto &prep : app.prep_cmds) {
        commands.push_back(prep.do_cmd);
      }
      for (const auto &command : commands) {
        std::smatch match;
        if (std::regex_search(command, match, link) || std::regex_search(command, match, applaunch)) {
          return match[1].str();
        }
      }
      return {};
    }

    std::string steam_id_for_name(const std::string &name) {
      const auto key = normalise(name);
      if (key.empty()) {
        return {};
      }
      std::lock_guard lg {lock};
      auto now = std::chrono::steady_clock::now();
      if (steam_names.empty() || now - steam_names_read > REREAD_STEAM_AFTER) {
        steam_names.clear();
        for (const auto &game : game_library::find_steam_games()) {
          steam_names[normalise(game.name)] = game.id;
        }
        steam_names_read = now;
      }
      auto it = steam_names.find(key);
      return it == steam_names.end() ? std::string {} : it->second;
    }

    size_t append_text(char *data, size_t size, size_t count, void *user) {
      auto *out = static_cast<std::string *>(user);
      if (out->size() + size * count > 512 * 1024) {
        return 0;
      }
      out->append(data, size * count);
      return size * count;
    }

    /// Names already looked up on the Steam store: the app id found, or empty for none
    std::map<std::string, std::string> store_lookups;

    /**
     * A game from another store (Epic, GOG, Xbox...) is often on Steam too: look its name up on
     * the Steam store and take the result only when the name matches exactly.
     */
    std::string steam_id_from_store(const std::string &name) {
      const auto key = normalise(name);
      static const std::set<std::string> not_games {"desktop", "steambigpicture", "steam", "virtualdesktop", "terminate"};
      if (key.size() < 3 || not_games.contains(key)) {
        return {};
      }
      {
        std::lock_guard lg {lock};
        if (auto it = store_lookups.find(key); it != store_lookups.end()) {
          return it->second;
        }
      }
      std::string found;
      CURL *curl = curl_easy_init();
      if (curl) {
        char *escaped = curl_easy_escape(curl, name.c_str(), (int) name.size());
        const std::string url = "https://store.steampowered.com/api/storesearch/?l=english&cc=US&term="s + (escaped ? escaped : "");
        if (escaped) {
          curl_free(escaped);
        }
        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_text);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#ifdef _WIN32
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
        const auto result = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (result == CURLE_OK) {
          try {
            auto json = nlohmann::json::parse(body);
            for (const auto &item : json.value("items", nlohmann::json::array())) {
              if (normalise(item.value("name", "")) == key && item.contains("id")) {
                found = std::to_string(item["id"].get<long long>());
                break;
              }
            }
          } catch (const std::exception &) {
          }
        } else if (result == CURLE_COULDNT_RESOLVE_HOST || result == CURLE_COULDNT_CONNECT || result == CURLE_OPERATION_TIMEDOUT) {
          return {};  // offline: don't remember a miss
        }
      }
      std::lock_guard lg {lock};
      store_lookups[key] = found;
      if (!found.empty()) {
        BOOST_LOG(info) << "["sv << name << "] found on the Steam store (app "sv << found << ')';
      }
      return found;
    }

    std::string steam_id_for(const proc::ctx_t &app, bool allow_download) {
      auto id = steam_id_in_commands(app);
      if (id.empty()) {
        id = steam_id_for_name(app.name);
      }
      if (id.empty() && allow_download) {
        id = steam_id_from_store(app.name);
      }
      return id;
    }

    /// A cover from the Steam library on this PC: a custom one first, then Steam's own
    std::optional<std::string> from_steam_library(const std::string &id) {
      const auto steam = game_library::steam_dir();
      if (steam.empty()) {
        return std::nullopt;
      }
      std::error_code ec;

      // Custom covers set in the library ("Manage > Set custom artwork")
      for (const auto &user : fs::directory_iterator {steam / "userdata", ec}) {
        for (const auto *ext : {".png", ".jpg", ".jpeg"}) {
          if (auto bytes = read_picture(user.path() / "config" / "grid" / (id + "p" + ext))) {
            return bytes;
          }
        }
      }

      // The library's own cache: <id>_library_600x900.jpg, or a folder per game in newer Steam
      const auto cache = steam / "appcache" / "librarycache";
      if (auto bytes = read_picture(cache / (id + "_library_600x900.jpg"))) {
        return bytes;
      }
      for (const auto &entry : fs::recursive_directory_iterator {cache / id, ec}) {
        auto filename = entry.path().filename().string();
        if (entry.is_regular_file(ec) && filename.starts_with("library_600x900")) {
          if (auto bytes = read_picture(entry.path())) {
            return bytes;
          }
        }
      }
      return std::nullopt;
    }

    size_t append(char *data, size_t size, size_t count, void *user) {
      auto *out = static_cast<std::string *>(user);
      if (out->size() + size * count > MAX_COVER_BYTES) {
        return 0;  // too big: stop
      }
      out->append(data, size * count);
      return size * count;
    }

    /// The picture at url; unreachable is set when Steam's servers couldn't be reached at all
    std::optional<std::string> download(const std::string &url, bool &unreachable) {
      CURL *curl = curl_easy_init();
      if (!curl) {
        return std::nullopt;
      }
      std::string bytes;
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bytes);
      curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#ifdef _WIN32
      curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
      auto result = curl_easy_perform(curl);
      curl_easy_cleanup(curl);
      unreachable = result == CURLE_COULDNT_RESOLVE_HOST || result == CURLE_COULDNT_CONNECT || result == CURLE_OPERATION_TIMEDOUT;
      if (result != CURLE_OK || !is_picture(bytes)) {
        return std::nullopt;
      }
      return bytes;
    }

    std::optional<std::string> from_steam_store(const std::string &id) {
      for (const auto *host : {"https://shared.steamstatic.com/store_item_assets/steam/apps/", "https://cdn.cloudflare.steamstatic.com/steam/apps/"}) {
        for (const auto *file : {"/library_600x900.jpg", "/library_600x900_2x.jpg"}) {
          bool unreachable = false;
          if (auto bytes = download(host + id + file, unreachable)) {
            return bytes;
          }
          if (unreachable) {
            break;  // try the other server, but don't wait on this one again
          }
        }
      }
      return std::nullopt;
    }

    fs::path cached_path(const std::string &id) {
      return cover_dir() / ("steam_" + id + ".jpg");
    }

    /// Finds the cover and keeps a copy. Returns its path, or empty.
    std::string fetch(const std::string &id, const std::string &app_name, bool allow_download) {
      {
        std::lock_guard lg {lock};
        auto it = failed.find(id);
        if (it != failed.end() && std::chrono::steady_clock::now() - it->second < RETRY_AFTER) {
          return {};
        }
      }

      const char *where = "your Steam library";
      std::optional<std::string> bytes;
      try {
        bytes = from_steam_library(id);
      } catch (const std::exception &e) {
        BOOST_LOG(debug) << "Reading the Steam library's covers failed: "sv << e.what();
      }
      if (!bytes && allow_download) {
        where = "Steam";
        bytes = from_steam_store(id);
      }
      if (!bytes) {
        if (allow_download) {
          std::lock_guard lg {lock};
          failed[id] = std::chrono::steady_clock::now();
          BOOST_LOG(info) << "No cover found on Steam for ["sv << app_name << "] (app "sv << id << ')';
        }
        return {};
      }

      std::error_code ec;
      fs::create_directories(cover_dir(), ec);
      const auto path = cached_path(id);
      const auto partial = fs::path {path}.concat(".part");
      {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(bytes->data(), (std::streamsize) bytes->size());
        if (!out) {
          return {};
        }
      }
      fs::rename(partial, path, ec);
      if (ec) {
        fs::remove(partial, ec);
        return {};
      }
      BOOST_LOG(info) << "Cover for ["sv << app_name << "] taken from "sv << where;
      return path.string();
    }
  }  // namespace

  std::string cover_for(const proc::ctx_t &app, bool allow_download) {
    std::string id;
    try {
      id = steam_id_for(app, allow_download);
    } catch (const std::exception &e) {
      BOOST_LOG(debug) << "Looking up the Steam game for ["sv << app.name << "] failed: "sv << e.what();
    }
    if (id.empty()) {
      return {};
    }
    std::error_code ec;
    const auto path = cached_path(id);
    if (fs::exists(path, ec)) {
      return path.string();
    }
    return fetch(id, app.name, allow_download);
  }

  void prefetch(const std::vector<proc::ctx_t> &apps) {
    std::vector<proc::ctx_t> wanted;
    for (const auto &app : apps) {
      if (app.image_path.empty()) {
        wanted.push_back(app);
      }
    }
    if (wanted.empty()) {
      return;
    }
    {
      std::lock_guard lg {lock};
      if (prefetching) {
        return;
      }
      prefetching = true;
    }
    std::thread {[wanted = std::move(wanted)]() {
      for (const auto &app : wanted) {
        try {
          cover_for(app, true);
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Fetching the cover for ["sv << app.name << "] failed: "sv << e.what();
        }
      }
      std::lock_guard lg {lock};
      prefetching = false;
    }}.detach();
  }

  std::string content_type_of(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char head[2] {};
    in.read(reinterpret_cast<char *>(head), 2);
    return (in.gcount() == 2 && head[0] == 0xFF && head[1] == 0xD8) ? "image/jpeg" : "image/png";
  }
}  // namespace box_art
