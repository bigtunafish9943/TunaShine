/**
 * @file src/settings_backup.cpp
 * @brief Backup and restore of TunaStream's settings as one file, and keeping TunaReceive's backups.
 *
 * A backup is a JSON document holding each file base64-encoded under a fixed name. Restoring
 * only writes those names back to where TunaStream keeps them, so a backup can't put a file
 * anywhere else. Pairings (paired devices, the PC's pairing certificate and key, and the web
 * page's sign-in) are only included when asked for, since whoever has them can act as this PC.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>

// lib includes
#include <Simple-Web-Server/crypto.hpp>

// local includes
#include "config.h"
#include "logging.h"
#include "platform/common.h"
#include "settings_backup.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace settings_backup {
  namespace {
    constexpr auto FORMAT = "tunastream-backup";
    constexpr size_t MAX_RECEIVER_BACKUP = 4 * 1024 * 1024;
    constexpr size_t KEEP_RECEIVER_BACKUPS = 20;

    std::int64_t now_seconds() {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /// Fixed names for the files a backup may hold, and where each lives
    std::map<std::string, fs::path> known_files(bool with_pairings) {
      std::map<std::string, fs::path> files {
        {"settings", config::tunastream.config_file},
        {"apps", config::stream.file_apps},
        {"play_history", platf::appdata() / "play_history.json"},
        {"play_milestones", platf::appdata() / "play_milestones.json"},
      };
      if (with_pairings) {
        files["state"] = config::nvhttp.file_state;
        files["cert"] = config::nvhttp.cert;
        files["pkey"] = config::nvhttp.pkey;
        files["credentials"] = config::tunastream.credentials_file;
      }
      return files;
    }

    fs::path sounds_dir() {
      return platf::appdata() / "sounds";
    }

    /// A sound's file name as kept in a backup: letters, digits, spaces and a few marks only
    bool safe_sound_name(const std::string &name) {
      if (name.empty() || name.size() > 60 || name.find("..") != std::string::npos) {
        return false;
      }
      return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '\'' || c == '!' || c == '.';
      });
    }

    bool read_bytes(const fs::path &path, std::string &out) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return false;
      }
      out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      return true;
    }

    bool write_bytes(const fs::path &path, const std::string &bytes) {
      std::error_code ec;
      if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
      }
      auto partial = path;
      partial += ".part";
      {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), (std::streamsize) bytes.size());
        if (!out) {
          return false;
        }
      }
      fs::rename(partial, path, ec);
      return !ec;
    }

    fs::path receiver_dir() {
      return platf::appdata() / "tunareceive_backups";
    }
  }  // namespace

  nlohmann::json make(bool with_pairings) {
    nlohmann::json files = nlohmann::json::object();
    for (const auto &[name, path] : known_files(with_pairings)) {
      std::string bytes;
      if (!path.empty() && read_bytes(path, bytes)) {
        files[name] = SimpleWeb::Crypto::Base64::encode(bytes);
      }
    }
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator {sounds_dir(), ec}) {
      try {
        const auto name = entry.path().filename().string();
        std::string bytes;
        if (entry.is_regular_file(ec) && safe_sound_name(name) && read_bytes(entry.path(), bytes)) {
          files["sounds/" + name] = SimpleWeb::Crypto::Base64::encode(bytes);
        }
      } catch (const std::exception &) {
        // A file name that isn't plain text; not one of ours
      }
    }
    BOOST_LOG(info) << "Backup made"sv << (with_pairings ? " (with pairings)"sv : ""sv);
    return {
      {"format", FORMAT},
      {"version", 1},
      {"made", now_seconds()},
      {"pc", config::nvhttp.tunastream_name},
      {"withPairings", with_pairings},
      {"files", files},
    };
  }

  std::string restore(const nlohmann::json &backup) {
    if (!backup.is_object() || backup.value("format", "") != FORMAT || !backup.contains("files") || !backup["files"].is_object()) {
      return "That file isn't a TunaStream backup.";
    }
    if (backup.value("version", 0) > 1) {
      return "That backup was made by a newer TunaStream. Update this one first.";
    }
    const auto known = known_files(true);
    std::map<fs::path, std::string> writes;
    for (const auto &[name, value] : backup["files"].items()) {
      if (!value.is_string()) {
        return "The backup is damaged.";
      }
      fs::path target;
      if (auto it = known.find(name); it != known.end()) {
        target = it->second;
      } else if (name.starts_with("sounds/") && safe_sound_name(name.substr(7))) {
        target = sounds_dir() / name.substr(7);
      } else {
        continue;  // something this version doesn't know; leave it
      }
      if (target.empty()) {
        continue;
      }
      writes[target] = SimpleWeb::Crypto::Base64::decode(value.get<std::string>());
    }
    if (writes.empty()) {
      return "The backup is empty.";
    }
    // Everything is decoded before anything is written, so a damaged backup changes nothing
    for (const auto &[path, bytes] : writes) {
      if (!write_bytes(path, bytes)) {
        BOOST_LOG(error) << "Restore: couldn't write "sv << path.string();
        return "Part of the backup couldn't be written. Check that TunaStream's folder isn't read-only, then try again.";
      }
    }
    BOOST_LOG(info) << "Backup restored ("sv << writes.size() << " files)"sv;
    return {};
  }

  std::string keep_receiver_backup(const std::string &device, const std::string &json) {
    if (json.size() > MAX_RECEIVER_BACKUP) {
      return {};
    }
    try {
      auto parsed = nlohmann::json::parse(json);
      if (!parsed.is_object() || parsed.value("format", "") != "tunareceive-backup") {
        return {};
      }
    } catch (const std::exception &) {
      return {};
    }
    std::string safe;
    for (unsigned char c : device) {
      safe.push_back(std::isalnum(c) ? (char) c : '-');
    }
    if (safe.empty()) {
      safe = "device";
    }
    const auto name = safe + "-" + std::to_string(now_seconds()) + ".json";
    if (!write_bytes(receiver_dir() / name, json)) {
      return {};
    }
    // Keep the newest few
    auto list = receiver_backups();
    std::error_code ec;
    for (size_t i = KEEP_RECEIVER_BACKUPS; i < list.size(); ++i) {
      fs::remove(receiver_dir() / list[i].name, ec);
    }
    BOOST_LOG(info) << "TunaReceive backup kept for "sv << device;
    return name;
  }

  std::vector<receiver_backup_t> receiver_backups() {
    std::vector<receiver_backup_t> out;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator {receiver_dir(), ec}) {
      try {
        const auto name = entry.path().filename().string();
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
          continue;
        }
        // <device>-<seconds>.json
        const auto dash = name.rfind('-');
        std::int64_t made = 0;
        std::string device = name;
        if (dash != std::string::npos) {
          device = name.substr(0, dash);
          try {
            made = std::stoll(name.substr(dash + 1));
          } catch (const std::exception &) {
          }
        }
        out.push_back({name, device, made, entry.file_size(ec)});
      } catch (const std::exception &) {
      }
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
      return a.made > b.made;
    });
    return out;
  }

  std::string receiver_backup(const std::string &name) {
    if (name.empty() || name.find_first_of("/\\") != std::string::npos || name.find("..") != std::string::npos) {
      return {};
    }
    std::string bytes;
    if (!read_bytes(receiver_dir() / name, bytes)) {
      return {};
    }
    return bytes;
  }
}  // namespace settings_backup
