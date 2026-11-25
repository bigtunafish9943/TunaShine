/**
 * @file src/receiver_settings.h
 * @brief TunaReceive's settings, seen and changed from TunaStream's Devices page.
 */
#pragma once

// standard includes
#include <string>

// lib includes
#include <nlohmann/json.hpp>

namespace receiver_settings {
  /**
   * @brief A TV tells its settings (values and choices) and gets back the changes made on the PC.
   * @param uuid The paired device's id.
   * @param device The paired device's name.
   * @param report {"applied": revision, "settings": [...], "profile", "version"}.
   * @return {"revision", "changes": {key: value}}; no changes when everything was applied.
   */
  nlohmann::json report(const std::string &uuid, const std::string &device, const nlohmann::json &report);

  /// Every TV that has told its settings: [{"uuid", "device", "seen", "settings", "pending", "profile", "version"}].
  nlohmann::json list();

  /**
   * @brief Change settings of a TV, applied the next time it checks in (within 15 seconds while its
   *        games are on screen). Values are checked against the TV's own choices.
   * @param changes {key: value}; a null value takes back a change that wasn't applied yet.
   * @return Empty, or why it was refused.
   */
  std::string change(const std::string &uuid, const nlohmann::json &changes);

  /// Forget a TV (when it is unpaired).
  void forget(const std::string &uuid);
}  // namespace receiver_settings
