/**
 * @file src/pairing_backup.h
 * @brief Declarations for keeping paired devices through reinstalls.
 */
#pragma once

namespace pairing_backup {
  /**
   * @brief Put back paired devices, the PC's pairing certificate and settings if they are missing.
   * @details Call after the configuration is read and before the certificate would be created,
   *          so a reinstall or a removed config folder doesn't force every device to pair again.
   */
  void restore_missing();

  /// Copy the current paired devices, certificate and settings to the backup.
  void save();
}  // namespace pairing_backup
