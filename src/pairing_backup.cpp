/**
 * @file src/pairing_backup.cpp
 * @brief Keeps paired devices through reinstalls.
 *
 * Paired devices live in the state file, and the PC's pairing certificate and key in the
 * credentials folder, both inside the install folder. Losing either means every device has to
 * pair again. A copy is kept in %ProgramData%\TunaStream\backup, outside the install folder, in a
 * folder only Windows itself and administrators can read, because it holds the private key.
 */
// standard includes
#include <filesystem>
#include <string>
#include <vector>

// local includes
#include "config.h"
#include "logging.h"
#include "pairing_backup.h"

#ifdef _WIN32
  #include <windows.h>
  #include <aclapi.h>
  #include <sddl.h>
#endif

using namespace std::literals;
namespace fs = std::filesystem;

namespace pairing_backup {
#ifdef _WIN32
  namespace {
    /// The backup folder, created with access for SYSTEM and Administrators only. Empty on failure.
    fs::path backup_folder() {
      wchar_t program_data[MAX_PATH] {};
      if (!GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH)) {
        return {};
      }
      auto parent = fs::path {program_data} / L"TunaStream";
      auto folder = parent / L"backup";
      std::error_code ec;
      fs::create_directories(parent, ec);

      // SYSTEM and Administrators get full control; nothing is inherited, so ordinary users can't read it
      PSECURITY_DESCRIPTOR sd = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sd, nullptr)) {
        return {};
      }
      SECURITY_ATTRIBUTES sa {sizeof(sa), sd, FALSE};
      if (!CreateDirectoryW(folder.c_str(), &sa) && GetLastError() != ERROR_ALREADY_EXISTS) {
        LocalFree(sd);
        return {};
      }

      // Apply the same protection to a folder that already existed
      BOOL present = FALSE, defaulted = FALSE;
      PACL dacl = nullptr;
      bool secured = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present &&
                     SetNamedSecurityInfoW(const_cast<wchar_t *>(folder.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
      LocalFree(sd);
      if (!secured) {
        BOOST_LOG(warning) << "Couldn't restrict access to the pairing backup folder; not using it"sv;
        return {};
      }
      return folder;
    }

    struct item_t {
      fs::path live;
      const wchar_t *backup_name;
    };

    /// Paired devices with the certificate and key they were paired against
    std::vector<item_t> pairing_items() {
      return {
        {fs::path {config::nvhttp.file_state}, L"state.json"},
        {fs::path {config::nvhttp.cert}, L"cacert.pem"},
        {fs::path {config::nvhttp.pkey}, L"cakey.pem"},
      };
    }

    /// Settings and the app list, restored one by one
    std::vector<item_t> setting_items() {
      return {
        {fs::path {config::tunastream.config_file}, L"tunastream.conf"},
        {fs::path {config::stream.file_apps}, L"apps.json"},
      };
    }

    bool restore(const fs::path &folder, const item_t &item) {
      std::error_code ec;
      fs::create_directories(item.live.parent_path(), ec);
      return fs::copy_file(folder / item.backup_name, item.live, fs::copy_options::overwrite_existing, ec);
    }
  }  // namespace

  void restore_missing() {
    auto folder = backup_folder();
    if (folder.empty()) {
      return;
    }
    std::error_code ec;

    // Paired devices only work with the certificate they were paired against, so these go
    // back together, and only if the backup has all of them.
    auto pairing = pairing_items();
    bool any_missing = false, backup_complete = true;
    for (const auto &item : pairing) {
      any_missing = any_missing || !fs::exists(item.live, ec);
      backup_complete = backup_complete && fs::exists(folder / item.backup_name, ec);
    }
    if (any_missing && backup_complete) {
      bool ok = true;
      for (const auto &item : pairing) {
        ok = restore(folder, item) && ok;
      }
      BOOST_LOG(info) << (ok ? "Restored paired devices from the backup"sv : "Couldn't fully restore paired devices from the backup"sv);
    }

    for (const auto &item : setting_items()) {
      if (!fs::exists(item.live, ec) && fs::exists(folder / item.backup_name, ec) && restore(folder, item)) {
        BOOST_LOG(info) << "Restored "sv << item.live.filename().string() << " from the backup"sv;
      }
    }
  }

  void save() {
    auto folder = backup_folder();
    if (folder.empty()) {
      return;
    }
    std::error_code ec;
    auto items = pairing_items();
    auto settings = setting_items();
    items.insert(items.end(), settings.begin(), settings.end());
    for (const auto &item : items) {
      if (fs::exists(item.live, ec)) {
        fs::copy_file(item.live, folder / item.backup_name, fs::copy_options::overwrite_existing, ec);
        if (ec) {
          BOOST_LOG(warning) << "Couldn't back up "sv << item.live.filename().string() << ": "sv << ec.message();
        }
      }
    }
  }
#else
  void restore_missing() {
  }

  void save() {
  }
#endif
}  // namespace pairing_backup
