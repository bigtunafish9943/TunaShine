/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>

// local includes
#include "platform/common.h"
#include "thread_safe.h"
#include "crypto.h"

namespace input {
  struct input_t;

  void print(void *input);
  void reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data, const crypto::PERM& permission);

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  /**
   * @brief Create the input context for a streaming session.
   * @param device_name Name of the paired device, used when reporting its controllers.
   */
  std::shared_ptr<input_t> alloc(safe::mail_t mail, const std::string &device_name = {});

  /// A controller a streaming device has connected
  struct gamepad_status_t {
    std::string device;  ///< Name of the paired device it belongs to
    int controller;  ///< The device's own number for it, starting at 0
    std::string emulated;  ///< What games see it as, such as "Xbox 360"
    int battery = -1;  ///< Percent the device last reported, or -1 when it hasn't
    bool charging = false;  ///< Whether the device said it's charging
  };

  /// Controllers currently connected by streaming devices, in the order they connected.
  std::vector<gamepad_status_t> gamepad_status();

  /**
   * @brief The web page's Phone remote: the PC's mouse and keyboard from a phone, with or without
   *        a stream (a stuck Steam, a password to type, a pointer stuck in the middle).
   */
  namespace remote {
    void move(int dx, int dy);
    /// button: 1 left, 2 middle, 3 right
    void click(int button);
    void scroll(int amount);
    /// Text as typed, any language
    void type(const std::string &utf8);
    /// One of: enter, backspace, escape, tab, up, down, left, right, win, alt_tab, alt_f4
    bool key(const std::string &name);
    /// The pointer to the bottom-right corner, out of the game's way
    void pointer_aside();
  }  // namespace remote

  /**
   * @brief Phone controllers: a phone's browser as an Xbox controller on the PC, so friends can
   *        join a game with their phones. Each phone is one controller, up to four; one that
   *        stops answering for 20 seconds is unplugged.
   */
  namespace phone_pads {
    struct state_t {
      std::uint32_t buttons = 0;
      std::uint8_t lt = 0, rt = 0;
      std::int16_t lsX = 0, lsY = 0, rsX = 0, rsY = 0;
    };

    /**
     * @brief The phone [key]'s controller now looks like [state]; plugged in on first use.
     * @param rumble Set to how hard the game wants it to rumble, 0 to 1.
     * @param player Set to its player number among the phones, from 1.
     * @return Why it couldn't, or empty.
     */
    std::string update(const std::string &key, const std::string &name, const state_t &state, float &rumble, int &player);

    /// The phone left its page: its controller is unplugged.
    void leave(const std::string &key);
  }  // namespace phone_pads

  struct touch_port_t: public platf::touch_port_t {
    int env_width, env_height;

    // Offset x and y coordinates of the client
    float client_offsetX, client_offsetY;

    float scalar_inv;

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
