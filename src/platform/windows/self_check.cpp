/**
 * @file src/platform/windows/self_check.cpp
 * @brief What the PC can check by itself before testing: the checklist page's Self-check.
 *        Each check is {"id", "state": "ok" | "warn" | "info", "detail"}; the page words them.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// platform includes
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on

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

    /** What a Windows tool prints, without a window; empty if it couldn't run. */
    std::string run_quiet(const std::wstring &exe, const std::wstring &args) {
      SECURITY_ATTRIBUTES sa {sizeof(sa), nullptr, TRUE};
      HANDLE rd = nullptr;
      HANDLE wr = nullptr;
      if (!CreatePipe(&rd, &wr, &sa, 0)) {
        return {};
      }
      SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
      STARTUPINFOW si {};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdOutput = wr;
      si.hStdError = wr;
      PROCESS_INFORMATION pi {};
      std::wstring cmd = L"\"" + exe + L"\" " + args;
      const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
      CloseHandle(wr);
      std::string out;
      if (started) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0 && out.size() < (1 << 20)) {
          out.append(buf, n);
        }
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
      }
      CloseHandle(rd);
      return out;
    }

    std::string lower(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char) std::tolower(c);
      });
      return s;
    }

    /**
     * Whether the TV can wake this PC: the network adapter it reaches the home network through
     * (the one with a gateway, a cable preferred), and whether Windows lets that adapter wake it.
     */
    nlohmann::json wake_check() {
      ULONG size = 32 * 1024;
      std::vector<ULONGLONG> buffer(size / sizeof(ULONGLONG) + 1);
      auto list = (IP_ADAPTER_ADDRESSES *) buffer.data();
      if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
        buffer.assign(size / sizeof(ULONGLONG) + 1, 0);
        list = (IP_ADAPTER_ADDRESSES *) buffer.data();
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) != NO_ERROR) {
          return nullptr;
        }
      }
      IP_ADAPTER_ADDRESSES *best = nullptr;
      for (auto a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->PhysicalAddressLength != 6 || !a->FirstGatewayAddress ||
            a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) {
          continue;
        }
        if (!best || (best->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType == IF_TYPE_ETHERNET_CSMACD)) {
          best = a;
        }
      }
      if (!best) {
        return nullptr;
      }
      const auto name = to_utf8(best->Description ? best->Description : L"");
      if (best->IfType == IF_TYPE_IEEE80211) {
        return check("wake", "info", name);
      }
      // Devices Windows lets wake the PC, one per line
      wchar_t system[MAX_PATH];
      GetSystemDirectoryW(system, MAX_PATH);
      const auto armed = lower(run_quiet(std::wstring {system} + L"\\powercfg.exe", L"/devicequery wake_armed"));
      const bool can_wake = !name.empty() && armed.find(lower(name)) != std::string::npos;
      return check("wake", can_wake ? "ok" : "warn", name);
    }

    /** Fast startup on: a shut-down PC usually can't be woken, only a sleeping one. */
    bool fast_startup_on() {
      DWORD value = 0;
      DWORD size = sizeof(value);
      return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", L"HiberbootEnabled", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value == 1;
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

    if (auto wake = wake_check(); !wake.is_null()) {
      out.push_back(wake);
    }
    if (fast_startup_on()) {
      out.push_back(check("fast_startup", "info"));
    }
    return out;
  }
}  // namespace platf
