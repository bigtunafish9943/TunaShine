/**
 * @file src/replay_mkv.cpp
 * @brief Writes an instant replay (encoded video and Opus sound) as a Matroska (.mkv) file.
 *
 * The encoded frames and sound packets are stored as they were sent to the device, with no
 * re-encoding. H.264 and HEVC frames are converted from Annex B (start codes) to the
 * length-prefixed form Matroska expects, and the codec setup (avcC, hvcC, av1C) is built from
 * the parameter sets in the first keyframe. The whole file is built in memory, which also lets
 * the seek table and the cue points be written up front.
 */
// standard includes
#include <algorithm>
#include <cstring>
#include <fstream>
#include <filesystem>

// local includes
#include "replay_mkv.h"

namespace replay_mkv {
  namespace {
    using bytes_t = std::vector<std::uint8_t>;

    // ---------- EBML ----------
    void put_id(bytes_t &out, std::uint32_t id) {
      if (id > 0xFFFFFF) {
        out.push_back((id >> 24) & 0xFF);
      }
      if (id > 0xFFFF) {
        out.push_back((id >> 16) & 0xFF);
      }
      if (id > 0xFF) {
        out.push_back((id >> 8) & 0xFF);
      }
      out.push_back(id & 0xFF);
    }

    /// Sizes are always written in eight bytes: simple, and lets sizes be known before contents
    void put_size(bytes_t &out, std::uint64_t size) {
      out.push_back(0x01);
      for (int i = 6; i >= 0; --i) {
        out.push_back((size >> (8 * i)) & 0xFF);
      }
    }

    void element(bytes_t &out, std::uint32_t id, const bytes_t &body) {
      put_id(out, id);
      put_size(out, body.size());
      out.insert(out.end(), body.begin(), body.end());
    }

    void uint_element(bytes_t &out, std::uint32_t id, std::uint64_t value) {
      bytes_t body;
      int bytes = 1;
      while (bytes < 8 && (value >> (8 * bytes)) != 0) {
        ++bytes;
      }
      for (int i = bytes - 1; i >= 0; --i) {
        body.push_back((value >> (8 * i)) & 0xFF);
      }
      element(out, id, body);
    }

    /// An unsigned integer always written in eight bytes, so its size doesn't depend on its value
    void uint8_element(bytes_t &out, std::uint32_t id, std::uint64_t value) {
      bytes_t body;
      for (int i = 7; i >= 0; --i) {
        body.push_back((value >> (8 * i)) & 0xFF);
      }
      element(out, id, body);
    }

    void float_element(bytes_t &out, std::uint32_t id, double value) {
      std::uint64_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      bytes_t body;
      for (int i = 7; i >= 0; --i) {
        body.push_back((bits >> (8 * i)) & 0xFF);
      }
      element(out, id, body);
    }

    void string_element(bytes_t &out, std::uint32_t id, const std::string &value) {
      element(out, id, bytes_t(value.begin(), value.end()));
    }

    // ---------- NAL units ----------
    struct nal_t {
      const std::uint8_t *data;
      size_t size;
    };

    /// The NAL units of an Annex B frame, without their start codes
    std::vector<nal_t> split_annex_b(const bytes_t &frame) {
      std::vector<nal_t> nals;
      const auto n = frame.size();
      size_t i = 0;
      size_t start = SIZE_MAX;
      while (i + 2 < n) {
        if (frame[i] == 0 && frame[i + 1] == 0 && frame[i + 2] == 1) {
          if (start != SIZE_MAX) {
            size_t end = i;
            // A four-byte start code's leading zero belongs to it, not to the NAL before
            while (end > start && frame[end - 1] == 0) {
              --end;
            }
            nals.push_back({frame.data() + start, end - start});
          }
          i += 3;
          start = i;
          continue;
        }
        ++i;
      }
      if (start != SIZE_MAX && start < n) {
        nals.push_back({frame.data() + start, n - start});
      }
      return nals;
    }

