/**
 * @file src/library_sync.h
 * @brief Keeping the app list in step with the games on this PC.
 */
#pragma once

// lib includes
#include <nlohmann/json.hpp>

namespace library_sync {
  /// Start syncing in the background: shortly after start, then every 30 minutes.
  void start();

  /// The settings and the last sync, for the Apps page.
  nlohmann::json status();

  /// Change the settings: {"enabled", "days"}. Syncs straight away when turned on.
  void configure(const nlohmann::json &settings);

  /// Sync now (on a background thread). Skipped while someone streams.
  void run_soon();
}  // namespace library_sync
