/**
 * @file src/updater.h
 * @brief Declarations for installing a newer TunaStream release from inside the app.
 */
#pragma once

// standard includes
#include <string>

namespace updater {
  /// Where an update is up to, as shown in the web interface
  struct state_t {
    std::string stage;  ///< idle, checking, downloading, installing, up_to_date or failed
    std::string version;  ///< Version being installed, if known
    std::string error;  ///< What went wrong, when stage is failed
  };

  /**
   * @brief Find the latest release, download its installer and run it in the background.
   * @details Returns at once; follow progress with state(). Only one update runs at a time.
   * @return False if an update is already running.
   */
  bool start();

  /// Current progress of the update started by start().
  state_t state();
}  // namespace updater
