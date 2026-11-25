/**
 * @file src/captures.cpp
 * @brief Screenshots and clips from streams, saved in the signed-in user's Pictures and Videos folders.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

// local includes
#include "captures.h"
#include "logging.h"
#include "platform/common.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace captures {
  namespace {
    fs::path from_utf8(const std::string &s) {
      return fs::path {std::u8string(s.begin(), s.end())};
    }

    std::string to_utf8(const fs::path &p) {
      auto u8 = p.u8string();
      return std::string(u8.begin(), u8.end());
    }

    fs::path folder(platf::user_folder_e kind, const char *fallback) {
      auto base = platf::user_folder(kind);
      if (base.empty()) {
        return platf::appdata() / fallback;
      }
      return base / "TunaStream";
    }

    std::string kind_of(const fs::path &path) {
      auto ext = path.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return (char) std::tolower(c);
      });
      if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
        return "screenshot";
      }
      if (ext == ".mp4" || ext == ".mkv" || ext == ".hevc" || ext == ".h264") {
        return "clip";
      }
      return {};
    }
  }  // namespace

  fs::path screenshot_dir() {
    return folder(platf::user_folder_e::pictures, "screenshots");
  }

  fs::path clip_dir() {
    return folder(platf::user_folder_e::videos, "clips");
  }

  std::string file_name(const std::string &app, const std::string &extension) {
    std::string safe;
    for (unsigned char c : app) {
      // Keep letters, digits, spaces and a few marks; Windows refuses \ / : * ? " < > |
      if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '\'' || c == '!' || c == '&' || c == '(' || c == ')' || c >= 0x80) {
        safe.push_back((char) c);
      }
    }
    while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.')) {
      safe.pop_back();
    }
    if (safe.empty()) {
      safe = "Stream";
    }
    if (safe.size() > 60) {
      safe.resize(60);
    }
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << safe << ' ' << std::put_time(&local, "%Y-%m-%d %H.%M.%S") << extension;
    return out.str();
  }

  fs::path save_screenshot(const std::string &app, const std::string &bytes) {
    if (bytes.size() < 64 || bytes.size() > MAX_SCREENSHOT_BYTES) {
      return {};
    }
    const bool png = bytes.compare(0, 8, "\x89PNG\r\n\x1a\n"s) == 0;
    const bool jpeg = (unsigned char) bytes[0] == 0xFF && (unsigned char) bytes[1] == 0xD8;
    if (!png && !jpeg) {
      return {};
    }
    const auto dir = screenshot_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto path = dir / from_utf8(file_name(app, png ? ".png" : ".jpg"));
    // Two in the same second get a number
    for (int i = 2; fs::exists(path, ec) && i < 100; ++i) {
      path = dir / from_utf8(file_name(app, " (" + std::to_string(i) + ")" + (png ? ".png"s : ".jpg"s)));
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), (std::streamsize) bytes.size());
    if (!out) {
      BOOST_LOG(warning) << "Couldn't save a screenshot in "sv << to_utf8(dir);
      return {};
    }
    BOOST_LOG(info) << "Screenshot saved: "sv << to_utf8(path);
    return path;
  }

  std::vector<capture_t> recent(size_t at_most) {
    std::vector<capture_t> out;
    std::error_code ec;
    for (const auto &dir : {screenshot_dir(), clip_dir()}) {
      for (const auto &entry : fs::directory_iterator {dir, ec}) {
        try {
          if (!entry.is_regular_file(ec)) {
            continue;
          }
          auto kind = kind_of(entry.path());
          if (kind.empty()) {
            continue;
          }
          auto modified = entry.last_write_time(ec);
          auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::file_clock::to_sys(modified).time_since_epoch())
                           .count();
          out.push_back({to_utf8(entry.path().filename()), kind, (std::int64_t) seconds, entry.file_size(ec)});
        } catch (const std::exception &) {
          // A file whose name can't be read as text
        }
      }
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
      return a.modified > b.modified;
    });
    if (out.size() > at_most) {
      out.resize(at_most);
    }
    return out;
  }

  fs::path file_for(const std::string &kind, const std::string &name) {
    // Only a plain file name in one of the two folders
    if (name.empty() || name.find_first_of("/\\") != std::string::npos || name.find("..") != std::string::npos) {
      return {};
    }
    const auto dir = kind == "clip" ? clip_dir() : screenshot_dir();
    auto path = dir / from_utf8(name);
    std::error_code ec;
    if (kind_of(path) != kind || !fs::is_regular_file(path, ec)) {
      return {};
    }
    return path;
  }
}  // namespace captures
