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

// lib includes
#include <nlohmann/json.hpp>

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
   * @param name Who is watching, for the list on the Watch page.
   * @param token_id The watch key or token they use, empty when signed in with the password.
   */
  std::shared_ptr<viewer_t> join(std::string &error, const std::string &name, const std::string &token_id);

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

  /// A device is starting a stream: stop showing the PC's own screen, so viewers follow the stream.
  void stop_desktop();

  /**
   * @brief Share my screen: show one of the PC's own screens to viewers while nobody streams,
   *        for a while. monitor is an id from monitors(); empty for the main screen.
   */
  void share_screen(bool on, int minutes, const std::string &monitor = {});

  /// {"on", "minutesLeft", "showingScreen", "viewers", "monitor"} for the home and watch pages.
  nlohmann::json sharing();

  /// Who is watching now: [{"id", "name", "since", "signedIn"}].
  nlohmann::json watching_now();

  /// Stop one viewer now. Returns the id of the watch key or token it used (empty if signed in).
  std::string remove_viewer(const std::string &id);

  /// Voice chat and music programs whose sound Share my screen can leave out:
  /// [{"key", "name", "running", "leftOut"}].
  nlohmann::json sound_apps();

  /// Share my screen leaves this program's sound out (a key from sound_apps(); empty for none).
  void set_leave_out(const std::string &key);

  /**
   * @brief A viewer's cheer ("fire", "clap"...): shown to the other viewers, and on the PC's
   *        screen while sharing it or on the TV while a TV streams.
   * @return Empty when sent, otherwise why not, in a sentence for the page.
   */
  std::string cheer(const std::string &name, const std::string &kind);

  /// Whether viewers may send cheers (on unless turned off on the Watch page). Kept across restarts.
  bool cheers_allowed();
  void allow_cheers(bool on);

  /// The Watch page's picture: 1080p when true, 720p when false. Kept across restarts.
  bool sharper_picture();
  void set_sharper_picture(bool on);

  /// The PC's screens that can be shared: [{"id", "number", "model", "width", "height", "main"}].
  nlohmann::json monitors();
}  // namespace watch
