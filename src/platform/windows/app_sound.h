/**
 * @file src/platform/windows/app_sound.h
 * @brief The PC's sound without one program's (voice chat), for Share my screen.
 */
#pragma once

// standard includes
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace platf {
  /// The main process of a running program, by its .exe names, or 0 when it isn't running.
  std::uint32_t find_program(const std::vector<std::string> &exe_names);

  /**
   * @brief Capture everything the PC plays except this process and what it started, as 48 kHz
   *        stereo float, until stop is set.
   * @return false if it couldn't start (older Windows, or Windows refused): use the usual sound.
   */
  bool sound_without(std::uint32_t pid, const std::function<void(const float *samples, std::size_t frames)> &sink, const std::atomic_bool &stop);
}  // namespace platf
