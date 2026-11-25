/**
 * @file src/platform/windows/nvprefs/undo_data.h
 * @brief Declarations for undoing changes to nvidia preferences.
 */
#pragma once

// standard includes
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace nvprefs {

  class undo_data_t {
  public:
    struct data_t {
      struct opengl_swapchain_t {
        uint32_t our_value;
        std::optional<uint32_t> undo_value;
      };

      std::optional<opengl_swapchain_t> opengl_swapchain;

      // Max Frame Rate in the global profile, set to the stream's frame rate while streaming
      std::optional<opengl_swapchain_t> frame_limit;

      // Other global profile settings changed while streaming (such as Vertical Sync), by setting ID
      std::map<uint32_t, opengl_swapchain_t> stream_settings;
    };

    void set_opengl_swapchain(uint32_t our_value, std::optional<uint32_t> undo_value);

    std::optional<data_t::opengl_swapchain_t> get_opengl_swapchain() const;

    void set_frame_limit(uint32_t our_value, std::optional<uint32_t> undo_value);

    std::optional<data_t::opengl_swapchain_t> get_frame_limit() const;

    void set_stream_setting(uint32_t id, uint32_t our_value, std::optional<uint32_t> undo_value);

    const std::map<uint32_t, data_t::opengl_swapchain_t> &get_stream_settings() const;

    std::string write() const;

    void read(const std::vector<char> &buffer);

    void merge(const undo_data_t &newer_data);

  private:
    data_t data;
  };

}  // namespace nvprefs
