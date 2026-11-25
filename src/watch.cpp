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
#include <list>
#include <mutex>
#include <thread>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "audio.h"
#include "globals.h"
#include "logging.h"
#include "process.h"
#include "rtsp.h"
#include "system_tray.h"
#include "video.h"
#include "watch.h"

#ifdef _WIN32
  #include "platform/windows/misc.h"
  #include "platform/windows/virtual_display.h"
#endif

using namespace std::literals;

namespace watch {
  namespace {
    constexpr int WIDTH = 1280;
    constexpr int HEIGHT = 720;
    constexpr int BITRATE_KBPS = 6000;
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

    bool start_locked(std::string &error) {
      if (running) {
        return true;
      }
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
      config.width = WIDTH;
      config.height = HEIGHT;
      config.framerate = 60;
      config.encodingFramerate = 60;
      config.bitrate = BITRATE_KBPS;
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
      run->audio_thread = std::thread {[mail, audio_config]() {
        audio::capture(mail, audio_config, channel());
      }};
      run->pump_thread = std::thread {pump, run};

      nlohmann::json details {{"width", WIDTH}, {"height", HEIGHT}, {"codec", "h264"}, {"audio", "opus"}, {"sampleRate", 48000}, {"channels", 2}};
      info_message = message('I', false, now_us(), details.dump());
      running = std::move(run);
      BOOST_LOG(info) << "Watching in the browser: encoding "sv << WIDTH << 'x' << HEIGHT << " H.264 for viewers"sv << (desktop ? " (this PC's screen)"sv : ""sv);
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

  std::shared_ptr<viewer_t> join(std::string &error) {
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
    viewer->queue.push_back(info_message);
    viewer_list.push_back(viewer);
    // A key frame for the new viewer
    running->mail->event<bool>(mail::idr)->raise(true);
    BOOST_LOG(info) << "A browser started watching ("sv << viewer_list.size() << " now)"sv;
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
        if (shared_monitor != monitor) {
          shared_monitor = monitor;
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
