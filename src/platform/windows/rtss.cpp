/**
 * @file src/platform/windows/rtss.cpp
 * @brief Capping games with RivaTuner Statistics Server (RTSS) while streaming.
 *
 * NVIDIA's own frame rate limit only takes whole numbers, so a stream at 59.94 would get games
 * capped at 60 and the PC would still make one frame too many every 17 seconds or so. RTSS can
 * cap at exactly 59.94 (its Global profile keeps the limit as Limit / LimitDenominator), and it
 * works with any graphics card. When RTSS is installed and running, TunaStream sets its global
 * limit for the stream and puts the user's own limit back afterwards, also after a crash.
 */
// standard includes
#include <filesystem>
#include <optional>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// platform includes
#include <Windows.h>
#include <TlHelp32.h>

// local includes
#include "src/logging.h"
#include "src/platform/common.h"
#include "rtss.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf::rtss {
  namespace {
    std::mutex lock;

    fs::path undo_file() {
      return platf::appdata() / "rtss_limit_undo.txt";
    }

    std::optional<fs::path> install_dir() {
      for (auto key : {L"SOFTWARE\\WOW6432Node\\Unwinder\\RTSS", L"SOFTWARE\\Unwinder\\RTSS"}) {
        wchar_t buf[MAX_PATH] {};
        DWORD size = sizeof(buf);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"InstallDir", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && buf[0]) {
          fs::path dir {buf};
          if (fs::exists(dir / "Profiles" / "Global")) {
            return dir;
          }
        }
      }
      fs::path fallback {L"C:\\Program Files (x86)\\RivaTuner Statistics Server"};
      if (fs::exists(fallback / "Profiles" / "Global")) {
        return fallback;
      }
      return std::nullopt;
    }

    bool rtss_running() {
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (snap == INVALID_HANDLE_VALUE) {
        return false;
      }
      PROCESSENTRY32W entry {};
      entry.dwSize = sizeof(entry);
      bool found = false;
      for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"RTSS.exe") == 0) {
          found = true;
          break;
        }
      }
      CloseHandle(snap);
      return found;
    }

    struct profile_t {
      std::vector<std::string> lines;
      int limit_line = -1;
      int denominator_line = -1;
    };

    // The [Framerate] section's Limit and LimitDenominator lines of the Global profile
    bool read_profile(const fs::path &path, profile_t &profile) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return false;
      }
      std::string line;
      bool in_framerate = false;
      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        if (!line.empty() && line.front() == '[') {
          in_framerate = line == "[Framerate]";
        } else if (in_framerate && line.rfind("Limit=", 0) == 0) {
          profile.limit_line = (int) profile.lines.size();
        } else if (in_framerate && line.rfind("LimitDenominator=", 0) == 0) {
          profile.denominator_line = (int) profile.lines.size();
        }
        profile.lines.push_back(line);
      }
      return profile.limit_line >= 0;
    }

    bool write_profile(const fs::path &path, const profile_t &profile) {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out) {
        return false;
      }
      for (auto &line : profile.lines) {
        out << line << "\r\n";
      }
      return (bool) out;
    }

    std::string value_of(const std::string &line) {
      auto eq = line.find('=');
      return eq == std::string::npos ? std::string {} : line.substr(eq + 1);
    }

    // Tells RTSS to read its profiles again
    void update_profiles(const fs::path &dir) {
      auto dll = LoadLibraryExW((dir / "RTSSHooks64.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (!dll) {
        BOOST_LOG(warning) << "RTSS: couldn't load RTSSHooks64.dll, the new limit applies when RTSS next reads its profiles"sv;
        return;
      }
      using update_fn = void (*)();
      if (auto update = (update_fn) GetProcAddress(dll, "UpdateProfiles")) {
        update();
      }
      FreeLibrary(dll);
    }
  }  // namespace

  bool set_limit(int millifps) {
    std::lock_guard lg {lock};
    auto dir = install_dir();
    if (!dir || !rtss_running()) {
      return false;
    }
    const auto path = *dir / "Profiles" / "Global";
    profile_t profile;
    if (!read_profile(path, profile)) {
      BOOST_LOG(warning) << "RTSS: its Global profile has no frame rate limit to set"sv;
      return false;
    }

    // Remember the user's own limit once, so a second stream doesn't take ours for theirs
    if (!fs::exists(undo_file())) {
      std::ofstream undo(undo_file(), std::ios::trunc);
      undo << value_of(profile.lines[profile.limit_line]) << '\n'
           << (profile.denominator_line >= 0 ? value_of(profile.lines[profile.denominator_line]) : "-"s) << '\n';
      if (!undo) {
        return false;
      }
    }

    int limit = millifps;
    int denominator = 1000;
    if (profile.denominator_line < 0) {
      // An older RTSS without fractional limits: the nearest whole number
      limit = (millifps + 500) / 1000;
      denominator = 1;
    } else {
      while (denominator > 1 && limit % 10 == 0) {
        limit /= 10;
        denominator /= 10;
      }
    }
    profile.lines[profile.limit_line] = "Limit=" + std::to_string(limit);
    if (profile.denominator_line >= 0) {
      profile.lines[profile.denominator_line] = "LimitDenominator=" + std::to_string(denominator);
    }
    if (!write_profile(path, profile)) {
      BOOST_LOG(warning) << "RTSS: couldn't write its Global profile"sv;
      return false;
    }
    update_profiles(*dir);
    BOOST_LOG(info) << "RTSS caps games at "sv << (millifps / 1000.0) << " FPS while streaming"sv;
    return true;
  }

  void restore() {
    std::lock_guard lg {lock};
    if (!fs::exists(undo_file())) {
      return;
    }
    std::string limit, denominator;
    {
      std::ifstream undo(undo_file());
      std::getline(undo, limit);
      std::getline(undo, denominator);
    }
    auto dir = install_dir();
    if (dir) {
      const auto path = *dir / "Profiles" / "Global";
      profile_t profile;
      if (read_profile(path, profile)) {
        profile.lines[profile.limit_line] = "Limit=" + limit;
        if (profile.denominator_line >= 0 && denominator != "-") {
          profile.lines[profile.denominator_line] = "LimitDenominator=" + denominator;
        }
        if (write_profile(path, profile)) {
          update_profiles(*dir);
          BOOST_LOG(info) << "RTSS: put back its own frame rate limit"sv;
        }
      }
    }
    std::error_code ec;
    fs::remove(undo_file(), ec);
  }

  bool installed() {
    return install_dir().has_value();
  }

  bool running() {
    return rtss_running();
  }
}  // namespace platf::rtss
