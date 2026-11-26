/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <string_view>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  void print_status(const std::string_view &prefix, HRESULT status);
  HDESK syncThreadDesktop();

  int64_t qpc_counter();

  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Convert a UTF-8 string into a UTF-16 wide string.
   * @param string The UTF-8 string.
   * @return The converted UTF-16 wide string.
   */
  std::wstring from_utf8(const std::string_view &string);

  /**
   * @brief Convert a UTF-16 wide string into a UTF-8 string.
   * @param string The UTF-16 wide string.
   * @return The converted UTF-8 string.
   */
  std::string to_utf8(const std::wstring_view &string);

  /// Whether TunaStream runs as SYSTEM (as it does under its service).
  bool is_running_as_system();

  /// The signed-in user's token (elevated when asked and allowed), or nullptr. Close it after use.
  HANDLE retrieve_users_token(bool elevated);

  /// "tunastream.exe --wgc-helper ...": the screen capture helper started in the user's session.
  int wgc_helper_main(int argc, char *argv[]);

  /// Share my screen with one window: captures show only this window (0: whole displays again).
  void set_window_capture(std::uint64_t hwnd);
  std::uint64_t window_capture();
}  // namespace platf
