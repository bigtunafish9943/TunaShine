/**
 * @file src/play_limits.cpp
 * @brief Daily play-time limits for each device, with warnings on the TV before time runs out.
 *
 * Limits are set per device in minutes, one for school days (Monday to Friday) and one for the
 * weekend, and kept in play_limits.json. Today's time is what the play history holds for today
 * plus the stream running now. The TV is warned at 10, 5 and 1 minute left; when the time is up
 * it is told so, and half a minute later the stream ends and the game is closed. A device that is
 * out of time can't start another stream today, unless more time is given on the home page.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

// local includes
#include "logging.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "play_history.h"
#include "play_limits.h"
#include "process.h"
#include "stream.h"
#include "tv_messages.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace play_limits {
  namespace {
    std::mutex lock;
    nlohmann::json data = nlohmann::json::object();  // {"devices": {...}, "extra": {...}}
    bool loaded = false;

    // Warnings already given today, per device: "10", "5", "1", "up" (guarded by state_lock)
    std::mutex state_lock;
    std::map<std::string, std::set<std::string>> warned;
    std::string warned_day;
    // When each device ran out, so the stream ends a little after the message
    std::map<std::string, std::chrono::steady_clock::time_point> ran_out;

    fs::path file() {
      return platf::appdata() / "play_limits.json";
    }

    std::tm local_now() {
      const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      std::tm local {};
#ifdef _WIN32
      localtime_s(&local, &now);
#else
      localtime_r(&now, &local);
#endif
      return local;
    }

    std::string today() {
      const auto t = local_now();
      char buf[16];
      std::strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
      return buf;
    }

    void load_locked() {
      if (loaded) {
        return;
      }
      loaded = true;
      std::ifstream in(file());
      if (!in) {
        return;
      }
      try {
        data = nlohmann::json::parse(in);
        if (!data.is_object()) {
          data = nlohmann::json::object();
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Play time limits couldn't be read: "sv << e.what();
        data = nlohmann::json::object();
      }
    }

    void save_locked() {
      std::error_code ec;
      fs::create_directories(file().parent_path(), ec);
      auto partial = file();
      partial += ".part";
      {
        std::ofstream out(partial, std::ios::trunc);
        out << data.dump(1);
        if (!out) {
          return;
        }
      }
      fs::rename(partial, file(), ec);
    }

    /// Minutes allowed today, 0 for no limit
    int limit_today_locked(const std::string &device) {
      const auto t = local_now();
      const bool weekend = t.tm_wday == 0 || t.tm_wday == 6;
      const auto devices = data.value("devices", nlohmann::json::object());
      if (!devices.contains(device)) {
        return 0;
      }
      return devices[device].value(weekend ? "weekend" : "weekday", 0);
    }

    int extra_today_locked(const std::string &device) {
      const auto extra = data.value("extra", nlohmann::json::object());
      if (!extra.contains(device) || extra[device].value("date", "") != today()) {
        return 0;
      }
      return extra[device].value("minutes", 0);
    }

    /// Seconds left today, or -1 when there is no limit
    std::int64_t left_seconds(const std::string &device, double running_s) {
      int limit, extra;
      {
        std::lock_guard lg {lock};
        load_locked();
        limit = limit_today_locked(device);
        extra = extra_today_locked(device);
      }
      if (limit <= 0) {
        return -1;
      }
      const auto used = play_history::seconds_today(device) + (std::int64_t) running_s;
      return std::max<std::int64_t>(0, (limit + extra) * 60LL - used);
    }

    std::string uuid_of(const std::string &device) {
      for (const auto &client : nvhttp::get_all_clients()) {
        if (client.value("name", "") == device) {
          return client.value("uuid", "");
        }
      }
      return {};
    }

    void check_once() {
      std::lock_guard state {state_lock};
      const auto day = today();
      if (day != warned_day) {
        warned.clear();
        ran_out.clear();
        warned_day = day;
      }
      for (const auto &h : stream::session::health()) {
        const auto left = left_seconds(h.device, h.running_s);
        if (left < 0) {
          continue;
        }
        auto &given = warned[h.device];
        auto warn = [&](const char *key, const std::string &text) {
          if (given.insert(key).second) {
            tv_messages::post(h.device, text, "Doorbell");
            BOOST_LOG(info) << "Play time for "sv << h.device << ": "sv << text;
          }
        };
        if (left <= 0) {
          warn("up", "Play time is up for today. The game closes in 30 seconds, so save now!");
          auto [it, first] = ran_out.try_emplace(h.device, std::chrono::steady_clock::now());
          if (std::chrono::steady_clock::now() - it->second >= 30s) {
            BOOST_LOG(info) << "Play time is up for "sv << h.device << "; ending the stream"sv;
            const auto uuid = uuid_of(h.device);
            if (!uuid.empty()) {
              nvhttp::find_and_stop_session(uuid, true);
            }
            proc::proc.terminate();
            ran_out.erase(h.device);
          }
        } else if (left <= 60) {
          warn("1", "1 minute of play left today. Time to save!");
        } else if (left <= 5 * 60) {
          warn("5", "5 minutes of play left today.");
        } else if (left <= 10 * 60) {
          warn("10", "10 minutes of play left today.");
        }
      }
    }
  }  // namespace

  void start() {
    std::thread([]() {
      while (true) {
        std::this_thread::sleep_for(15s);
        try {
          check_once();
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Play time check failed: "sv << e.what();
        }
      }
    }).detach();
  }

  nlohmann::json status() {
    std::map<std::string, double> running;
    for (const auto &h : stream::session::health()) {
      running[h.device] = h.running_s;
    }
    nlohmann::json out = nlohmann::json::array();
    std::set<std::string> names;
    for (const auto &client : nvhttp::get_all_clients()) {
      names.insert(client.value("name", ""));
    }
    std::lock_guard lg {lock};
    load_locked();
    const auto all_limits = data.value("devices", nlohmann::json::object());
    for (const auto &[name, limits] : all_limits.items()) {
      names.insert(name);
    }
    for (const auto &name : names) {
      if (name.empty()) {
        continue;
      }
      const auto devices = data.value("devices", nlohmann::json::object());
      const auto limits = devices.contains(name) ? devices[name] : nlohmann::json::object();
      const double run = running.count(name) ? running.at(name) : 0;
      const auto used = play_history::seconds_today(name) + (std::int64_t) run;
      const int limit = limit_today_locked(name);
      const int extra = extra_today_locked(name);
      out.push_back({
        {"name", name},
        {"weekday", limits.value("weekday", 0)},
        {"weekend", limits.value("weekend", 0)},
        {"usedSeconds", used},
        {"extraMinutes", extra},
        {"leftSeconds", limit > 0 ? std::max<std::int64_t>(0, (limit + extra) * 60LL - used) : -1},
        {"streaming", running.count(name) > 0},
      });
    }
    return out;
  }

  void set_limits(const nlohmann::json &limits) {
    std::lock_guard lg {lock};
    load_locked();
    nlohmann::json devices = nlohmann::json::object();
    for (const auto &[name, value] : limits.items()) {
      const int weekday = std::clamp(value.value("weekday", 0), 0, 24 * 60);
      const int weekend = std::clamp(value.value("weekend", 0), 0, 24 * 60);
      if (weekday > 0 || weekend > 0) {
        devices[name] = {{"weekday", weekday}, {"weekend", weekend}};
      }
    }
    data["devices"] = devices;
    save_locked();
    BOOST_LOG(info) << "Play time limits saved for "sv << devices.size() << " device(s)"sv;
  }

  void extend(const std::string &device, int minutes) {
    minutes = std::clamp(minutes, 1, 240);
    {
      std::lock_guard lg {lock};
      load_locked();
      if (!data.contains("extra") || !data["extra"].is_object()) {
        data["extra"] = nlohmann::json::object();
      }
      const int before = extra_today_locked(device);
      data["extra"][device] = {{"date", today()}, {"minutes", before + minutes}};
      save_locked();
    }
    {
      // Warnings can be given again with the new time
      std::lock_guard state {state_lock};
      warned.erase(device);
      ran_out.erase(device);
    }
    tv_messages::post(device, std::to_string(minutes) + " more minutes of play today!", "Level up");
    BOOST_LOG(info) << "Play time for "sv << device << ": "sv << minutes << " more minutes today"sv;
  }

  bool may_stream(const std::string &device, std::string &why) {
    if (left_seconds(device, 0) == 0) {
      why = "Play time is up for today.";
      return false;
    }
    return true;
  }
}  // namespace play_limits