    bytes_t length_prefixed(const bytes_t &frame) {
      bytes_t out;
      out.reserve(frame.size() + 16);
      for (const auto &nal : split_annex_b(frame)) {
        const auto size = (std::uint32_t) nal.size;
        out.push_back(size >> 24);
        out.push_back((size >> 16) & 0xFF);
        out.push_back((size >> 8) & 0xFF);
        out.push_back(size & 0xFF);
        out.insert(out.end(), nal.data, nal.data + nal.size);
      }
      return out;
    }

    /// The NAL's payload with emulation prevention bytes (00 00 03) taken out
    bytes_t rbsp(const nal_t &nal) {
      bytes_t out;
      out.reserve(nal.size);
      int zeros = 0;
      for (size_t i = 0; i < nal.size; ++i) {
        const auto b = nal.data[i];
        if (zeros >= 2 && b == 3) {
          zeros = 0;
          continue;
        }
        zeros = b == 0 ? zeros + 1 : 0;
        out.push_back(b);
      }
      return out;
    }

    void put16(bytes_t &out, std::uint32_t v) {
      out.push_back((v >> 8) & 0xFF);
      out.push_back(v & 0xFF);
    }

    bytes_t avcc(const bytes_t &keyframe) {
      std::vector<nal_t> sps, pps;
      for (const auto &nal : split_annex_b(keyframe)) {
        if (nal.size < 4) {
          continue;
        }
        const int type = nal.data[0] & 0x1F;
        if (type == 7) {
          sps.push_back(nal);
        } else if (type == 8) {
          pps.push_back(nal);
        }
      }
      if (sps.empty() || pps.empty()) {
        return {};
      }
      bytes_t out {1, sps[0].data[1], sps[0].data[2], sps[0].data[3], 0xFF};
      out.push_back(0xE0 | (std::uint8_t) std::min<size_t>(sps.size(), 31));
      for (size_t i = 0; i < sps.size() && i < 31; ++i) {
        put16(out, (std::uint32_t) sps[i].size);
        out.insert(out.end(), sps[i].data, sps[i].data + sps[i].size);
      }
      out.push_back((std::uint8_t) std::min<size_t>(pps.size(), 255));
      for (size_t i = 0; i < pps.size() && i < 255; ++i) {
        put16(out, (std::uint32_t) pps[i].size);
        out.insert(out.end(), pps[i].data, pps[i].data + pps[i].size);
      }
      return out;
    }

    bytes_t hvcc(const bytes_t &keyframe, int bit_depth, bool yuv444) {
      std::vector<nal_t> vps, sps, pps;
      for (const auto &nal : split_annex_b(keyframe)) {
        if (nal.size < 3) {
          continue;
        }
        const int type = (nal.data[0] >> 1) & 0x3F;
        if (type == 32) {
          vps.push_back(nal);
        } else if (type == 33) {
          sps.push_back(nal);
        } else if (type == 34) {
          pps.push_back(nal);
        }
      }
      if (sps.empty()) {
        return {};
      }
      // The profile, tier and level are the twelve bytes after the SPS's first payload byte
      auto sps_rbsp = rbsp(sps[0]);
      if (sps_rbsp.size() < 15) {
        return {};
      }
      bytes_t out {1};
      out.insert(out.end(), sps_rbsp.begin() + 3, sps_rbsp.begin() + 15);
      out.push_back(0xF0);  // min_spatial_segmentation_idc = 0
      out.push_back(0x00);
      out.push_back(0xFC);  // parallelismType = 0
      out.push_back(0xFC | (yuv444 ? 3 : 1));
      out.push_back(0xF8 | (std::uint8_t) std::clamp(bit_depth - 8, 0, 7));
      out.push_back(0xF8 | (std::uint8_t) std::clamp(bit_depth - 8, 0, 7));
      put16(out, 0);  // avgFrameRate unknown
      out.push_back((1 << 3) | (1 << 2) | 3);  // one temporal layer, nested, four-byte lengths
      std::vector<std::pair<int, std::vector<nal_t> *>> arrays {{32, &vps}, {33, &sps}, {34, &pps}};
      std::uint8_t count = 0;
      for (auto &[type, list] : arrays) {
        count += list->empty() ? 0 : 1;
      }
      out.push_back(count);
      for (auto &[type, list] : arrays) {
        if (list->empty()) {
          continue;
        }
        out.push_back((std::uint8_t) type);  // not complete: parameter sets also come with the frames
        put16(out, (std::uint32_t) list->size());
        for (const auto &nal : *list) {
          put16(out, (std::uint32_t) nal.size);
          out.insert(out.end(), nal.data, nal.data + nal.size);
        }
      }
      return out;
    }

