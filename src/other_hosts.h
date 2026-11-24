/**
 * @file src/other_hosts.h
 * @brief Declarations for noticing other streaming hosts on this PC and pausing them.
 */
#pragma once

// standard includes
#include <string>
#include <vector>

namespace other_hosts {
  /// Another streaming host installed as a Windows service.
  struct host_t {
    std::string name;  ///< Shown to the user, e.g. Sunshine
    std::string service;  ///< Windows service name
    bool running;  ///< True if the service is running now
  };

  /// Sunshine and Apollo services installed on this PC. Empty elsewhere.
  std::vector<host_t> installed();

  /**
   * @brief Stop or start one of the hosts listed by installed().
   * @param service Service name; only the known hosts are accepted.
   * @param run True to start it, false to stop it.
   * @return Empty on success, otherwise what went wrong.
   */
  std::string set_running(const std::string &service, bool run);
}  // namespace other_hosts
