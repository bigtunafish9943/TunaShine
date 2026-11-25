/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <string>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  struct session_t;

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    // The bitrate setting on the device, before room for error correction and audio was taken
    // out of it. Zero when the device didn't say.
    int configuredBitrateKbps = 0;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    std::string uuid(const session_t& session);

    /**
     * @brief Ask the stream's encoder for a different bitrate, without a restart or key frame.
     * @details kbps is in the units of the device's own bitrate setting, and is scaled to the
     *          encoder's share of it. Kept between a fifth of and the bitrate the stream started with.
     * @return The bitrate used, in the same units as kbps.
     */
    int set_bitrate(session_t &session, int kbps);

    /**
     * @brief Ask the stream's encoder for a smaller or the full picture size, mid-stream.
     * @details The game keeps drawing at the full size; only what is sent changes. The device
     *          stretches it back to the screen. The new size starts with a key frame, without a pause.
     * @return The percentage actually used: 100, 75, 67 or 50.
     */
    int set_picture_scale(session_t &session, int percent);

    bool uuid_match(const session_t& session, const std::string_view& uuid);
    bool update_device_info(session_t& session, const std::string& name, const crypto::PERM& newPerm);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void graceful_stop(session_t& session);
    void join(session_t &session);

    /**
     * @brief Rumble the controllers of every connected client for a moment.
     * @details Goes straight to the clients, bypassing games and the virtual gamepad driver,
     *          so it tells whether the host-to-controller path works.
     * @return Number of connected clients the rumble was sent to.
     */
    int test_rumble();

    /// What the device sees of its stream, as TunaReceive reports it.
    struct device_view_t {
      bool known = false;  ///< False until a report arrives, or when the last one is old
      double loss_pct = 0;  ///< Share of frames that never arrived
      double slow_pct = 0;  ///< Share of frames that took most of a frame interval to arrive
      double fps = 0;  ///< Frames received per second
      double decode_ms = 0;  ///< Average time to decode a frame
      int latency_ms = 0;  ///< Average delay on the device from receiving a frame to showing it
      int kbps = 0;  ///< Bitrate the device is asking for, in the units of its own setting
      int setting_kbps = 0;  ///< The device's bitrate setting
    };

    /// How smoothly one client's stream is running, measured over the last few seconds.
    struct health_t {
      std::string device;  ///< Name of the paired device
      int target_fps;  ///< Frame rate the client asked for
      double fps;  ///< New frames sent per second
      double skipped_per_sec;  ///< Game frames drawn but never sent, per second (the game outran the stream)
      int gaps;  ///< Times no new frame was sent for more than two frame intervals
      double pc_ms_avg;  ///< Average time from capture to sending, per frame
      double pc_ms_max;  ///< Longest time from capture to sending
      double send_ms_avg;  ///< Average time to put one frame on the network
      double send_ms_max;  ///< Longest time to put one frame on the network
      int recoveries;  ///< Times the client asked for lost video to be resent or repaired
      int bitrate_kbps;  ///< Bitrate the video is being sent at right now
      int fec_percent;  ///< Error correction being added to each frame right now
      int picture_scale;  ///< Picture size in percent of what the device asked for
      device_view_t view;  ///< What the device sees, when it reports it
      int send_rate_mbps;  ///< Rate packets are paced at
      double window_s;  ///< Seconds the figures cover
    };

    /// Health of every running stream.
    std::vector<health_t> health();

    /// Keep what the device reports it sees of its stream, for the health readout.
    void set_device_view(session_t &session, const device_view_t &view);

    state_e state(session_t &session);
    inline bool send(session_t& session, const std::string_view &payload);
  }  // namespace session
}  // namespace stream
