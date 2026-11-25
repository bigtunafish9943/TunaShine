/**
 * @file src/platform/windows/cheer_overlay.h
 * @brief Cheers from viewers, shown for a moment in the corner of the PC's shared screen.
 */
#pragma once

// standard includes
#include <string>

namespace platf {
  /// Show a cheer ("🔥", "Jake") on this screen (\\.\DISPLAYn; empty for the main one).
  void show_cheer(const std::string &monitor, const std::string &emoji, const std::string &text);
}  // namespace platf
