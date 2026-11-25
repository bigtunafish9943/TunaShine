/**
 * @file src/platform/windows/nvprefs/nvprefs_interface.h
 * @brief Declarations for nvidia preferences interface.
 */
#pragma once

// standard includes
#include <memory>

namespace nvprefs {

  class undo_data_t;

  class nvprefs_interface {
  public:
    nvprefs_interface();
    ~nvprefs_interface();

    bool load();

    void unload();

    bool restore_from_and_delete_undo_file_if_exists();

    bool modify_application_profile();

    bool modify_global_profile();

    bool owning_undo_file();

    bool restore_global_profile();

    /**
     * @brief Cap games at the stream's frame rate through the driver's global Max Frame Rate.
     * @details Undone by restore_global_profile(), or at the next start after a crash.
     */
    bool set_frame_limit(unsigned int fps);

    /// Set a global profile DWORD setting while streaming; restore_global_profile() puts it back.
    bool set_stream_setting(unsigned int id, unsigned int value);

  private:
    bool commit_global_profile_change(const undo_data_t &undo_data);

    struct impl;
    std::unique_ptr<impl> pimpl;
  };

}  // namespace nvprefs
