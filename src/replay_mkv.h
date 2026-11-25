/**
 * @file src/replay_mkv.h
 * @brief Writes an instant replay (encoded video and Opus sound) as a Matroska (.mkv) file.
 *
 * Kept free of the rest of TunaStream so it can be tested on its own.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <vector>

namespace replay_mkv {
  enum class codec_e {
    h264,
    hevc,
    av1,
  };

  struct video_frame_t {
    std::int64_t time_us;  ///< When the frame was captured
    bool keyframe;
    std::vector<std::uint8_t> data;  ///< As the encoder made it: Annex B for H.264 and HEVC, OBUs for AV1
  };

  struct audio_packet_t {
    std::int64_t time_us;
    std::vector<std::uint8_t> data;  ///< One Opus (multistream) packet
  };

  struct opus_layout_t {
    int channels = 2;
    int streams = 1;
    int coupled_streams = 1;
    std::uint8_t mapping[8] {0, 1, 2, 3, 4, 5, 6, 7};
  };

  struct clip_t {
    codec_e codec = codec_e::hevc;
    int width = 0;
    int height = 0;
    bool hdr = false;  ///< Video is BT.2020 with the PQ curve
    int bit_depth = 8;  ///< 8 or 10
    bool yuv444 = false;  ///< Full colour rather than 4:2:0
    std::vector<video_frame_t> video;  ///< In order, the first a keyframe
    bool has_audio = false;
    opus_layout_t audio_layout;
    std::vector<audio_packet_t> audio;  ///< In order
  };

  /**
   * @brief Write the clip to a file.
   * @return Empty on success, otherwise what went wrong.
   */
  std::string write(const std::string &path_utf8, const clip_t &clip);

  /// The bytes of the file write() would make (for tests).
  std::vector<std::uint8_t> build(const clip_t &clip);

  /**
   * @brief Read back a clip written by write(), with frames in the form write() takes.
   * @return False when the file isn't one TunaStream wrote.
   */
  bool read(const std::string &path_utf8, clip_t &clip);

  /**
   * @brief Join the last seconds of several clips into one, for the weekly highlights.
   * @param clips Clips of the same codec and picture size, oldest first.
   * @param seconds_each How much of the end of each to keep (from the keyframe before that point).
   */
  clip_t join(const std::vector<clip_t> &clips, int seconds_each);
}  // namespace replay_mkv
