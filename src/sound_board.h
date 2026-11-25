/**
 * @file src/sound_board.h
 * @brief Short sounds that can be played on a streaming device's TV from the home page.
 */
#pragma once

// standard includes
#include <optional>
#include <string>
#include <vector>

namespace sound_board {
  struct sound_t {
    std::string name;  ///< Shown on the button, and used to ask for it
    bool built_in;  ///< Comes with TunaStream (can't be removed)
  };

  /// Longest sound file accepted, in bytes
  constexpr size_t MAX_BYTES = 2 * 1024 * 1024;

  /// Every sound: your own first, then the built-in ones, each in name order.
  std::vector<sound_t> list();

  /// The file of a sound, if there is one by that name.
  std::optional<std::string> file_for(const std::string &name);

  /// "audio/ogg", "audio/mpeg" or "audio/wav", from the file's name.
  std::string content_type_of(const std::string &path);

  /**
   * @brief Add one of your own sounds (OGG, MP3 or WAV).
   * @return Empty on success, otherwise why it was refused, in a sentence.
   */
  std::string add(const std::string &name, const std::string &extension, const std::string &bytes);

  /// Remove one of your own sounds. Built-in sounds can't be removed.
  bool remove(const std::string &name);
}  // namespace sound_board
