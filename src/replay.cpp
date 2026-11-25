/**
 * @file src/replay.cpp
 * @brief Instant replay: the last 30 seconds of a stream, kept so they can be saved as a clip.
 *
 * The frames and sound packets the stream sends are copied into memory as they go out. A clip
 * must start on a keyframe, and a stream normally has one only at its start, so the encoder is
 * asked for one every ten seconds while instant replay is on. The buffer keeps everything from
 * the newest keyframe that is at least 30 seconds old, so a saved clip is 30 to 40 seconds long.
 */
// standard includes
#include <algorithm>

// local includes
#include "replay.h"
#include "logging.h"

using namespace std::literals;

namespace replay {
  namespace {
    std::int64_t micros(std::chrono::steady_clock::time_point t) {
      return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
    }
  }  // namespace

  buffer_t::buffer_t(replay_mkv::codec_e codec, int width, int height, int bit_depth, bool yuv444, replay_mkv::opus_layout_t audio):
      codec(codec),
      width(width),
      height(height),
      bit_depth(bit_depth),
      yuv444(yuv444),
      audio_layout(audio) {
  }

  void buffer_t::add_video(std::chrono::steady_clock::time_point at, bool keyframe, const std::uint8_t *data, std::size_t size) {
    std::lock_guard lg {lock};
    // Nothing can be played before the first keyframe
    if (video.empty() && !keyframe) {
      return;
    }
    if (keyframe) {
      last_keyframe = at;
    }
    // Frames must go forward in time even when a repeated frame has no capture time of its own
    auto t = micros(at);
    if (!video.empty() && t <= video.back().time_us) {
      t = video.back().time_us + 1000;
    }
    video.push_back({t, keyframe, std::vector<std::uint8_t>(data, data + size)});
    bytes += size;
    prune();
  }

  void buffer_t::add_audio(std::chrono::steady_clock::time_point at, const std::uint8_t *data, std::size_t size) {
    std::lock_guard lg {lock};
    if (video.empty()) {
      return;
    }
    audio.push_back({micros(at), std::vector<std::uint8_t>(data, data + size)});
    bytes += size;
  }

  void buffer_t::prune() {
    const auto newest = video.back().time_us;
    const auto keep_us = std::chrono::duration_cast<std::chrono::microseconds>(KEEP).count();
    // Drop the oldest stretch from one keyframe to the next while the next one is old enough to
    // start a full clip, or while there is more than the memory allowed
    while (true) {
      auto next = std::find_if(video.begin() + 1, video.end(), [](const auto &f) {
        return f.keyframe;
      });
      if (next == video.end()) {
        break;
      }
      const bool old_enough = newest - next->time_us >= keep_us;
      if (!old_enough && bytes <= MAX_BYTES) {
        break;
      }
      for (auto it = video.begin(); it != next; ++it) {
        bytes -= it->data.size();
      }
      video.erase(video.begin(), next);
    }
    // Sound from before the first frame isn't needed
    const auto first = video.front().time_us - 100000;
    while (!audio.empty() && audio.front().time_us < first) {
      bytes -= audio.front().data.size();
      audio.pop_front();
    }
  }

  bool buffer_t::wants_keyframe(std::chrono::steady_clock::time_point now) {
    std::lock_guard lg {lock};
    if (now - last_keyframe < KEYFRAME_EVERY || now - last_keyframe_request < 2s) {
      return false;
    }
    last_keyframe_request = now;
    return true;
  }

  double buffer_t::seconds() const {
    std::lock_guard lg {lock};
    if (video.size() < 2) {
      return 0;
    }
    return (video.back().time_us - video.front().time_us) / 1e6;
  }

  std::string buffer_t::save(const std::string &path_utf8) {
    replay_mkv::clip_t clip;
    {
      std::lock_guard lg {lock};
      if (video.empty()) {
        return "Nothing has been streamed yet";
      }
      clip.codec = codec;
      clip.width = width;
      clip.height = height;
      clip.bit_depth = bit_depth;
      clip.yuv444 = yuv444;
      clip.video.assign(video.begin(), video.end());
      clip.has_audio = !audio.empty();
      clip.audio_layout = audio_layout;
      clip.audio.assign(audio.begin(), audio.end());
    }
    // Writing happens without the lock, so the stream keeps being recorded
    auto error = replay_mkv::write(path_utf8, clip);
    if (error.empty()) {
      BOOST_LOG(info) << "Replay saved: "sv << path_utf8 << " ("sv
                      << (clip.video.back().time_us - clip.video.front().time_us) / 1000000 << " s)"sv;
    }
    return error;
  }
}  // namespace replay
