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
 * once when a device's stream ends, so a viewer never sees the PC's own screen.
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
#include "rtsp.h"
#include "video.h"
#include "watch.h"

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
      if (rtsp_stream::session_count() == 0) {
        error = "Nobody is streaming right now. Watching starts when a device is playing.";
        return false;
      }
      if (!video::parallel_encoding()) {
        error = "This PC's video encoder can't make a second picture at the same time. Watching needs a graphics card encoder.";
        return false;
      }

      auto run = std::make_shared<run_t>();
      run->mail = std::make_shared<safe::mail_raw_t>();

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
      BOOST_LOG(info) << "Watching in the browser: encoding "sv << WIDTH << 'x' << HEIGHT << " H.264 for viewers"sv;
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
          } else if (rtsp_stream::session_count() == 0) {
            end_viewers("The stream ended.");
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
}  // namespace watch
