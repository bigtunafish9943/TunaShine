/**
 * @file src/game_library.h
 * @brief Declarations for finding games installed on this PC.
 */
#pragma once

// standard includes
#include <string>
#include <vector>

namespace game_library {
  /// A game found in one of the PC's game launchers, ready to add as an app.
  struct game_t {
    std::string source;  ///< steam, epic, gog, ea, ubisoft, amazon, store or program
    std::string id;  ///< The launcher's own id for the game
    std::string name;  ///< Name shown to the user
    std::string cmd;  ///< Command to run and track, when the game is started directly
    std::string detached;  ///< Command or URL that asks the launcher to start the game
    std::string working_dir;  ///< Folder to start cmd in
  };

  /**
   * @brief List the games installed through Steam, Epic, GOG, EA, Ubisoft Connect and Amazon
   *        Games, and everything in the Start menu, including Microsoft Store and Xbox games.
   * @details Reads each launcher's own records and the Start menu shortcuts; nothing is started
   *          or changed. Tools such as redistributables, runtimes and uninstallers are left out.
   *          Empty on platforms other than Windows.
   */
  std::vector<game_t> find_installed_games();
}  // namespace game_library
