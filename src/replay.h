/**
 * @file src/replay.h
 * @brief Instant replay: the last 30 seconds of a stream, kept so they can be saved as a clip.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// local includes
#include "replay_mkv.h"

namespace replay {
  /// How much is kept: at least this much is always there to save once the stream has run that long
  constexpr auto KEEP = std::chrono::seconds(30);
  /// A clip has to start on a keyframe; one is asked for this often so the buffer stays short
  constexpr auto KEYFRAME_EVERY = std::chrono::seconds(10);
  /// Memory the buffer may use at most
  constexpr std::size_t MAX_BYTES = 900ull * 1024 * 1024;

  /// The encoded video and sound of one stream, as it was sent.
  class buffer_t {
  public:
    buffer_t(replay_mkv::codec_e codec, int width, int height, int bit_depth, bool yuv444, replay_mkv::opus_layout_t audio);

    void add_video(std::chrono::steady_clock::time_point at, bool keyframe, const std::uint8_t *data, std::size_t size);
    void add_audio(std::chrono::steady_clock::time_point at, const std::uint8_t *data, std::size_t size);

    /// True when it's time to ask the encoder for a keyframe (and then not again for a while).
    bool wants_keyframe(std::chrono::steady_clock::time_point now);

    /**
     * @brief Write what is kept (up to the last KEEP plus the time back to a keyframe) to a file.
     * @return Empty on success, otherwise what went wrong in a sentence.
     */
    std::string save(const std::string &path_utf8);

    /// Seconds of video kept right now.
    double seconds() const;

  private:
    void prune();

    mutable std::mutex lock;
    replay_mkv::codec_e codec;
    int width, height, bit_depth;
    bool yuv444;
    replay_mkv::opus_layout_t audio_layout;
    std::deque<replay_mkv::video_frame_t> video;
    std::deque<replay_mkv::audio_packet_t> audio;
    std::size_t bytes = 0;
    std::chrono::steady_clock::time_point last_keyframe;
    std::chrono::steady_clock::time_point last_keyframe_request;
  };
}  // namespace replay
