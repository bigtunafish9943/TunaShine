/**
 * @file src/tv_messages.h
 * @brief Short messages typed on the home page and shown on a streaming device's screen.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tv_messages {
  struct message_t {
    std::uint32_t id;  ///< Increases with each message; the device uses it to show each one once
    std::string text;  ///< May be empty when only a sound was sent
    std::string sound;  ///< Name of a sound-board sound to play, or empty
  };

  /// Longest message, in characters (bytes of UTF-8)
  constexpr size_t MAX_LENGTH = 200;

  /**
   * @brief Queue a message for a device.
   * @param device The paired device's name, or empty for every device that is streaming.
   * @param text What to show. Trimmed, with line breaks kept and other control characters removed.
   * @param sound A sound-board sound to play with it, or empty.
   * @return The message's id, or 0 when there is neither text nor sound.
   */
  std::uint32_t post(const std::string &device, const std::string &text, const std::string &sound = {});

  /**
   * @brief The messages waiting for this device, which are then marked as delivered to it.
   * @details Messages are kept for two minutes; a device that doesn't ask in that time misses them.
   */
  std::vector<message_t> take(const std::string &device);

  /// Names of the devices each recent message has reached, by message id, for the home page.
  std::vector<std::pair<std::uint32_t, std::vector<std::string>>> delivered();
}  // namespace tv_messages
