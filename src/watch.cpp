/**
 * @file src/watch.cpp
 * @brief Watching a stream in a web browser.
 *
 * While a device streams, a browser can watch the same screen. A second encoder joins the shared
 * screen capture and encodes it small (1280x720 H.264, which every browser can decode), and a
 * second sound capture encodes stereo Opus. Both go to each viewer as a stream of short messages
 * over one long HTTP response; the page decodes them with WebCodecs.
 *
 * Message: 1 byte type ('I' info JSON, 'V' video access unit, 'A' Opus packet, 'E' ended),
 * 1 byte flags (1 = key frame), 8 bytes timestamp in microseconds, 4 bytes length, then the
 * payload. Numbers are little-endian.
 *
 * Watching starts with the first viewer, stops a little after the last one leaves, and stops at
 * once when a device's stream ends, so a viewer never sees the PC's own screen by accident.
 *
 * Share my screen: the owner can also choose to show the PC's own main screen while no device is
 * streaming (to let someone watch them play at the desk). It turns itself off after the time they
 * chose. When a device starts streaming, viewers move over to that stream, and back to the PC's
 * screen when it ends.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <thread>

// lib includes
#include <nlohmann/json.hpp>
#include <opus/opus.h>

// local includes
#include "audio.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "system_tray.h"
#include "video.h"
#include "watch.h"

#ifdef _WIN32
  #include "platform/windows/app_sound.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/virtual_display.h"
#endif

using namespace std::literals;

namespace watch {
  namespace {
    // Standard: 720p; Sharper (chosen on the Watch page): 1080p, for small text such as a game's
    struct quality_t {
      int width;
      int height;
      int kbps;
    };

    constexpr quality_t STANDARD {1280, 720, 6000};
    constexpr quality_t SHARPER {1920, 1080, 12000};
    constexpr std::size_t MAX_VIEWERS = 4;
    // A viewer this far behind is skipped ahead to the next key frame
    constexpr std::size_t MAX_QUEUED = 90;
    constexpr auto LINGER = 15s;

    char channel_marker;

    // Share my screen: on until this time (steady clock, microseconds), 0 when off
    std::atomic<std::int64_t> sharing_until {0};
    // A device is starting a stream: the PC's screen isn't shown for a moment
    std::atomic<std::int64_t> desktop_blocked_until {0};
    // The screen to share (\\.\DISPLAYn); empty for the main one. With state_lock.
    std::string shared_monitor;
    // 1080p instead of 720p. With state_lock.
    bool sharper = false;
    // Share my screen: a program whose sound viewers don't hear (voice chat), by key. With state_lock.
    std::string leave_out;

    struct sound_app_t {
      const char *key;
      const char *name;
      std::vector<std::string> exes;
    };

    const std::vector<sound_app_t> &sound_apps_list() {
      static const std::vector<sound_app_t> apps {
        {"discord", "Discord", {"Discord.exe", "DiscordPTB.exe", "DiscordCanary.exe"}},
        {"teamspeak", "TeamSpeak", {"ts3client_win64.exe", "ts3client_win32.exe", "TeamSpeak.exe"}},
        {"teams", "Microsoft Teams", {"ms-teams.exe", "Teams.exe"}},
        {"zoom", "Zoom", {"Zoom.exe"}},
        {"mumble", "Mumble", {"mumble.exe"}},
        {"slack", "Slack", {"slack.exe"}},
        {"guilded", "Guilded", {"Guilded.exe"}},
        {"spotify", "Spotify", {"Spotify.exe"}},
      };
      return apps;
    }
    bool settings_loaded = false;

    // The Watch page's choices, kept across restarts: watch.json next to the settings
    std::filesystem::path settings_file() {
      return platf::appdata() / "watch.json";
    }

    void load_settings_locked() {
      if (settings_loaded) {
        return;
      }
      settings_loaded = true;
      std::ifstream in {settings_file()};
      if (!in) {
        return;
      }
      try {
        auto j = nlohmann::json::parse(in);
        sharper = j.value("sharper", false);
        shared_monitor = j.value("monitor", std::string {});
        leave_out = j.value("leaveOut", std::string {});
      } catch (const std::exception &) {
      }
    }

    void save_settings_locked() {
      std::ofstream out {settings_file(), std::ios::trunc};
      out << nlohmann::json {{"sharper", sharper}, {"monitor", shared_monitor}, {"leaveOut", leave_out}}.dump(2);
    }

    std::int64_t now_us() {
      return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string message(char type, bool key, std::int64_t timestamp, std::string_view payload) {
      std::string out;
      out.resize(14 + payload.size());
      out[0] = type;
      out[1] = key ? 1 : 0;
      for (int i = 0; i < 8; ++i) {
        out[2 + i] = (char) ((std::uint64_t) timestamp >> (8 * i));
      }
      const auto size = (std::uint32_t) payload.size();
      for (int i = 0; i < 4; ++i) {
        out[10 + i] = (char) (size >> (8 * i));
      }
      std::copy(payload.begin(), payload.end(), out.begin() + 14);
      return out;
    }
  }  // namespace

  struct viewer_t {
    std::mutex lock;
    std::condition_variable cv;
    std::deque<std::string> queue;
    bool waiting_for_key = true;
    bool ended = false;

    // Who: shown on the Watch page, so the owner always knows who sees the screen
    std::string id;
    std::string name;
    std::string token_id;  ///< the watch key or token it uses; empty when signed in
    std::int64_t since = 0;  ///< seconds since 1970
  };

  namespace {
    bool sharing_now() {
      const auto until = sharing_until.load();
      return until != 0 && now_us() < until;
    }

    std::mutex state_lock;
    std::list<std::shared_ptr<viewer_t>> viewer_list;
    std::string info_message;

    // The running watch, if any
    struct run_t {
      safe::mail_t mail;
      std::thread video_thread;
      std::thread audio_thread;
      std::thread pump_thread;
      std::atomic_bool stopping {false};
      bool desktop = false;  ///< showing the PC's own screen (Share my screen), not a device's stream
    };

    std::shared_ptr<run_t> running;
    std::atomic<std::int64_t> last_viewer_left {0};

    void deliver(const std::string &msg, bool video, bool key) {
      std::lock_guard lg {state_lock};
      for (auto &v : viewer_list) {
        std::lock_guard vl {v->lock};
        if (v->ended) {
          continue;
        }
        if (video) {
          if (v->waiting_for_key && !key) {
            continue;
          }
          if (v->queue.size() >= MAX_QUEUED) {
            // Too far behind: drop what's queued and pick up again at the next key frame
            v->queue.clear();
            v->waiting_for_key = true;
            if (!key) {
              continue;
            }
          }
          if (key) {
            v->waiting_for_key = false;
          }
        } else if (v->waiting_for_key || v->queue.size() >= MAX_QUEUED) {
          continue;
        }
        v->queue.push_back(msg);
        v->cv.notify_one();
      }
    }

    std::string replaced(std::string_view payload, const std::vector<video::packet_raw_t::replace_t> &replacements) {
      std::string out {payload};
      for (auto &r : replacements) {
        auto pos = out.find(r.old);
        if (pos != std::string::npos) {
          out.replace(pos, r.old.size(), r._new);
        }
      }
      return out;
    }

    void pump(std::shared_ptr<run_t> run) {
      auto packets = run->mail->queue<video::packet_t>(mail::video_packets);
      auto idr = run->mail->event<bool>(mail::idr);
      std::int64_t last_key = 0;
      while (auto packet = packets->pop()) {
        if (run->stopping) {
          break;
        }
        const bool key = packet->is_idr();
        std::string_view payload {(const char *) packet->data(), packet->data_size()};
        std::string fixed;
        if (key && packet->replacements) {
          fixed = replaced(payload, *packet->replacements);
          payload = fixed;
        }
        const auto ts = packet->frame_timestamp ? std::chrono::duration_cast<std::chrono::microseconds>(packet->frame_timestamp->time_since_epoch()).count() : now_us();
        deliver(message('V', key, ts, payload), true, key);
        if (key) {
          last_key = now_us();
        } else if (now_us() - last_key > 10'000'000) {
          // A key frame now and then, so a viewer that fell behind doesn't wait long
          idr->raise(true);
          last_key = now_us();
        }
      }
    }

    // Takes the running watch out with the state lock held; finish() then stops it without the
    // lock, as the encoders take that lock to hand over what they made.
    std::shared_ptr<run_t> take_locked() {
      auto run = std::move(running);
      if (run) {
        run->stopping = true;
        run->mail->event<bool>(mail::shutdown)->raise(true);
        run->mail->queue<video::packet_t>(mail::video_packets)->stop();
      }
      return run;
    }

    void finish(std::shared_ptr<run_t> run) {
      if (!run) {
        return;
      }
      if (run->video_thread.joinable()) {
        run->video_thread.join();
      }
      if (run->audio_thread.joinable()) {
        run->audio_thread.join();
      }
      if (run->pump_thread.joinable()) {
        run->pump_thread.join();
      }
      if (run->desktop && !proc::proc.running()) {
        // The capture remembers the screen it used; nobody is streaming, so forget it
        proc::proc.display_name.clear();
      }
      BOOST_LOG(info) << "Stopped watching in the browser"sv;
    }

    void end_viewers(std::string_view why) {
      const auto msg = message('E', false, now_us(), why);
      for (auto &v : viewer_list) {
        std::lock_guard vl {v->lock};
        v->queue.clear();
        v->queue.push_back(msg);
        v->ended = true;
        v->cv.notify_one();
      }
      viewer_list.clear();
    }

#ifdef _WIN32
    /// The PC's sound minus one program's, as Opus for viewers, until stopping. false if it can't.
    bool sound_without_encoded(std::uint32_t pid, const std::atomic_bool &stopping) {
      int err = 0;
      OpusEncoder *enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
      if (!enc || err != OPUS_OK) {
        return false;
      }
      opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
      constexpr int FRAME = 480;  // 10 ms, as the usual watch sound
      std::vector<float> pending;
      std::vector<unsigned char> packet(4000);
      auto started = platf::sound_without(pid, [&](const float *samples, std::size_t frames) {
        pending.insert(pending.end(), samples, samples + frames * 2);
        std::size_t used = 0;
        while (pending.size() - used >= (std::size_t) FRAME * 2) {
          const int bytes = opus_encode_float(enc, pending.data() + used, FRAME, packet.data(), (opus_int32) packet.size());
          used += (std::size_t) FRAME * 2;
          if (bytes > 0) {
            on_audio(std::string_view {(const char *) packet.data(), (std::size_t) bytes});
          }
        }
        pending.erase(pending.begin(), pending.begin() + (std::ptrdiff_t) used);
      }, stopping);
      opus_encoder_destroy(enc);
      return started;
    }
#endif

    bool start_locked(std::string &error) {
      if (running) {
        return true;
      }
      load_settings_locked();
      const auto quality = sharper ? SHARPER : STANDARD;
      const bool desktop = rtsp_stream::session_count() == 0;
      if (desktop && !sharing_now()) {
        error = "Nobody is streaming right now. Watching starts when a device is playing.";
        return false;
      }
      if (desktop && now_us() < desktop_blocked_until.load()) {
        error = "A device is starting a stream. Watching follows it in a moment.";
        return false;
      }
      if (!video::parallel_encoding()) {
        error = "This PC's video encoder can't make a second picture at the same time. Watching needs a graphics card encoder.";
        return false;
      }

      auto run = std::make_shared<run_t>();
      run->mail = std::make_shared<safe::mail_raw_t>();
      run->desktop = desktop;
#ifdef _WIN32
      if (desktop && !proc::proc.running()) {
        // The screen chosen to share, if it's still there, or else the PC's main screen
        std::string chosen;
        for (const auto &m : monitors()) {
          if (m.value("id", "") == shared_monitor) {
            chosen = shared_monitor;
          }
        }
        if (chosen.empty()) {
          chosen = platf::to_utf8(VDISPLAY::getPrimaryDisplay());
        }
        if (!chosen.empty()) {
          proc::proc.display_name = chosen;
        }
      }
#endif

      video::config_t config {};
      config.width = quality.width;
      config.height = quality.height;
      config.framerate = 60;
      config.encodingFramerate = 60;
      config.bitrate = quality.kbps;
      config.slicesPerFrame = 1;
      config.numRefFrames = 1;
      config.encoderCscMode = 2;  // BT.709, limited range: what browsers expect
      config.videoFormat = 0;  // H.264
      config.dynamicRange = 0;
      config.chromaSamplingType = 0;
      config.enableIntraRefresh = 0;
      config.input_only = false;

      audio::config_t audio_config {};
      audio_config.packetDuration = 10;
      audio_config.channels = 2;
      audio_config.mask = 0x3;
      audio_config.input_only = false;

      auto mail = run->mail;
      run->video_thread = std::thread {[mail, config]() {
        video::capture(mail, config, channel());
      }};

      // Share my screen without a voice chat's sound: the PC's sound minus that program's
      std::uint32_t leave_out_pid = 0;
#ifdef _WIN32
      if (desktop && !leave_out.empty()) {
        for (const auto &app : sound_apps_list()) {
          if (app.key == leave_out) {
            leave_out_pid = platf::find_program(app.exes);
          }
        }
      }
#endif
      auto stopping = &run->stopping;
      run->audio_thread = std::thread {[mail, audio_config, leave_out_pid, stopping]() {
#ifdef _WIN32
        if (leave_out_pid && sound_without_encoded(leave_out_pid, *stopping)) {
          return;
        }
#endif
        audio::capture(mail, audio_config, channel());
      }};
      run->pump_thread = std::thread {pump, run};

      nlohmann::json details {{"width", quality.width}, {"height", quality.height}, {"codec", "h264"}, {"audio", "opus"}, {"sampleRate", 48000}, {"channels", 2}};
      info_message = message('I', false, now_us(), details.dump());
      running = std::move(run);
      BOOST_LOG(info) << "Watching in the browser: encoding "sv << quality.width << 'x' << quality.height << " H.264 for viewers"sv << (desktop ? " (this PC's screen)"sv : ""sv);
      return true;
    }

    // Stops the encoders a while after the last viewer left, or when nobody streams any more
    void linger(std::shared_ptr<run_t> mine) {
      while (true) {
        std::this_thread::sleep_for(1s);
        std::shared_ptr<run_t> done;
        {
          std::lock_guard lg {state_lock};
          viewer_list.remove_if([](const std::shared_ptr<viewer_t> &v) {
            std::lock_guard vl {v->lock};
            return v->ended;
          });
          if (running != mine) {
            return;  // stopped, or replaced by a newer watch with its own lingerer
          }
          if (viewer_list.empty() && now_us() - last_viewer_left > std::chrono::duration_cast<std::chrono::microseconds>(LINGER).count()) {
            done = take_locked();
          } else if (!mine->desktop && rtsp_stream::session_count() == 0) {
            end_viewers("The stream ended.");
            done = take_locked();
          } else if (mine->desktop && !sharing_now()) {
            end_viewers("Screen sharing stopped.");
            done = take_locked();
          }
        }
        if (done) {
          finish(std::move(done));
          return;
        }
      }
    }
  }  // namespace

  void *channel() {
    return &channel_marker;
  }

  void on_audio(std::string_view opus) {
    deliver(message('A', false, now_us(), opus), false, false);
  }

  std::shared_ptr<viewer_t> join(std::string &error, const std::string &name, const std::string &token_id) {
    std::lock_guard lg {state_lock};
    viewer_list.remove_if([](const std::shared_ptr<viewer_t> &v) {
      std::lock_guard vl {v->lock};
      return v->ended;
    });
    if (viewer_list.size() >= MAX_VIEWERS) {
      error = "Four browsers are already watching. Close one, then try again.";
      return nullptr;
    }
    const bool was_running = (bool) running;
    if (!start_locked(error)) {
      return nullptr;
    }
    if (!was_running) {
      std::thread {linger, running}.detach();
    }
    auto viewer = std::make_shared<viewer_t>();
    static std::atomic<int> next_id {1};
    viewer->id = std::to_string(next_id++);
    viewer->name = name;
    viewer->token_id = token_id;
    viewer->since = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    viewer->queue.push_back(info_message);
    viewer_list.push_back(viewer);
    // A key frame for the new viewer
    running->mail->event<bool>(mail::idr)->raise(true);
    BOOST_LOG(info) << "Started watching in a browser: "sv << name << " ("sv << viewer_list.size() << " now)"sv;
    return viewer;
  }

  bool next(viewer_t &viewer, std::string &out, std::chrono::milliseconds timeout) {
    std::unique_lock vl {viewer.lock};
    viewer.cv.wait_for(vl, timeout, [&]() {
      return !viewer.queue.empty() || viewer.ended;
    });
    if (!viewer.queue.empty()) {
      out = std::move(viewer.queue.front());
      viewer.queue.pop_front();
      return true;
    }
    out.clear();
    return !viewer.ended;
  }

  void leave(const std::shared_ptr<viewer_t> &viewer) {
    {
      std::lock_guard vl {viewer->lock};
      viewer->ended = true;
      viewer->queue.clear();
    }
    std::lock_guard lg {state_lock};
    viewer_list.remove(viewer);
    if (viewer_list.empty()) {
      last_viewer_left = now_us();
    }
  }

  void stop() {
    std::shared_ptr<run_t> done;
    {
      std::lock_guard lg {state_lock};
      if (!running) {
        return;
      }
      end_viewers("The stream ended.");
      done = take_locked();
    }
    finish(std::move(done));
  }

  int viewers() {
    std::lock_guard lg {state_lock};
    return (int) viewer_list.size();
  }

  nlohmann::json watching_now() {
    nlohmann::json out = nlohmann::json::array();
    std::lock_guard lg {state_lock};
    for (const auto &v : viewer_list) {
      std::lock_guard vl {v->lock};
      if (!v->ended) {
        out.push_back({{"id", v->id}, {"name", v->name}, {"since", v->since}, {"signedIn", v->token_id.empty()}});
      }
    }
    return out;
  }

  std::string remove_viewer(const std::string &id) {
    std::lock_guard lg {state_lock};
    for (auto it = viewer_list.begin(); it != viewer_list.end(); ++it) {
      auto v = *it;
      if (v->id != id) {
        continue;
      }
      const auto token_id = v->token_id;
      {
        std::lock_guard vl {v->lock};
        v->queue.clear();
        v->queue.push_back(message('E', false, now_us(), "You can't watch on this device any more."));
        v->ended = true;
        v->cv.notify_one();
      }
      viewer_list.erase(it);
      if (viewer_list.empty()) {
        last_viewer_left = now_us();
      }
      BOOST_LOG(info) << "Stopped a browser watching: "sv << v->name;
      return token_id;
    }
    return {};
  }

  nlohmann::json sound_apps() {
    nlohmann::json out = nlohmann::json::array();
    std::lock_guard lg {state_lock};
    load_settings_locked();
    for (const auto &app : sound_apps_list()) {
      bool running_now = false;
#ifdef _WIN32
      running_now = platf::find_program(app.exes) != 0;
#endif
      out.push_back({{"key", app.key}, {"name", app.name}, {"running", running_now}, {"leftOut", leave_out == app.key}});
    }
    return out;
  }

  void set_leave_out(const std::string &key) {
    std::shared_ptr<run_t> done;
    {
      std::lock_guard lg {state_lock};
      load_settings_locked();
      std::string valid;
      for (const auto &app : sound_apps_list()) {
        if (app.key == key) {
          valid = key;
        }
      }
      if (leave_out == valid) {
        return;
      }
      leave_out = valid;
      save_settings_locked();
      if (running && running->desktop) {
        end_viewers("Changing the sound.");
        done = take_locked();
      }
    }
    finish(std::move(done));
  }

  bool sharper_picture() {
    std::lock_guard lg {state_lock};
    load_settings_locked();
    return sharper;
  }

  void set_sharper_picture(bool on) {
    std::shared_ptr<run_t> done;
    {
      std::lock_guard lg {state_lock};
      load_settings_locked();
      if (sharper == on) {
        return;
      }
      sharper = on;
      save_settings_locked();
      // Viewers pick the new picture up when they reconnect, a few seconds later
      if (running) {
        end_viewers("Changing the picture.");
        done = take_locked();
      }
    }
    finish(std::move(done));
  }

  void stop_desktop() {
    desktop_blocked_until = now_us() + std::chrono::duration_cast<std::chrono::microseconds>(15s).count();
    std::shared_ptr<run_t> done;
    {
      std::lock_guard lg {state_lock};
      if (!running || !running->desktop) {
        return;
      }
      end_viewers("A device started streaming.");
      done = take_locked();
    }
    finish(std::move(done));
  }

  nlohmann::json monitors() {
    nlohmann::json out = nlohmann::json::array();
#ifdef _WIN32
    struct found_t {
      std::wstring device;
      RECT rect;
      bool primary;
    };

    std::vector<found_t> found;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
      MONITORINFOEXW info {};
      info.cbSize = sizeof(info);
      if (GetMonitorInfoW(monitor, &info)) {
        ((std::vector<found_t> *) param)->push_back({info.szDevice, info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
      }
      return TRUE;
    }, (LPARAM) &found);
    int n = 0;
    for (const auto &f : found) {
      // The monitor's own name ("DELL U2720Q"), when Windows knows it
      DISPLAY_DEVICEW dd {};
      dd.cb = sizeof(dd);
      std::string model;
      if (EnumDisplayDevicesW(f.device.c_str(), 0, &dd, 0)) {
        model = platf::to_utf8(dd.DeviceString);
      }
      if (proc::proc.running() && proc::proc.virtual_display && platf::to_utf8(f.device) == proc::proc.display_name) {
        continue;  // the TV's own screen while a game streams there
      }
      ++n;
      out.push_back({
        {"id", platf::to_utf8(f.device)},
        {"number", n},
        {"model", model},
        {"width", f.rect.right - f.rect.left},
        {"height", f.rect.bottom - f.rect.top},
        {"main", f.primary},
      });
    }
#endif
    return out;
  }

  void share_screen(bool on, int minutes, const std::string &monitor) {
    if (on) {
      minutes = std::clamp(minutes, 1, 24 * 60);
      // A different screen than viewers see now: they switch over
      std::shared_ptr<run_t> done;
      {
        std::lock_guard lg {state_lock};
        load_settings_locked();
        if (shared_monitor != monitor) {
          shared_monitor = monitor;
          save_settings_locked();
          if (running && running->desktop) {
            end_viewers("Switching screens.");
            done = take_locked();
          }
        }
      }
      finish(std::move(done));
      sharing_until = now_us() + (std::int64_t) minutes * 60'000'000;
      BOOST_LOG(info) << "Share my screen: on for "sv << minutes << " minutes"sv;
#if defined TUNASTREAM_TRAY && TUNASTREAM_TRAY >= 1
      system_tray::update_tray_sharing(true);
#endif
    } else {
      const bool was_on = sharing_now();
      sharing_until = 0;
      BOOST_LOG(info) << "Share my screen: off"sv;
#if defined TUNASTREAM_TRAY && TUNASTREAM_TRAY >= 1
      if (was_on) {
        system_tray::update_tray_sharing(false);
      }
#endif
      // Viewers of the PC's screen stop now, not at the next check
      std::shared_ptr<run_t> done;
      {
        std::lock_guard lg {state_lock};
        if (running && running->desktop) {
          end_viewers("Screen sharing stopped.");
          done = take_locked();
        }
      }
      finish(std::move(done));
    }
  }

  nlohmann::json sharing() {
    nlohmann::json out;
    const bool on = sharing_now();
    out["on"] = on;
    out["minutesLeft"] = on ? (int) ((sharing_until.load() - now_us() + 59'999'999) / 60'000'000) : 0;
    std::lock_guard lg {state_lock};
    out["showingScreen"] = running && running->desktop;
    out["viewers"] = (int) viewer_list.size();
    out["monitor"] = shared_monitor;
    return out;
  }
}  // namespace watch
