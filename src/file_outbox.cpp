/**
 * @file src/file_outbox.cpp
 * @brief Files waiting to be sent from this PC to a paired device.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>

// local includes
#include "file_outbox.h"
#include "logging.h"
#include "platform/common.h"

using namespace std::literals;

namespace file_outbox {
  namespace fs = std::filesystem;

  namespace {
    std::mutex outbox_mutex;

    fs::path root() {
      return platf::appdata() / "outbox";
    }

    // Keeps the file's name readable but safe as a Windows file name
    std::string safe_name(const std::string &name) {
      // Only the last part, in case a path was sent
      auto slash = name.find_last_of("/\\");
      std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
      std::string out;
      for (unsigned char c : base) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ' ' || c == '(' || c == ')') {
          out += static_cast<char>(c);
        } else {
          out += '_';
        }
      }
      // No leading dots or spaces, no trailing dots or spaces (Windows drops them)
      while (!out.empty() && (out.front() == '.' || out.front() == ' ')) {
        out.erase(out.begin());
      }
      while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
      }
      if (out.size() > 120) {
        out = out.substr(out.size() - 120);
      }
      return out.empty() ? "file" : out;
    }

    // ids look like "1764012345678-name.ext"
    bool valid_id(const std::string &id) {
      auto dash = id.find('-');
      if (dash == std::string::npos || dash == 0 || id.size() > 160) {
        return false;
      }
      if (!std::all_of(id.begin(), id.begin() + dash, [](unsigned char c) { return std::isdigit(c); })) {
        return false;
      }
      return safe_name(id.substr(dash + 1)) == id.substr(dash + 1);
    }

    std::string name_from_id(const std::string &id) {
      return id.substr(id.find('-') + 1);
    }
  }  // namespace

  bool valid_uuid(const std::string &uuid) {
    return !uuid.empty() && uuid.size() <= 64 &&
           std::all_of(uuid.begin(), uuid.end(), [](unsigned char c) { return std::isalnum(c) || c == '-'; });
  }

  std::string add(const std::string &uuid, const std::string &name, std::istream &data, std::string &error) {
    if (!valid_uuid(uuid)) {
      error = "Unknown device";
      return {};
    }
    std::lock_guard lock(outbox_mutex);
    std::error_code ec;
    fs::path dir = root() / uuid;
    fs::create_directories(dir, ec);
    if (ec) {
      error = "Couldn't make the outbox folder: " + ec.message();
      return {};
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string id = std::to_string(ms) + "-" + safe_name(name);
    fs::path path = dir / id;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
      error = "Couldn't save the file";
      return {};
    }
    char buf[64 * 1024];
    std::uintmax_t total = 0;
    while (data.read(buf, sizeof(buf)) || data.gcount() > 0) {
      total += static_cast<std::uintmax_t>(data.gcount());
      if (total > max_file_size) {
        out.close();
        fs::remove(path, ec);
        error = "The file is bigger than 256 MB";
        return {};
      }
      out.write(buf, data.gcount());
    }
    out.close();
    if (!out) {
      fs::remove(path, ec);
      error = "Couldn't save the file";
      return {};
    }
    BOOST_LOG(info) << "File waiting for device "sv << uuid << ": "sv << id << " ("sv << total << " bytes)"sv;
    return id;
  }

  std::vector<entry_t> list(const std::string &uuid) {
    std::vector<entry_t> entries;
    if (!valid_uuid(uuid)) {
      return entries;
    }
    std::lock_guard lock(outbox_mutex);
    std::error_code ec;
    for (auto &file : fs::directory_iterator(root() / uuid, ec)) {
      if (!file.is_regular_file(ec)) {
        continue;
      }
      std::string id = file.path().filename().string();
      if (!valid_id(id)) {
        continue;
      }
      entries.push_back({id, name_from_id(id), file.file_size(ec)});
    }
    // The id starts with the time it was added
    std::sort(entries.begin(), entries.end(), [](const entry_t &a, const entry_t &b) {
      return std::stoull(a.id.substr(0, a.id.find('-'))) < std::stoull(b.id.substr(0, b.id.find('-')));
    });
    return entries;
  }

  fs::path path_of(const std::string &uuid, const std::string &id) {
    if (!valid_uuid(uuid) || !valid_id(id)) {
      return {};
    }
    std::lock_guard lock(outbox_mutex);
    std::error_code ec;
    fs::path path = root() / uuid / id;
    return fs::is_regular_file(path, ec) ? path : fs::path {};
  }

  bool remove(const std::string &uuid, const std::string &id) {
    if (!valid_uuid(uuid) || !valid_id(id)) {
      return false;
    }
    std::lock_guard lock(outbox_mutex);
    std::error_code ec;
    return fs::remove(root() / uuid / id, ec);
  }

  nlohmann::json all() {
    nlohmann::json out = nlohmann::json::object();
    std::error_code ec;
    std::vector<std::string> uuids;
    {
      std::lock_guard lock(outbox_mutex);
      for (auto &dir : fs::directory_iterator(root(), ec)) {
        if (dir.is_directory(ec) && valid_uuid(dir.path().filename().string())) {
          uuids.push_back(dir.path().filename().string());
        }
      }
    }
    for (auto &uuid : uuids) {
      nlohmann::json files = nlohmann::json::array();
      for (auto &entry : list(uuid)) {
        files.push_back({{"id", entry.id}, {"name", entry.name}, {"size", entry.size}});
      }
      if (!files.empty()) {
        out[uuid] = files;
      }
    }
    return out;
  }

  void clear(const std::string &uuid) {
    if (!valid_uuid(uuid)) {
      return;
    }
    std::lock_guard lock(outbox_mutex);
    std::error_code ec;
    fs::remove_all(root() / uuid, ec);
  }
}  // namespace file_outbox
