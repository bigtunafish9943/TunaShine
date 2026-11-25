/**
 * @file src/platform/windows/nvprefs/undo_data.cpp
 * @brief Definitions for undoing changes to nvidia preferences.
 */
// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "nvprefs_common.h"
#include "undo_data.h"

using json = nlohmann::json;

// Separate namespace for ADL, otherwise we need to define json
// functions in the same namespace as our types
namespace nlohmann {
  using data_t = nvprefs::undo_data_t::data_t;
  using opengl_swapchain_t = data_t::opengl_swapchain_t;

  template<typename T>
  struct adl_serializer<std::optional<T>> {
    static void to_json(json &j, const std::optional<T> &opt) {
      if (opt == std::nullopt) {
        j = nullptr;
      } else {
        j = *opt;
      }
    }

    static void from_json(const json &j, std::optional<T> &opt) {
      if (j.is_null()) {
        opt = std::nullopt;
      } else {
        opt = j.template get<T>();
      }
    }
  };

  template<>
  struct adl_serializer<opengl_swapchain_t> {
    static void to_json(json &j, const opengl_swapchain_t &opengl_swapchain) {
      j = json {
        {"our_value", opengl_swapchain.our_value},
        {"undo_value", opengl_swapchain.undo_value}
      };
    }

    static void from_json(const json &j, opengl_swapchain_t &opengl_swapchain) {
      j.at("our_value").get_to(opengl_swapchain.our_value);
      j.at("undo_value").get_to(opengl_swapchain.undo_value);
    }
  };

  template<>
  struct adl_serializer<data_t> {
    static void to_json(json &j, const data_t &data) {
      json settings = json::object();
      for (const auto &[id, value] : data.stream_settings) {
        settings[std::to_string(id)] = value;
      }
      j = json {{"opengl_swapchain", data.opengl_swapchain}, {"frame_limit", data.frame_limit}, {"stream_settings", settings}};
    }

    static void from_json(const json &j, data_t &data) {
      j.at("opengl_swapchain").get_to(data.opengl_swapchain);
      // Undo files written before the frame limit existed don't have it
      if (j.contains("frame_limit")) {
        j.at("frame_limit").get_to(data.frame_limit);
      }
      if (j.contains("stream_settings") && j.at("stream_settings").is_object()) {
        for (const auto &[id, value] : j.at("stream_settings").items()) {
          data.stream_settings[(uint32_t) std::stoul(id)] = value.get<opengl_swapchain_t>();
        }
      }
    }
  };
}  // namespace nlohmann

namespace nvprefs {

  void undo_data_t::set_opengl_swapchain(uint32_t our_value, std::optional<uint32_t> undo_value) {
    data.opengl_swapchain = data_t::opengl_swapchain_t {
      our_value,
      undo_value
    };
  }

  std::optional<undo_data_t::data_t::opengl_swapchain_t> undo_data_t::get_opengl_swapchain() const {
    return data.opengl_swapchain;
  }

  void undo_data_t::set_frame_limit(uint32_t our_value, std::optional<uint32_t> undo_value) {
    data.frame_limit = data_t::opengl_swapchain_t {
      our_value,
      undo_value
    };
  }

  std::optional<undo_data_t::data_t::opengl_swapchain_t> undo_data_t::get_frame_limit() const {
    return data.frame_limit;
  }

  void undo_data_t::set_stream_setting(uint32_t id, uint32_t our_value, std::optional<uint32_t> undo_value) {
    data.stream_settings[id] = data_t::opengl_swapchain_t {our_value, undo_value};
  }

  const std::map<uint32_t, undo_data_t::data_t::opengl_swapchain_t> &undo_data_t::get_stream_settings() const {
    return data.stream_settings;
  }

  std::string undo_data_t::write() const {
    try {
      // Keep this assignment otherwise data will be treated as an array due to
      // initializer list shenanigangs.
      const json json_data = data;
      return json_data.dump();
    } catch (const std::exception &err) {
      error_message(std::string {"failed to serialize json data"});
      return {};
    }
  }

  void undo_data_t::read(const std::vector<char> &buffer) {
    try {
      data = json::parse(std::begin(buffer), std::end(buffer));
    } catch (const std::exception &err) {
      error_message(std::string {"failed to parse json data: "} + err.what());
      data = {};
    }
  }

  void undo_data_t::merge(const undo_data_t &newer_data) {
    const auto &swapchain_data = newer_data.get_opengl_swapchain();
    if (swapchain_data) {
      set_opengl_swapchain(swapchain_data->our_value, swapchain_data->undo_value);
    }

    // Keep the value from before our first change: a newer change by us must not become the undo value
    const auto &frame_limit_data = newer_data.get_frame_limit();
    if (frame_limit_data) {
      auto undo_value = data.frame_limit ? data.frame_limit->undo_value : frame_limit_data->undo_value;
      set_frame_limit(frame_limit_data->our_value, undo_value);
    }

    // Same for the other settings: the value from before our first change is the one to put back
    for (const auto &[id, value] : newer_data.get_stream_settings()) {
      auto existing = data.stream_settings.find(id);
      auto undo_value = existing != data.stream_settings.end() ? existing->second.undo_value : value.undo_value;
      set_stream_setting(id, value.our_value, undo_value);
    }
  }

}  // namespace nvprefs