    // ---------- AV1 ----------
    struct obu_t {
      int type;
      size_t start;  // offset of the OBU header
      size_t end;
      size_t payload;  // offset of the payload
    };

    std::vector<obu_t> split_obus(const bytes_t &frame) {
      std::vector<obu_t> obus;
      size_t i = 0;
      while (i < frame.size()) {
        const auto header = frame[i];
        const int type = (header >> 3) & 0x0F;
        const bool extension = header & 0x04;
        const bool has_size = header & 0x02;
        size_t p = i + 1 + (extension ? 1 : 0);
        std::uint64_t size = 0;
        if (has_size) {
          for (int k = 0; k < 8 && p < frame.size(); ++k) {
            const auto b = frame[p++];
            size |= (std::uint64_t) (b & 0x7F) << (7 * k);
            if (!(b & 0x80)) {
              break;
            }
          }
        } else {
          size = frame.size() - p;
        }
        const size_t end = std::min(frame.size(), p + (size_t) size);
        obus.push_back({type, i, end, p});
        if (end <= i) {
          break;
        }
        i = end;
      }
      return obus;
    }

    /// AV1 frames go into Matroska without their temporal delimiters
    bytes_t av1_frame(const bytes_t &frame) {
      bytes_t out;
      out.reserve(frame.size());
      for (const auto &obu : split_obus(frame)) {
        if (obu.type != 2) {
          out.insert(out.end(), frame.begin() + obu.start, frame.begin() + obu.end);
        }
      }
      return out;
    }

    bytes_t av1c(const bytes_t &keyframe, int bit_depth, bool yuv444) {
      for (const auto &obu : split_obus(keyframe)) {
        if (obu.type != 1 || obu.payload >= obu.end) {
          continue;
        }
        const int profile = keyframe[obu.payload] >> 5;
        bytes_t out {0x81};
        out.push_back((std::uint8_t) (profile << 5));  // level unknown here; the sequence header follows
        out.push_back((bit_depth > 8 ? 0x40 : 0) | (yuv444 ? 0 : 0x0C));
        out.push_back(0x00);
        out.insert(out.end(), keyframe.begin() + obu.start, keyframe.begin() + obu.end);
        return out;
      }
      return {};
    }

    // ---------- Opus ----------
    bytes_t opus_head(const opus_layout_t &layout, std::uint16_t pre_skip) {
      bytes_t out {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, (std::uint8_t) layout.channels};
      out.push_back(pre_skip & 0xFF);
      out.push_back(pre_skip >> 8);
      const std::uint32_t rate = 48000;
      for (int i = 0; i < 4; ++i) {
        out.push_back((rate >> (8 * i)) & 0xFF);
      }
      out.push_back(0);  // output gain
      out.push_back(0);
      if (layout.channels <= 2 && layout.streams == 1) {
        out.push_back(0);  // mapping family 0: mono or stereo
        return out;
      }
      // Family 1 lists the channels in Vorbis order; the stream gives them in Windows order
      static const int from_windows_6[6] {0, 2, 1, 4, 5, 3};
      static const int from_windows_8[8] {0, 2, 1, 6, 7, 4, 5, 3};
      const int *order = layout.channels == 6 ? from_windows_6 : layout.channels == 8 ? from_windows_8 : nullptr;
      out.push_back(order ? 1 : 255);
      out.push_back((std::uint8_t) layout.streams);
      out.push_back((std::uint8_t) layout.coupled_streams);
      for (int i = 0; i < layout.channels; ++i) {
        out.push_back(layout.mapping[order ? order[i] : i]);
      }
      return out;
    }

