/**
 * @file src/game_library.cpp
 * @brief Finds games and programs installed on this PC, so they can be added as apps.
 *
 * Each launcher keeps its own record of what is installed: Steam in its library manifests,
 * the Epic Games Launcher in JSON manifests under ProgramData, and GOG Galaxy in the registry.
 * Other programs are found through their Start menu shortcuts. Nothing is started or changed.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "game_library.h"
#include "logging.h"
#include "utility.h"

#ifdef _WIN32
  // platform includes
  #include <windows.h>
  #include <objbase.h>
  #include <shlobj.h>
  #include <shobjidl.h>
  #include <userenv.h>

  #include "platform/windows/misc.h"
#endif

using namespace std::literals;
namespace fs = std::filesystem;

namespace game_library {
#ifdef _WIN32
  namespace {
    std::string to_utf8(const std::wstring &w) {
      return platf::to_utf8(w);
    }

    std::string read_file(const fs::path &path) {
      std::ifstream in {path, std::ios::binary};
      std::stringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }

    std::string lower(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char) std::tolower(c);
      });
      return s;
    }

    /// Read a string value from the registry, or empty if it isn't there.
    std::wstring reg_string(HKEY root, const std::wstring &key, const std::wstring &value) {
      DWORD size = 0;
      if (RegGetValueW(root, key.c_str(), value.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0) {
        return {};
      }
      std::wstring out(size / sizeof(wchar_t), L'\0');
      if (RegGetValueW(root, key.c_str(), value.c_str(), RRF_RT_REG_SZ, nullptr, out.data(), &size) != ERROR_SUCCESS) {
        return {};
      }
      out.resize(wcsnlen(out.c_str(), out.size()));
      return out;
    }

    /**
     * @brief Split Valve's KeyValues text (.vdf, .acf) into its quoted tokens.
     * @details Keys and values alternate; braces open and close sections. Backslash escapes
     *          are undone, since paths are stored as "D:\\SteamLibrary".
     */
    std::vector<std::string> vdf_tokens(const std::string &text) {
      std::vector<std::string> tokens;
      for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') {
          continue;
        }
        std::string token;
        for (++i; i < text.size() && text[i] != '"'; ++i) {
          if (text[i] == '\\' && i + 1 < text.size()) {
            ++i;
          }
          token.push_back(text[i]);
        }
        tokens.push_back(std::move(token));
      }
      return tokens;
    }

    /// The value after the first occurrence of key in a token list, or empty.
    std::string vdf_value(const std::vector<std::string> &tokens, const std::string &key) {
      for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (lower(tokens[i]) == key) {
          return tokens[i + 1];
        }
      }
      return {};
    }

    void find_steam_games(std::vector<game_t> &out) {
      auto root = reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
      if (root.empty()) {
        root = reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
      }
      if (root.empty()) {
        return;
      }

      // Every library folder, including the one inside the Steam install
      std::set<fs::path> libraries {fs::path {root}};
      auto folders = vdf_tokens(read_file(fs::path {root} / "steamapps" / "libraryfolders.vdf"));
      for (size_t i = 0; i + 1 < folders.size(); ++i) {
        if (lower(folders[i]) == "path") {
          libraries.insert(fs::path {platf::from_utf8(folders[i + 1])});
        }
      }

      // Tools Steam installs alongside games
      static const std::set<std::string> tool_ids {"228980", "1070560", "1391110", "1628350", "1493710", "2180100", "250820"};

      std::error_code ec;
      for (const auto &library : libraries) {
        for (const auto &entry : fs::directory_iterator {library / "steamapps", ec}) {
          auto filename = entry.path().filename().string();
          if (!filename.starts_with("appmanifest_") || entry.path().extension() != ".acf") {
            continue;
          }
          auto tokens = vdf_tokens(read_file(entry.path()));
          auto appid = vdf_value(tokens, "appid");
          auto name = vdf_value(tokens, "name");
          if (appid.empty() || name.empty() || tool_ids.contains(appid)) {
            continue;
          }
          auto lname = lower(name);
          if (lname.find("redistributable") != std::string::npos || lname.starts_with("proton ") ||
              lname.find("steam linux runtime") != std::string::npos || lname.find("dedicated server") != std::string::npos) {
            continue;
          }
          out.push_back({"steam", appid, name, {}, "steam://rungameid/" + appid, {}});
        }
      }
    }

    void find_epic_games(std::vector<game_t> &out) {
      wchar_t program_data[MAX_PATH] {};
      if (!GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH)) {
        return;
      }
      auto manifests = fs::path {program_data} / "Epic" / "EpicGamesLauncher" / "Data" / "Manifests";

      std::error_code ec;
      for (const auto &entry : fs::directory_iterator {manifests, ec}) {
        if (entry.path().extension() != ".item") {
          continue;
        }
        try {
          auto item = nlohmann::json::parse(read_file(entry.path()));
          if (item.value("bIsIncompleteInstall", false)) {
            continue;
          }
          // Leave out the Unreal Engine and other non-game items
          if (item.contains("AppCategories") && item["AppCategories"].is_array()) {
            bool is_game = false;
            for (const auto &category : item["AppCategories"]) {
              if (category.is_string() && category.get<std::string>() == "games") {
                is_game = true;
              }
            }
            if (!is_game) {
              continue;
            }
          }
          auto name = item.value("DisplayName", "");
          auto ns = item.value("CatalogNamespace", "");
          auto catalog_id = item.value("CatalogItemId", "");
          auto app_name = item.value("AppName", "");
          if (name.empty() || ns.empty() || catalog_id.empty() || app_name.empty()) {
            continue;
          }
          auto url = "com.epicgames.launcher://apps/" + ns + "%3A" + catalog_id + "%3A" + app_name + "?action=launch&silent=true";
          out.push_back({"epic", app_name, name, {}, url, {}});
        } catch (const std::exception &e) {
          BOOST_LOG(debug) << "Skipping unreadable Epic manifest "sv << entry.path().string() << ": "sv << e.what();
        }
      }
    }

    void find_gog_games(std::vector<game_t> &out) {
      const std::wstring games_key = L"SOFTWARE\\WOW6432Node\\GOG.com\\Games";
      HKEY key;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, games_key.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return;
      }
      wchar_t sub[256];
      for (DWORD i = 0;; ++i) {
        DWORD length = 256;
        if (RegEnumKeyExW(key, i, sub, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
          break;
        }
        auto path = games_key + L"\\" + sub;
        auto name = reg_string(HKEY_LOCAL_MACHINE, path, L"gameName");
        auto exe = reg_string(HKEY_LOCAL_MACHINE, path, L"exe");
        auto dir = reg_string(HKEY_LOCAL_MACHINE, path, L"workingDir");
        if (dir.empty()) {
          dir = reg_string(HKEY_LOCAL_MACHINE, path, L"path");
        }
        std::error_code ec;
        if (name.empty() || exe.empty() || !fs::exists(exe, ec)) {
          continue;
        }
        out.push_back({"gog", to_utf8(sub), to_utf8(name), "\"" + to_utf8(exe) + "\"", {}, to_utf8(dir)});
      }
      RegCloseKey(key);
    }

    /// Start menu folders for all users and for each user profile on this PC.
    std::vector<fs::path> start_menu_folders() {
      std::vector<fs::path> folders;
      wchar_t program_data[MAX_PATH] {};
      if (GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH)) {
        folders.push_back(fs::path {program_data} / "Microsoft" / "Windows" / "Start Menu" / "Programs");
      }
      // TunaStream runs as a system service, so each user's own Start menu is found by profile
      wchar_t profiles[MAX_PATH] {};
      DWORD size = MAX_PATH;
      if (GetProfilesDirectoryW(profiles, &size)) {
        std::error_code ec;
        for (const auto &profile : fs::directory_iterator {fs::path {profiles}, ec}) {
          auto menu = profile.path() / "AppData" / "Roaming" / "Microsoft" / "Windows" / "Start Menu" / "Programs";
          if (fs::is_directory(menu, ec)) {
            folders.push_back(menu);
          }
        }
      }
      return folders;
    }

    void find_programs(std::vector<game_t> &out, const std::set<std::string> &taken_names) {
      if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return;
      }
      auto uninit = util::fail_guard([]() {
        CoUninitialize();
      });

      wchar_t windows_dir[MAX_PATH] {};
      GetWindowsDirectoryW(windows_dir, MAX_PATH);
      const auto windows_prefix = lower(to_utf8(windows_dir));

      std::set<std::string> seen_targets;
      std::set<std::string> seen_names = taken_names;
      std::error_code ec;
      for (const auto &folder : start_menu_folders()) {
        for (auto it = fs::recursive_directory_iterator {folder, fs::directory_options::skip_permission_denied, ec}; it != fs::recursive_directory_iterator {}; it.increment(ec)) {
          if (ec) {
            break;
          }
          const auto &link = it->path();
          if (lower(link.extension().string()) != ".lnk") {
            continue;
          }
          auto name = to_utf8(link.stem().wstring());
          auto lname = lower(name);
          // Uninstallers, help files and similar are not things to stream
          if (lname.find("uninstall") != std::string::npos || lname.find("readme") != std::string::npos ||
              lname.find("help") != std::string::npos || lname.find("documentation") != std::string::npos ||
              lname.find("manual") != std::string::npos || lname.find("release notes") != std::string::npos ||
              lname.find("website") != std::string::npos) {
            continue;
          }

          IShellLinkW *shell_link = nullptr;
          if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **) &shell_link))) {
            continue;
          }
          auto release_link = util::fail_guard([shell_link]() {
            shell_link->Release();
          });
          IPersistFile *persist = nullptr;
          if (FAILED(shell_link->QueryInterface(IID_IPersistFile, (void **) &persist))) {
            continue;
          }
          auto release_persist = util::fail_guard([persist]() {
            persist->Release();
          });
          if (FAILED(persist->Load(link.wstring().c_str(), STGM_READ))) {
            continue;
          }

          wchar_t target[MAX_PATH] {};
          wchar_t args[1024] {};
          wchar_t dir[MAX_PATH] {};
          if (FAILED(shell_link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH)) || !target[0]) {
            continue;
          }
          shell_link->GetArguments(args, 1024);
          shell_link->GetWorkingDirectory(dir, MAX_PATH);

          wchar_t expanded[MAX_PATH] {};
          ExpandEnvironmentStringsW(target, expanded, MAX_PATH);
          fs::path exe {expanded};
          auto lexe = lower(to_utf8(exe.wstring()));
          if (lower(exe.extension().string()) != ".exe" || !fs::exists(exe, ec) ||
              lexe.starts_with(windows_prefix) || seen_names.contains(lname)) {
            continue;
          }
          // One entry per program, even when several shortcuts open it the same way
          auto key = lexe + "|" + lower(to_utf8(args));
          if (!seen_targets.insert(key).second) {
            continue;
          }
          seen_names.insert(lname);

          std::string cmd = "\"" + to_utf8(exe.wstring()) + "\"";
          if (args[0]) {
            cmd += " " + to_utf8(args);
          }
          auto working_dir = dir[0] ? to_utf8(dir) : to_utf8(exe.parent_path().wstring());
          out.push_back({"program", lexe, name, cmd, {}, working_dir});
        }
      }
    }
  }  // namespace

  std::vector<game_t> find_installed_games() {
    std::vector<game_t> games;
    try {
      find_steam_games(games);
      find_epic_games(games);
      find_gog_games(games);
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Looking for installed games failed: "sv << e.what();
    }

    // Programs whose names match a game found above are that game's own shortcut
    std::set<std::string> game_names;
    for (const auto &game : games) {
      game_names.insert(lower(game.name));
    }
    try {
      find_programs(games, game_names);
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Looking for installed programs failed: "sv << e.what();
    }

    std::sort(games.begin(), games.end(), [](const game_t &a, const game_t &b) {
      if (a.source != b.source) {
        return a.source < b.source;
      }
      return lower(a.name) < lower(b.name);
    });
    BOOST_LOG(info) << "Found "sv << games.size() << " games and programs installed on this PC"sv;
    return games;
  }
#else
  std::vector<game_t> find_installed_games() {
    return {};
  }
#endif
}  // namespace game_library
