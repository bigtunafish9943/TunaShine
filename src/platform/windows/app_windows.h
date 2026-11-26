/**
 * @file src/platform/windows/app_windows.h
 * @brief The windows Share my screen can show on their own, with their display and program.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>

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
}  // namespace platf
