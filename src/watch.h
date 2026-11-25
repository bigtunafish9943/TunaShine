/**
 * @file src/watch.h
 * @brief Watching a stream in a web browser: a second, small encoding of the screen a device is
 *        streaming, with its sound, sent to signed-in viewers of the web page. Viewers can only
 *        watch; nothing they do reaches the PC.
 */
#pragma once

// standard includes
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace watch {
  /// The channel_data the watch encoders are started with, so their output comes here.
  void *channel();

  inline bool is_watch(void *channel_data) {
    return channel_data == channel();
  }

  /// A sound packet (Opus, stereo, 48 kHz) from the watch's sound encoder.
  void on_audio(std::string_view opus);

  struct viewer_t;

  /**
   * @brief Start watching. Starts the watch encoders when this is the first viewer.
   * @param error Why it couldn't start, in a sentence for the page, when it returns nullptr.
   */
  std::shared_ptr<viewer_t> join(std::string &error);

  /**
   * @brief The next message for this viewer, waiting up to timeout for one.
   * @return false when the viewer should stop: the stream ended, or the viewer was left behind.
   */
  bool next(viewer_t &viewer, std::string &message, std::chrono::milliseconds timeout);

  /// The viewer has gone. The encoders stop a little after the last one leaves.
  void leave(const std::shared_ptr<viewer_t> &viewer);

  /// Stop watching at once (a device's stream is ending, so the screen may change or go away).
  void stop();

  /// How many browsers are watching now.
  int viewers();
}  // namespace watch
