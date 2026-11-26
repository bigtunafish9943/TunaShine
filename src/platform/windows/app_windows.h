/**
 * @file src/platform/windows/app_windows.h
 * @brief The windows Share my screen can show on their own, with their display and program.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace platf {
  /// Windows a person would pick (visible, titled, not TunaStream's or the taskbar's):
  /// [{"id", "title", "exe", "minimized"}].
  nlohmann::json shareable_windows();

  bool window_exists(std::uint64_t id);

  /// The program the window belongs to: whose sound goes with it.
  std::uint32_t window_process(std::uint64_t id);

  /// The display the window is (mostly) on, as \\.\DISPLAYn.
  std::string window_display(std::uint64_t id);

  std::string window_title(std::uint64_t id);

  /**
   * @brief The window in front at the PC, when it's full screen or belongs to a program in one of
   *        [game_folders]: {"id", "title", "path" (the program), "fullScreen"}; null otherwise.
   */
  nlohmann::json front_window(const std::vector<std::string> &game_folders);

  /// Moves the window onto the display (\\.\DISPLAYn), full screen if it was, and brings it to the front.
  bool move_window_to_display(std::uint64_t id, const std::string &display_name);
}  // namespace platf
