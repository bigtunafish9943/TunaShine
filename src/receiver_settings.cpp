/**
 * @file src/receiver_settings.cpp
 * @brief TunaReceive's settings, seen and changed from TunaStream's Devices page.
 *
 * Each TV keeps a file in config/tunareceive_settings: what it last said its settings are, and the
 * changes made on the PC that it hasn't picked up yet, with a revision that goes up with each
 * change. The TV tells the revision it has applied, so a change is cleared only once it's in.
 */
// standard includes
#include <cctype>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>

// local includes
#include "logging.h"
#include "platform/common.h"
#include "receiver_settings.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace receiver_settings {
  namespace {
    std::mutex lock;

    fs::path folder() {
      return platf::appdata() / "tunareceive_settings";
    }

    /// A file name from the device id (letters, digits and dashes only)
    fs::path file_for(const std::string &uuid) {
      std::string safe;
      for (char c : uuid) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-') {
          safe += c;
        }
      }
      if (safe.empty()) {
        return {};
      }
      return folder() / (safe + ".json");
    }

    nlohmann::json load(const fs::path &path) {
      std::ifstream in {path};
      if (!in) {
        return nlohmann::json::object();
      }
      try {
        auto j = nlohmann::json::parse(in);
        return j.is_object() ? j : nlohmann::json::object();
      } catch (...) {
        return nlohmann::json::object();
      }
    }

    void save(const fs::path &path, const nlohmann::json &j) {
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      auto tmp = path;
      tmp += ".tmp";
      {
        std::ofstream out {tmp, std::ios::trunc};
        out << j.dump(1);
      }
      fs::rename(tmp, path, ec);
      if (ec) {
        BOOST_LOG(warning) << "TV settings: couldn't save "sv << path.string() << ": "sv << ec.message();
      }
    }

    std::int64_t now_s() {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /// Whether a value is allowed for a setting, as the TV described it
    bool allowed(const nlohmann::json &setting, const nlohmann::json &value) {
      const auto type = setting.value("type", "");
      if (type == "bool") {
        return value.is_boolean();
      }
      if (type == "kbps") {
        if (!value.is_number_integer()) {
          return false;
        }
        const auto v = value.get<long long>();
        return v >= setting.value("min", 500) && v <= setting.value("max", 300000);
      }
      if (type == "list") {
        if (!value.is_string()) {
          return false;
        }
        const auto choices = setting.value("choices", nlohmann::json::array());
        for (const auto &choice : choices) {
          if (choice.value("value", "") == value.get<std::string>()) {
            return true;
          }
        }
      }
      return false;
    }
  }  // namespace

  nlohmann::json report(const std::string &uuid, const std::string &device, const nlohmann::json &body) {
    std::lock_guard lg {lock};
    const auto path = file_for(uuid);
    nlohmann::json answer {{"revision", 0}, {"changes", nlohmann::json::object()}};
    if (path.empty() || !body.is_object()) {
      return answer;
    }
    auto state = load(path);
    const std::int64_t revision = state.value("revision", std::int64_t {0});
    const std::int64_t applied = body.value("applied", std::int64_t {0});
    if (applied >= revision) {
      state["pending"] = nlohmann::json::object();
    }
    state["device"] = device;
    state["seen"] = now_s();
    if (body.contains("settings") && body["settings"].is_array()) {
      state["settings"] = body["settings"];
    }
    state["profile"] = body.value("profile", "");
    state["version"] = body.value("version", "");
    state["revision"] = revision;
    save(path, state);

    if (applied < revision) {
      answer["revision"] = revision;
      answer["changes"] = state.value("pending", nlohmann::json::object());
    }
    return answer;
  }

  nlohmann::json list() {
    std::lock_guard lg {lock};
    nlohmann::json out = nlohmann::json::array();
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(folder(), ec)) {
      if (entry.path().extension() != ".json") {
        continue;
      }
      auto state = load(entry.path());
      if (!state.contains("settings")) {
        continue;
      }
      state["uuid"] = entry.path().stem().string();
      if (!state.contains("pending")) {
        state["pending"] = nlohmann::json::object();
      }
      out.push_back(std::move(state));
    }
    return out;
  }

  std::string change(const std::string &uuid, const nlohmann::json &changes) {
    std::lock_guard lg {lock};
    const auto path = file_for(uuid);
    if (path.empty() || !changes.is_object()) {
      return "Unknown device";
    }
    auto state = load(path);
    if (!state.contains("settings")) {
      return "This TV hasn't shared its settings yet. Open TunaReceive on it once.";
    }
    auto pending = state.value("pending", nlohmann::json::object());
    const auto settings = state["settings"];
    for (const auto &[key, value] : changes.items()) {
      if (value.is_null()) {
        pending.erase(key);
        continue;
      }
      const nlohmann::json *setting = nullptr;
      for (const auto &s : settings) {
        if (s.value("key", "") == key) {
          setting = &s;
          break;
        }
      }
      if (!setting) {
        return "Unknown setting: " + key;
      }
      if (!allowed(*setting, value)) {
        return "Not a choice for " + setting->value("title", key);
      }
      // Back to the TV's current value: nothing to change
      if (setting->contains("value") && (*setting)["value"] == value) {
        pending.erase(key);
      } else {
        pending[key] = value;
      }
    }
    state["pending"] = pending;
    // The time keeps revisions going up even if this file is lost and the TV remembers an old one
    state["revision"] = std::max(state.value("revision", std::int64_t {0}) + 1, now_s());
    save(path, state);
    BOOST_LOG(info) << "TV settings: "sv << pending.size() << " change(s) waiting for "sv << state.value("device", uuid);
    return {};
  }

  void forget(const std::string &uuid) {
    std::lock_guard lg {lock};
    const auto path = file_for(uuid);
    std::error_code ec;
    if (!path.empty()) {
      fs::remove(path, ec);
    }
  }
}  // namespace receiver_settings
