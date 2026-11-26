/**
 * @file src/play_history.h
 * @brief A record of each stream, for the weekly recap on the home page.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>

// lib includes
#include <nlohmann/json.hpp>

namespace play_history {
  /// One finished stream.
  struct stream_t {
    std::int64_t started = 0;  ///< When it started, in seconds since 1970 (UTC)
    int seconds = 0;  ///< How long it ran
    std::string device;  ///< Name of the paired device
    std::string app;  ///< The app that was streamed
    int target_fps = 0;  ///< Frame rate the device asked for
    double avg_fps = 0;  ///< Frames sent per second, averaged over its minutes
    int minutes = 0;  ///< Whole minutes that were measured
    int smooth_minutes = 0;  ///< Minutes with no lost video and few frames arriving late
    int lost_video = 0;  ///< Times the device asked for lost video to be repaired
    int slow_game_minutes = 0;  ///< Minutes the game itself drew fewer than 59 frames a second
    double slow_game_fps = 0;  ///< Its frame rate over those minutes
    int width = 0;  ///< Picture size the device asked for
    int height = 0;
    int players = 0;  ///< Most controllers playing at once
    bool phone_player = false;  ///< Someone joined with a phone as a controller
  };

  /// Add a finished stream. Streams shorter than a minute are left out.
  void record(const stream_t &stream);

  /**
   * @brief The last days of play, summed up for the home page.
   * @param days How many days back, today included.
   * @param utc_offset_minutes The viewer's offset from UTC, so days start at their midnight.
   */
  nlohmann::json recap(int days, int utc_offset_minutes);

  /// Seconds this device has streamed today (local time), from finished streams.
  std::int64_t seconds_today(const std::string &device);

  /// Milestones reached, newest first: [{"id", "at"}] (at: seconds since 1970).
  nlohmann::json milestones();

  /// recap() with the milestones added.
  nlohmann::json recap_with_milestones(int days, int utc_offset_minutes);
}  // namespace play_history
