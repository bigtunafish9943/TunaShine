/**
 * @file src/platform/windows/nvprefs/driver_settings.cpp
 * @brief Definitions for nvidia driver settings.
 */
// this include
#include "driver_settings.h"

// standard includes
#include <algorithm>
#include <string>

// local includes
#include "nvprefs_common.h"

namespace {

  const auto tunastream_application_profile_name = L"TunaStreamStream";
  const auto tunastream_application_path = L"tunastream.exe";

  void nvapi_error_message(NvAPI_Status status) {
    NvAPI_ShortString message = {};
    NvAPI_GetErrorMessage(status, message);
    nvprefs::error_message(std::string("NvAPI error: ") + message);
  }

  void fill_nvapi_string(NvAPI_UnicodeString &dest, const wchar_t *src) {
    static_assert(sizeof(NvU16) == sizeof(wchar_t));
    memcpy_s(dest, NVAPI_UNICODE_STRING_MAX * sizeof(NvU16), src, (wcslen(src) + 1) * sizeof(wchar_t));
  }

}  // namespace

namespace nvprefs {

  driver_settings_t::~driver_settings_t() {
    if (session_handle) {
      NvAPI_DRS_DestroySession(session_handle);
    }
  }

  bool driver_settings_t::init() {
    if (session_handle) {
      return true;
    }

    NvAPI_Status status;

    status = NvAPI_Initialize();
    if (status != NVAPI_OK) {
      info_message("NvAPI_Initialize() failed, ignore if you don't have NVIDIA video card");
      return false;
    }

    status = NvAPI_DRS_CreateSession(&session_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_CreateSession() failed");
      return false;
    }

    return load_settings();
  }

  void driver_settings_t::destroy() {
    if (session_handle) {
      NvAPI_DRS_DestroySession(session_handle);
      session_handle = nullptr;
    }
    NvAPI_Unload();
  }

  bool driver_settings_t::load_settings() {
    if (!session_handle) {
      return false;
    }

    NvAPI_Status status = NvAPI_DRS_LoadSettings(session_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_LoadSettings() failed");
      destroy();
      return false;
    }

    return true;
  }

  bool driver_settings_t::save_settings() {
    if (!session_handle) {
      return false;
    }

    NvAPI_Status status = NvAPI_DRS_SaveSettings(session_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_SaveSettings() failed");
      return false;
    }

    return true;
  }

  bool driver_settings_t::restore_global_profile_to_undo(const undo_data_t &undo_data) {
    if (!session_handle) {
      return false;
    }

    const auto &swapchain_data = undo_data.get_opengl_swapchain();
    if (swapchain_data) {
      NvAPI_Status status;

      NvDRSProfileHandle profile_handle = nullptr;
      status = NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_GetBaseProfile() failed");
        return false;
      }

      NVDRS_SETTING setting = {};
      setting.version = NVDRS_SETTING_VER;
      status = NvAPI_DRS_GetSetting(session_handle, profile_handle, OGL_CPL_PREFER_DXPRESENT_ID, &setting);

      if (status == NVAPI_OK && setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && setting.u32CurrentValue == swapchain_data->our_value) {
        if (swapchain_data->undo_value) {
          setting = {};
          setting.version = NVDRS_SETTING_VER1;
          setting.settingId = OGL_CPL_PREFER_DXPRESENT_ID;
          setting.settingType = NVDRS_DWORD_TYPE;
          setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
          setting.u32CurrentValue = *swapchain_data->undo_value;

          status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);

          if (status != NVAPI_OK) {
            nvapi_error_message(status);
            error_message("NvAPI_DRS_SetSetting() OGL_CPL_PREFER_DXPRESENT failed");
            return false;
          }
        } else {
          status = NvAPI_DRS_DeleteProfileSetting(session_handle, profile_handle, OGL_CPL_PREFER_DXPRESENT_ID);

          if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
            nvapi_error_message(status);
            error_message("NvAPI_DRS_DeleteProfileSetting() OGL_CPL_PREFER_DXPRESENT failed");
            return false;
          }
        }