    // ---------- IDs ----------
    enum : std::uint32_t {
      EBML = 0x1A45DFA3,
      EBML_VERSION = 0x4286,
      EBML_READ_VERSION = 0x42F7,
      EBML_MAX_ID_LENGTH = 0x42F2,
      EBML_MAX_SIZE_LENGTH = 0x42F3,
      DOC_TYPE = 0x4282,
      DOC_TYPE_VERSION = 0x4287,
      DOC_TYPE_READ_VERSION = 0x4285,
      SEGMENT = 0x18538067,
      SEEK_HEAD = 0x114D9B74,
      SEEK = 0x4DBB,
      SEEK_ID = 0x53AB,
      SEEK_POSITION = 0x53AC,
      INFO = 0x1549A966,
      TIMESTAMP_SCALE = 0x2AD7B1,
      DURATION = 0x4489,
      MUXING_APP = 0x4D80,
      WRITING_APP = 0x5741,
      TRACKS = 0x1654AE6B,
      TRACK_ENTRY = 0xAE,
      TRACK_NUMBER = 0xD7,
      TRACK_UID = 0x73C5,
      TRACK_TYPE = 0x83,
      FLAG_LACING = 0x9C,
      CODEC_ID = 0x86,
      CODEC_PRIVATE = 0x63A2,
      CODEC_DELAY = 0x56AA,
      SEEK_PRE_ROLL = 0x56BB,
      VIDEO = 0xE0,
      PIXEL_WIDTH = 0xB0,
      PIXEL_HEIGHT = 0xBA,
      COLOUR = 0x55B0,
      MATRIX_COEFFICIENTS = 0x55B1,
      RANGE = 0x55B9,
      TRANSFER_CHARACTERISTICS = 0x55BA,
      PRIMARIES = 0x55BB,
      AUDIO = 0xE1,
      SAMPLING_FREQUENCY = 0xB5,
      CHANNELS = 0x9F,
      CLUSTER = 0x1F43B675,
      TIMESTAMP = 0xE7,
      SIMPLE_BLOCK = 0xA3,
      CUES = 0x1C53BB6B,
      CUE_POINT = 0xBB,
      CUE_TIME = 0xB3,
      CUE_TRACK_POSITIONS = 0xB7,
      CUE_TRACK = 0xF7,
      CUE_CLUSTER_POSITION = 0xF1,
    };

    void simple_block(bytes_t &cluster, int track, std::int16_t relative_ms, bool keyframe, const bytes_t &data) {
      bytes_t body;
      body.reserve(data.size() + 4);
      body.push_back(0x80 | track);
      body.push_back((std::uint16_t) relative_ms >> 8);
      body.push_back((std::uint16_t) relative_ms & 0xFF);
      body.push_back(keyframe ? 0x80 : 0x00);
      body.insert(body.end(), data.begin(), data.end());
      element(cluster, SIMPLE_BLOCK, body);
    }
  }  // namespace

