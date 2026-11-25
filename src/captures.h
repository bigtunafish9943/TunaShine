/**
 * @file src/captures.h
 * @brief Screenshots and clips from streams, saved in the signed-in user's Pictures and Videos folders.
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace captures {
  /// Largest screenshot accepted, in bytes
  constexpr size_t MAX_SCREENSHOT_BYTES = 40 * 1024 * 1024;

  /// Pictures\TunaStream (or TunaStream's own screenshots folder when nobody is signed in).
  std::filesystem::path screenshot_dir();

  /// Videos\TunaStream (or TunaStream's own clips folder when nobody is signed in).
  std::filesystem::path clip_dir();

  /**
   * @brief A file name for a new capture: "<app> 2026-11-25 21.03.15<extension>", made safe.
   */
  std::string file_name(const std::string &app, const std::string &extension);

  /**
   * @brief Save a screenshot sent by a device.
   * @return Where it was saved, or empty when it was refused (not a PNG or JPEG, or too large).
   */
  std::filesystem::path save_screenshot(const std::string &app, const std::string &bytes);

  struct capture_t {
    std::string name;  ///< File name
    std::string kind;  ///< "screenshot" or "clip"
    std::int64_t modified;  ///< Seconds since 1970
    std::uintmax_t size;
  };

  /// The newest screenshots and clips, newest first.
  std::vector<capture_t> recent(size_t at_most);

  /// The file of a screenshot or clip listed by recent(), if it is still there.
  std::filesystem::path file_for(const std::string &kind, const std::string &name);
}  // namespace captures
