/**
 * @file src/box_art.h
 * @brief Cover art for apps that don't have their own picture, taken from Steam.
 */
#pragma once

// standard includes
#include <string>
#include <vector>

// local includes
#include "process.h"

namespace box_art {
  /**
   * @brief The cover for an app with no picture of its own, when it is a Steam game.
   * @details The game is recognised by a steam:// link in its commands, by its name matching
   *          a game installed through Steam, or (for games from other stores) by its name matching a
   *          game on the Steam store exactly. The cover comes from your own Steam library (a
   *          custom cover you set there first, then the one Steam shows), and otherwise from
   *          Steam's store servers; it is kept in the covers folder after that.
   * @param app The app.
   * @param allow_download Whether it may wait for a download (up to about ten seconds).
   * @return The path of the cover, or empty when there isn't one.
   */
  std::string cover_for(const proc::ctx_t &app, bool allow_download);

  /**
   * @brief Fetch the covers of these apps in the background, so they are ready when asked for.
   */
  void prefetch(const std::vector<proc::ctx_t> &apps);

  /// "image/jpeg" or "image/png", from the first bytes of a picture file.
  std::string content_type_of(const std::string &path);
}  // namespace box_art