  std::vector<std::uint8_t> build(const clip_t &clip) {
    if (clip.video.empty() || !clip.video.front().keyframe) {
      return {};
    }
    const auto &first = clip.video.front();
    const auto start_us = first.time_us;
    const int bit_depth = clip.bit_depth;

    // The tracks
    bytes_t video_track;
    uint_element(video_track, TRACK_NUMBER, 1);
    uint_element(video_track, TRACK_UID, 0x7475AA01);
    uint_element(video_track, TRACK_TYPE, 1);
    uint_element(video_track, FLAG_LACING, 0);
    bytes_t codec_private;
    switch (clip.codec) {
      case codec_e::h264:
        string_element(video_track, CODEC_ID, "V_MPEG4/ISO/AVC");
        codec_private = avcc(first.data);
        break;
      case codec_e::hevc:
        string_element(video_track, CODEC_ID, "V_MPEGH/ISO/HEVC");
        codec_private = hvcc(first.data, bit_depth, clip.yuv444);
        break;
      case codec_e::av1:
        string_element(video_track, CODEC_ID, "V_AV1");
        codec_private = av1c(first.data, bit_depth, clip.yuv444);
        break;
    }
    if (codec_private.empty()) {
      return {};
    }
    element(video_track, CODEC_PRIVATE, codec_private);
    bytes_t video;
    uint_element(video, PIXEL_WIDTH, (std::uint64_t) std::max(clip.width, 1));
    uint_element(video, PIXEL_HEIGHT, (std::uint64_t) std::max(clip.height, 1));
    // Colour is only stated for HDR; otherwise players read it from the video itself
    if (clip.hdr) {
      bytes_t colour;
      uint_element(colour, MATRIX_COEFFICIENTS, 9);
      uint_element(colour, RANGE, 1);  // limited (TV) range
      uint_element(colour, TRANSFER_CHARACTERISTICS, 16);
      uint_element(colour, PRIMARIES, 9);
      element(video, COLOUR, colour);
    }
    element(video_track, VIDEO, video);

    bytes_t tracks;
    element(tracks, TRACK_ENTRY, video_track);
    const std::uint16_t pre_skip = 312;
    if (clip.has_audio && !clip.audio.empty()) {
      bytes_t audio_track;
      uint_element(audio_track, TRACK_NUMBER, 2);
      uint_element(audio_track, TRACK_UID, 0x7475AA02);
      uint_element(audio_track, TRACK_TYPE, 2);
      uint_element(audio_track, FLAG_LACING, 0);
      string_element(audio_track, CODEC_ID, "A_OPUS");
      element(audio_track, CODEC_PRIVATE, opus_head(clip.audio_layout, pre_skip));
      uint_element(audio_track, CODEC_DELAY, (std::uint64_t) pre_skip * 1000000000ull / 48000);
      uint_element(audio_track, SEEK_PRE_ROLL, 80000000);
      bytes_t audio;
      float_element(audio, SAMPLING_FREQUENCY, 48000.0);
      uint_element(audio, CHANNELS, (std::uint64_t) clip.audio_layout.channels);
      element(audio_track, AUDIO, audio);
      element(tracks, TRACK_ENTRY, audio_track);
    }

    // The clusters: a new one at each keyframe, or when a block's time no longer fits
    bytes_t clusters;
    std::vector<std::pair<std::int64_t, std::uint64_t>> cues;  // time in ms, offset into clusters
    std::int64_t last_ms = 0;
    {
      bytes_t cluster;
      std::int64_t cluster_ms = -1;
      auto flush = [&]() {
        if (cluster_ms < 0) {
          return;
        }
        element(clusters, CLUSTER, cluster);
        cluster.clear();
        cluster_ms = -1;
      };
      auto open = [&](std::int64_t ms) {
        flush();
        cluster_ms = ms;
        uint_element(cluster, TIMESTAMP, (std::uint64_t) ms);
      };

      size_t a = 0;
      auto add_audio_until = [&](std::int64_t until_us) {
        for (; a < clip.audio.size() && clip.audio[a].time_us <= until_us; ++a) {
          const auto &packet = clip.audio[a];
          if (packet.time_us < start_us) {
            continue;
          }
          const auto ms = (packet.time_us - start_us) / 1000;
          if (cluster_ms < 0 || ms - cluster_ms > 30000) {
            open(ms);
          }
          simple_block(cluster, 2, (std::int16_t) (ms - cluster_ms), true, packet.data);
          last_ms = std::max(last_ms, ms);
        }
      };

      for (const auto &frame : clip.video) {
        if (!clip.has_audio) {
          a = clip.audio.size();
        }
        add_audio_until(frame.time_us);
        const auto ms = std::max<std::int64_t>(0, (frame.time_us - start_us) / 1000);
        if (frame.keyframe || cluster_ms < 0 || ms - cluster_ms > 30000) {
          open(ms);
          if (frame.keyframe) {
            cues.emplace_back(ms, clusters.size());
          }
        }
        bytes_t data;
        switch (clip.codec) {
          case codec_e::h264:
          case codec_e::hevc:
            data = length_prefixed(frame.data);
            break;
          case codec_e::av1:
            data = av1_frame(frame.data);
            break;
        }
        simple_block(cluster, 1, (std::int16_t) (ms - cluster_ms), frame.keyframe, data);
        last_ms = std::max(last_ms, ms);
      }
      if (clip.has_audio) {
        add_audio_until(clip.video.back().time_us + 100000);
      }
      flush();
    }

    // Info
    bytes_t info;
    uint_element(info, TIMESTAMP_SCALE, 1000000);
    float_element(info, DURATION, (double) last_ms + 17.0);
    string_element(info, MUXING_APP, "TunaStream");
    string_element(info, WRITING_APP, "TunaStream instant replay");

    // The seek head has a fixed size, so the positions of what follows it are known first
    auto make_seek_head = [&](std::uint64_t info_pos, std::uint64_t tracks_pos, std::uint64_t cues_pos) {
      bytes_t head;
      for (auto [id, pos] : {std::pair<std::uint32_t, std::uint64_t> {INFO, info_pos}, {TRACKS, tracks_pos}, {CUES, cues_pos}}) {
        bytes_t seek;
        bytes_t id_bytes;
        put_id(id_bytes, id);
        element(seek, SEEK_ID, id_bytes);
        uint8_element(seek, SEEK_POSITION, pos);
        element(head, SEEK, seek);
      }
      bytes_t out;
      element(out, SEEK_HEAD, head);
      return out;
    };
    const auto seek_head_size = make_seek_head(0, 0, 0).size();
    bytes_t info_element, tracks_element;
    element(info_element, INFO, info);
    element(tracks_element, TRACKS, tracks);
    const std::uint64_t info_pos = seek_head_size;
    const std::uint64_t tracks_pos = info_pos + info_element.size();
    const std::uint64_t clusters_pos = tracks_pos + tracks_element.size();
    const std::uint64_t cues_pos = clusters_pos + clusters.size();

    bytes_t cue_body;
    for (const auto &[ms, offset] : cues) {
      bytes_t positions;
      uint_element(positions, CUE_TRACK, 1);
      uint_element(positions, CUE_CLUSTER_POSITION, clusters_pos + offset);
      bytes_t point;
      uint_element(point, CUE_TIME, (std::uint64_t) ms);
      element(point, CUE_TRACK_POSITIONS, positions);
      element(cue_body, CUE_POINT, point);
    }
    bytes_t cues_element;
    element(cues_element, CUES, cue_body);

    bytes_t segment = make_seek_head(info_pos, tracks_pos, cues_pos);
    segment.reserve(cues_pos + cues_element.size());
    segment.insert(segment.end(), info_element.begin(), info_element.end());
    segment.insert(segment.end(), tracks_element.begin(), tracks_element.end());
    segment.insert(segment.end(), clusters.begin(), clusters.end());
    segment.insert(segment.end(), cues_element.begin(), cues_element.end());

    bytes_t header;
    uint_element(header, EBML_VERSION, 1);
    uint_element(header, EBML_READ_VERSION, 1);
    uint_element(header, EBML_MAX_ID_LENGTH, 4);
    uint_element(header, EBML_MAX_SIZE_LENGTH, 8);
    string_element(header, DOC_TYPE, "matroska");
    uint_element(header, DOC_TYPE_VERSION, 4);
    uint_element(header, DOC_TYPE_READ_VERSION, 2);

    bytes_t file;
    file.reserve(segment.size() + 64);
    element(file, EBML, header);
    element(file, SEGMENT, segment);
    return file;
  }

  std::string write(const std::string &path_utf8, const clip_t &clip) {
    auto bytes = build(clip);
    if (bytes.empty()) {
      return "The replay had no complete picture to start from";
    }
    const std::filesystem::path path {std::u8string(path_utf8.begin(), path_utf8.end())};
    auto partial = path;
    partial += ".part";
    {
      std::ofstream out(partial, std::ios::binary | std::ios::trunc);
      out.write((const char *) bytes.data(), (std::streamsize) bytes.size());
      if (!out) {
        return "The file couldn't be written";
      }
    }
    std::error_code ec;
    std::filesystem::rename(partial, path, ec);
    if (ec) {
      std::filesystem::remove(partial, ec);
      return "The file couldn't be written";
    }
    return {};
  }
}  // namespace replay_mkv
