/**
 * @file src/platform/windows/self_check.cpp
 * @brief What the PC can check by itself before testing: the checklist page's Self-check.
 *        Each check is {"id", "state": "ok" | "warn" | "info", "detail"}; the page words them.
 */
// standard includes
#include <string>

// platform includes
#include <windows.h>

// local includes
#include "misc.h"
#include "rtss.h"
#include "self_check.h"
#include "src/config.h"
#include "src/process.h"
#include "src/video.h"
#include "virtual_display.h"

namespace platf {
  namespace {
    DWORD windows_build() {
      using rtl_fn = LONG(WINAPI *)(OSVERSIONINFOW *);
      OSVERSIONINFOW v {};
      v.dwOSVersionInfoSize = sizeof(v);
      if (auto ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto get = (rtl_fn) GetProcAddress(ntdll, "RtlGetVersion")) {
          get(&v);
        }
      }
      return v.dwBuildNumber;
    }

    nlohmann::json check(const char *id, const char *state, const std::string &detail = {}) {
      return {{"id", id}, {"state", state}, {"detail", detail}};
    }
  }  // namespace

  nlohmann::json self_check() {
    nlohmann::json out = nlohmann::json::array();
    const bool system = is_running_as_system();
    out.push_back(check("service", system ? "ok" : "info"));

    HANDLE token = retrieve_users_token(false);
    const bool signed_in = token != nullptr;
    if (token) {
      CloseHandle(token);
    }
    out.push_back(check("signed_in", signed_in ? "ok" : "warn"));

    const auto build = windows_build();
    out.push_back(check("windows", build >= 19041 ? "ok" : "warn", std::to_string(build)));

    const auto &capture = config::video.capture;
    out.push_back(check("capture", capture == "wgc" ? (signed_in ? "ok" : "warn") : "info", capture.empty() ? "auto" : capture));

    out.push_back(check("second_encoder", video::parallel_encoding() ? "ok" : "warn"));

    const auto vd = proc::vDisplayDriverStatus;
    out.push_back(check("virtual_display", vd == VDISPLAY::DRIVER_STATUS::OK ? "ok" : "warn", std::to_string((int) vd)));

    const bool rtss_in = rtss::installed();
    out.push_back(check("rtss", rtss_in ? (rtss::running() ? "ok" : "warn") : "info", rtss_in ? (rtss::running() ? "running" : "installed") : "missing"));
    return out;
  }
}  // namespace platf