        info_message("Restored OGL_CPL_PREFER_DXPRESENT for base profile");
      } else if (status == NVAPI_OK || status == NVAPI_SETTING_NOT_FOUND) {
        info_message("OGL_CPL_PREFER_DXPRESENT has been changed from our value in base profile, not restoring");
      } else {
        error_message("NvAPI_DRS_GetSetting() OGL_CPL_PREFER_DXPRESENT failed");
        return false;
      }
    }

    const auto &frame_limit_data = undo_data.get_frame_limit();
    if (frame_limit_data) {
      NvDRSProfileHandle profile_handle = nullptr;
      auto status = NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_GetBaseProfile() failed");
        return false;
      }

      NVDRS_SETTING setting = {};
      setting.version = NVDRS_SETTING_VER;
      status = NvAPI_DRS_GetSetting(session_handle, profile_handle, FRL_FPS_ID, &setting);

      // Only put it back if it is still our value; if the user changed it since, theirs stays
      if (status == NVAPI_OK && setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && setting.u32CurrentValue == frame_limit_data->our_value) {
        if (frame_limit_data->undo_value) {
          setting = {};
          setting.version = NVDRS_SETTING_VER1;
          setting.settingId = FRL_FPS_ID;
          setting.settingType = NVDRS_DWORD_TYPE;
          setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
          setting.u32CurrentValue = *frame_limit_data->undo_value;
          status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);
        } else {
          status = NvAPI_DRS_DeleteProfileSetting(session_handle, profile_handle, FRL_FPS_ID);
          if (status == NVAPI_SETTING_NOT_FOUND) {
            status = NVAPI_OK;
          }
        }

        if (status != NVAPI_OK) {
          nvapi_error_message(status);
          error_message("Couldn't restore Max Frame Rate in the global profile");
          return false;
        }
        info_message("Restored Max Frame Rate in the global profile");
      } else if (status == NVAPI_OK || status == NVAPI_SETTING_NOT_FOUND) {
        info_message("Max Frame Rate has been changed from our value in the global profile, not restoring");
      } else {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_GetSetting() FRL_FPS failed");
        return false;
      }
    }

    // The other settings changed while streaming: put back each one still at our value
    for (const auto &[id, value] : undo_data.get_stream_settings()) {
      NvDRSProfileHandle profile_handle = nullptr;
      if (NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle) != NVAPI_OK) {
        return false;
      }
      NVDRS_SETTING setting = {};
      setting.version = NVDRS_SETTING_VER;
      auto status = NvAPI_DRS_GetSetting(session_handle, profile_handle, id, &setting);
      if (status != NVAPI_OK || setting.settingLocation != NVDRS_CURRENT_PROFILE_LOCATION || setting.u32CurrentValue != value.our_value) {
        continue;  // changed since by the user: theirs stays
      }
      if (value.undo_value) {
        setting = {};
        setting.version = NVDRS_SETTING_VER1;
        setting.settingId = id;
        setting.settingType = NVDRS_DWORD_TYPE;
        setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
        setting.u32CurrentValue = *value.undo_value;
        status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);
      } else {
        status = NvAPI_DRS_DeleteProfileSetting(session_handle, profile_handle, id);
        if (status == NVAPI_SETTING_NOT_FOUND) {
          status = NVAPI_OK;
        }
      }
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("Couldn't restore a global profile setting changed for streaming");
        return false;
      }
    }

    return true;
  }

  bool driver_settings_t::set_global_stream_setting(uint32_t id, uint32_t value, const std::optional<undo_data_t> &current_undo, std::optional<undo_data_t> &undo_data) {
    if (!session_handle) {
      return false;
    }
    undo_data.reset();

    NvDRSProfileHandle profile_handle = nullptr;
    auto status = NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      return false;
    }

    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER;
    status = NvAPI_DRS_GetSetting(session_handle, profile_handle, id, &setting);
    if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
      nvapi_error_message(status);
      return false;
    }
    const bool set_here = status == NVAPI_OK && setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION;
    if (set_here && setting.u32CurrentValue == value) {
      return true;
    }

    std::optional<uint32_t> undo_value;
    const auto &previous = current_undo ? current_undo->get_stream_settings() : std::map<uint32_t, undo_data_t::data_t::opengl_swapchain_t> {};
    if (auto it = previous.find(id); it != previous.end()) {
      undo_value = it->second.undo_value;
    } else if (set_here) {
      undo_value = setting.u32CurrentValue;
    }

    undo_data = undo_data_t();
    undo_data->set_stream_setting(id, value, undo_value);

    setting = {};
    setting.version = NVDRS_SETTING_VER1;
    setting.settingId = id;
    setting.settingType = NVDRS_DWORD_TYPE;
    setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
    setting.u32CurrentValue = value;
    status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      undo_data.reset();
      return false;
    }
    return true;
  }

  bool driver_settings_t::check_and_modify_global_profile(std::optional<undo_data_t> &undo_data) {
    if (!session_handle) {
      return false;
    }

    undo_data.reset();
    NvAPI_Status status;

    if (!get_nvprefs_options().opengl_vulkan_on_dxgi) {
      // User requested to leave OpenGL/Vulkan DXGI swapchain setting alone
      return true;
    }

    NvDRSProfileHandle profile_handle = nullptr;
    status = NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetBaseProfile() failed");
      return false;
    }

    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER;
    status = NvAPI_DRS_GetSetting(session_handle, profile_handle, OGL_CPL_PREFER_DXPRESENT_ID, &setting);

    // Remember current OpenGL/Vulkan DXGI swapchain setting and change it if needed
    if (status == NVAPI_SETTING_NOT_FOUND || (status == NVAPI_OK && setting.u32CurrentValue != OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED)) {
      undo_data = undo_data_t();
      if (status == NVAPI_OK) {
        undo_data->set_opengl_swapchain(OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED, setting.u32CurrentValue);
      } else {
        undo_data->set_opengl_swapchain(OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED, std::nullopt);
      }

      setting = {};
      setting.version = NVDRS_SETTING_VER1;
      setting.settingId = OGL_CPL_PREFER_DXPRESENT_ID;
      setting.settingType = NVDRS_DWORD_TYPE;
      setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
      setting.u32CurrentValue = OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED;

      status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_SetSetting() OGL_CPL_PREFER_DXPRESENT failed");
        return false;
      }

      info_message("Changed OGL_CPL_PREFER_DXPRESENT to OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED for base profile");
    } else if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetSetting() OGL_CPL_PREFER_DXPRESENT failed");
      return false;
    }

    return true;
  }

  bool driver_settings_t::set_global_frame_limit(uint32_t fps, const std::optional<undo_data_t> &current_undo, std::optional<undo_data_t> &undo_data) {
    if (!session_handle) {
      return false;
    }

    undo_data.reset();
    fps = std::min<uint32_t>(fps, FRL_FPS_MAX);

    NvDRSProfileHandle profile_handle = nullptr;
    auto status = NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetBaseProfile() failed");
      return false;
    }

    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER;
    status = NvAPI_DRS_GetSetting(session_handle, profile_handle, FRL_FPS_ID, &setting);
    if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetSetting() FRL_FPS failed");
      return false;
    }

    const bool set_here = status == NVAPI_OK && setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION;
    const auto previous = current_undo ? current_undo->get_frame_limit() : std::nullopt;
    const bool ours = set_here && previous && setting.u32CurrentValue == previous->our_value;

    if (set_here && !ours && setting.u32CurrentValue != FRL_FPS_DISABLED) {
      // The user chose a Max Frame Rate for all games themselves; respect it
      info_message("Max Frame Rate is already set in the global profile, leaving it alone");
      return true;
    }
    if (set_here && setting.u32CurrentValue == fps) {
      return true;
    }

    std::optional<uint32_t> undo_value;
    if (ours) {
      undo_value = previous->undo_value;
    } else if (set_here) {
      undo_value = setting.u32CurrentValue;
    }

    undo_data = undo_data_t();
    undo_data->set_frame_limit(fps, undo_value);

    setting = {};
    setting.version = NVDRS_SETTING_VER1;
    setting.settingId = FRL_FPS_ID;
    setting.settingType = NVDRS_DWORD_TYPE;
    setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
    setting.u32CurrentValue = fps;

    status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_SetSetting() FRL_FPS failed");
      undo_data.reset();
      return false;
    }

    info_message("Set Max Frame Rate in the global profile to " + std::to_string(fps) + " to match the stream");
    return true;
  }

  bool driver_settings_t::check_and_modify_application_profile(bool &modified) {
    if (!session_handle) {
      return false;
    }

    modified = false;
    NvAPI_Status status;

    NvAPI_UnicodeString profile_name = {};
    fill_nvapi_string(profile_name, tunastream_application_profile_name);

    NvDRSProfileHandle profile_handle = nullptr;
    status = NvAPI_DRS_FindProfileByName(session_handle, profile_name, &profile_handle);

    if (status != NVAPI_OK) {
      // Create application profile if missing
      NVDRS_PROFILE profile = {};
      profile.version = NVDRS_PROFILE_VER1;
      fill_nvapi_string(profile.profileName, tunastream_application_profile_name);
      status = NvAPI_DRS_CreateProfile(session_handle, &profile, &profile_handle);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_CreateProfile() failed");
        return false;
      }
      modified = true;
    }

    NvAPI_UnicodeString tunastream_path = {};
    fill_nvapi_string(tunastream_path, tunastream_application_path);

    NVDRS_APPLICATION application = {};
    application.version = NVDRS_APPLICATION_VER_V1;
    status = NvAPI_DRS_GetApplicationInfo(session_handle, profile_handle, tunastream_path, &application);

    if (status != NVAPI_OK) {
      // Add application to application profile if missing
      application.version = NVDRS_APPLICATION_VER_V1;
      application.isPredefined = 0;
      fill_nvapi_string(application.appName, tunastream_application_path);
      fill_nvapi_string(application.userFriendlyName, tunastream_application_path);
      fill_nvapi_string(application.launcher, L"");

      status = NvAPI_DRS_CreateApplication(session_handle, profile_handle, &application);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_CreateApplication() failed");
        return false;
      }
      modified = true;
    }

    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER1;
    status = NvAPI_DRS_GetSetting(session_handle, profile_handle, PREFERRED_PSTATE_ID, &setting);

    if (!get_nvprefs_options().tunastream_high_power_mode) {
      if (status == NVAPI_OK &&
          setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION) {
        // User requested to not use high power mode for tunastream.exe,
        // remove the setting from application profile if it's been set previously

        status = NvAPI_DRS_DeleteProfileSetting(session_handle, profile_handle, PREFERRED_PSTATE_ID);
        if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
          nvapi_error_message(status);
          error_message("NvAPI_DRS_DeleteProfileSetting() PREFERRED_PSTATE failed");
          return false;
        }
        modified = true;

        info_message(std::wstring(L"Removed PREFERRED_PSTATE for ") + tunastream_application_path);
      }
    } else if (status != NVAPI_OK ||
               setting.settingLocation != NVDRS_CURRENT_PROFILE_LOCATION ||
               setting.u32CurrentValue != PREFERRED_PSTATE_PREFER_MAX) {
      // Set power setting if needed
      setting = {};
      setting.version = NVDRS_SETTING_VER1;
      setting.settingId = PREFERRED_PSTATE_ID;
      setting.settingType = NVDRS_DWORD_TYPE;
      setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
      setting.u32CurrentValue = PREFERRED_PSTATE_PREFER_MAX;

      status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_SetSetting() PREFERRED_PSTATE failed");
        return false;
      }
      modified = true;

      info_message(std::wstring(L"Changed PREFERRED_PSTATE to PREFERRED_PSTATE_PREFER_MAX for ") + tunastream_application_path);
    }

    return true;
  }

}  // namespace nvprefs
