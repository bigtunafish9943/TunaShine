/**
 * @file src/platform/windows/user_helper.h
 * @brief Small jobs done by a copy of TunaStream running as the signed-in user (see user_helper.cpp).
 */
#pragma once

// standard includes
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

// platform includes
#include <windows.h>

namespace platf {
  /// A helper running as the user, and the pipe it sends on. Stopped when destroyed.
  struct user_helper_t {
    HANDLE process = nullptr;
    HANDLE pipe = INVALID_HANDLE_VALUE;

    ~user_helper_t();

    /// Read what the helper sent, up to size bytes. false when it stopped, or stop was set.
    bool read(void *buffer, std::size_t size, std::size_t &got, const std::atomic_bool &stop);
  };

  /// Start "tunastream.exe --<job>-helper <pipe> <args>" as the signed-in user; nullptr if it can't.
  std::unique_ptr<user_helper_t> start_user_helper(const std::string &job, const std::wstring &args);

  /// "tunastream.exe --sound-helper <pipe> <pid>": the PC's sound without that program's.
  int sound_helper_main(int argc, char *argv[]);
}  // namespace platf
