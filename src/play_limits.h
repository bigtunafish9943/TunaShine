/**
 * @file src/play_limits.h
 * @brief Daily play-time limits for each device, with warnings on the TV before time runs out.
 */
#pragma once

// standard includes
#include <string>

// lib includes
#include <nlohmann/json.hpp>

namespace play_limits {
  /// Start watching the time of devices that are streaming. Call once at start-up.
  void start();

  /**
   * @brief Every paired device with its limits and today's time:
   *        [{"name", "weekday", "weekend", "usedSeconds", "leftSeconds", "extraMinutes"}].
   *        Limits are minutes, 0 for none; leftSeconds is -1 when there is no limit today.
   */
  nlohmann::json status();

  /// Set the limits: {"<device>": {"weekday": minutes, "weekend": minutes}, ...}
  void set_limits(const nlohmann::json &limits);

  /// Give a device more time today.
  void extend(const std::string &device, int minutes);

  /**
   * @brief Whether a device may start a stream now.
   * @param why Set to a sentence for the TV when it may not.
   */
  bool may_stream(const std::string &device, std::string &why);
}  // namespace play_limits
