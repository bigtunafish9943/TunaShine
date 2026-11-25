/**
 * @file src/settings_backup.h
 * @brief Backup and restore of TunaStream's settings as one file, and keeping TunaReceive's backups.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace settings_backup {
  /**
   * @brief Everything worth keeping, as one JSON document: settings, apps, sounds, play history
   *        and milestones, and (when asked) paired devices, the PC's pairing certificate and the
   *        web page's sign-in.
   */
  nlohmann::json make(bool with_pairings);

  /**
   * @brief Put a backup made by make() back. TunaStream needs restarting afterwards.
   * @return Empty on success, otherwise why it was refused, in a sentence.
   */
  std::string restore(const nlohmann::json &backup);

  // ---------- TunaReceive's backups, kept on the PC ----------

  /// Keep a backup TunaReceive sent. @return the name it was saved under, or empty when refused.
  std::string keep_receiver_backup(const std::string &device, const std::string &json);

  struct receiver_backup_t {
    std::string name;
    std::string device;
    std::int64_t made;  ///< Seconds since 1970
    std::uintmax_t size;
  };

  /// TunaReceive backups on this PC, newest first.
  std::vector<receiver_backup_t> receiver_backups();

  /// One TunaReceive backup by name, or empty when there is none.
  std::string receiver_backup(const std::string &name);
}  // namespace settings_backup
