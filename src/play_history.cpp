/**
 * @file src/play_history.cpp
 * @brief A record of each stream, for the weekly recap on the home page.
 *
 * Kept in play_history.json in TunaStream's settings folder: one entry per stream with when it
 * started, how long it ran, the device, the app and how smooth it was. Entries older than 120
 * days are dropped. Nothing leaves the PC.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// local includes
#include "logging.h"
#include "platform/common.h"
#include "play_history.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace play_history {
  namespace {
    std::mutex lock;
    constexpr std::int64_t KEEP_SECONDS = 120LL * 24 * 60 * 60;
    constexpr size_t KEEP_AT_MOST = 5000;

    fs::path file() {
      return platf::appdata() / "play_history.json";
    }

    nlohmann::json load() {
      std::ifstream in(file());
      if (!in) {
        return nlohmann::json::array();
      }
      try {
        auto data = nlohmann::json::parse(in);
        if (data.is_array()) {
          return data;
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Play history couldn't be read, starting a new one: "sv << e.what();
      }
      return nlohmann::json::array();
    }

    std::int64_t now_seconds() {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
  }  // namespace

  void record(const stream_t &s) {
    if (s.seconds < 60) {
      return;
    }
    std::lock_guard lg {lock};
    auto data = load();
    data.push_back({
      {"started", s.started},
      {"seconds", s.seconds},
      {"device", s.device},
      {"app", s.app},
      {"targetFps", s.target_fps},
      {"avgFps", std::round(s.avg_fps * 10) / 10},
      {"minutes", s.minutes},
      {"smoothMinutes", s.smooth_minutes},
      {"lostVideo", s.lost_video},
    });

    // Drop what is too old, and keep the file small
    const auto oldest = now_seconds() - KEEP_SECONDS;
    nlohmann::json kept = nlohmann::json::array();
    for (auto &entry : data) {
      if (entry.value("started", (std::int64_t) 0) >= oldest) {
        kept.push_back(std::move(entry));
      }
    }
    while (kept.size() > KEEP_AT_MOST) {
      kept.erase(kept.begin());
    }

    std::error_code ec;
    fs::create_directories(file().parent_path(), ec);
    const auto partial = fs::path {file()}.concat(".part");
    {
      std::ofstream out(partial, std::ios::trunc);
      out << kept.dump(1);
      if (!out) {
        BOOST_LOG(warning) << "Play history couldn't be saved"sv;
        return;
      }
    }
    fs::rename(partial, file(), ec);
    if (ec) {
      BOOST_LOG(warning) << "Play history couldn't be saved: "sv << ec.message();
    }
  }

  nlohmann::json recap(int days, int utc_offset_minutes) {
    days = std::clamp(days, 1, 90);
    utc_offset_minutes = std::clamp(utc_offset_minutes, -14 * 60, 14 * 60);
    const std::int64_t offset = utc_offset_minutes * 60LL;
    const std::int64_t day = 24 * 60 * 60;

    // Day numbers in the viewer's time zone; the recap covers today and the days before it
    const auto local_day = [&](std::int64_t t) {
      const auto local = t + offset;
      return local >= 0 ? local / day : (local - day + 1) / day;
    };
    const auto today = local_day(now_seconds());
    const auto first_day = today - days + 1;

    nlohmann::json data;
    {
      std::lock_guard lg {lock};
      data = load();
    }

    std::vector<std::int64_t> per_day(days, 0);
    std::map<std::string, std::int64_t> per_app;
    std::map<std::string, std::int64_t> per_device;
    std::int64_t total = 0, minutes = 0, smooth = 0, lost = 0, streams = 0, longest = 0;
    double fps_weighted = 0;
    std::string longest_app;
    for (const auto &entry : data) {
      const auto started = entry.value("started", (std::int64_t) 0);
      const auto d = local_day(started);
      if (d < first_day || d > today) {
        continue;
      }
      const auto seconds = (std::int64_t) entry.value("seconds", 0);
      per_day[d - first_day] += seconds;
      per_app[entry.value("app", "")] += seconds;
      per_device[entry.value("device", "")] += seconds;
      total += seconds;
      ++streams;
      const auto m = (std::int64_t) entry.value("minutes", 0);
      minutes += m;
      smooth += entry.value("smoothMinutes", 0);
      lost += entry.value("lostVideo", 0);
      fps_weighted += entry.value("avgFps", 0.0) * m;
      if (seconds > longest) {
        longest = seconds;
        longest_app = entry.value("app", "");
      }
    }

    std::vector<std::pair<std::string, std::int64_t>> apps(per_app.begin(), per_app.end());
    std::sort(apps.begin(), apps.end(), [](const auto &a, const auto &b) {
      return a.second > b.second;
    });
    nlohmann::json top = nlohmann::json::array();
    for (size_t i = 0; i < apps.size() && i < 5; ++i) {
      top.push_back({{"app", apps[i].first}, {"seconds", apps[i].second}});
    }
    nlohmann::json devices = nlohmann::json::array();
    for (const auto &[name, seconds] : per_device) {
      devices.push_back({{"device", name}, {"seconds", seconds}});
    }
    nlohmann::json by_day = nlohmann::json::array();
    for (int i = 0; i < days; ++i) {
      // The day's start, in UTC seconds, for the page to name the weekday
      by_day.push_back({{"start", (first_day + i) * day - offset}, {"seconds", per_day[i]}});
    }

    return {
      {"days", days},
      {"streams", streams},
      {"seconds", total},
      {"byDay", by_day},
      {"topApps", top},
      {"devices", devices},
      {"measuredMinutes", minutes},
      {"smoothMinutes", smooth},
      {"smoothPercent", minutes ? (int) std::lround(100.0 * smooth / minutes) : -1},
      {"avgFps", minutes ? std::round(fps_weighted / minutes * 10) / 10 : 0.0},
      {"lostVideo", lost},
      {"longestSeconds", longest},
      {"longestApp", longest_app},
    };
  }
}  // namespace play_history
