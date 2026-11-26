/**
 * @file src/platform/windows/app_windows.cpp
 * @brief The windows Share my screen can show on its own (a game's window), and what's needed to
 *        show one: which display it's on, and which program it belongs to (for its sound).
 */
// standard includes
#include <filesystem>
#include <string>

// platform includes
#include <windows.h>
// after windows.h
#include <dwmapi.h>

// local includes
#include "app_windows.h"
#include "misc.h"

namespace platf {
  namespace {
    std::string exe_of(DWORD pid) {
      std::string exe;
      if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2] {};
        DWORD size = (DWORD) std::size(path);
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
          exe = to_utf8(std::filesystem::path {path}.filename().wstring());
        }
        CloseHandle(process);
      }
      return exe;
    }

    bool shareable(HWND w) {
      if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) != nullptr) {
        return false;
      }
      if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) {
        return false;
      }
      BOOL cloaked = FALSE;
      if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;  // on another virtual desktop, or a suspended store app
      }
      wchar_t cls[64] {};
      GetClassNameW(w, cls, (int) std::size(cls));
      for (auto shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}) {
        if (_wcsicmp(cls, shell) == 0) {
          return false;
        }
      }
      return GetWindowTextLengthW(w) > 0;
    }
  }  // namespace

  nlohmann::json shareable_windows() {
    struct ctx_t {
      nlohmann::json list = nlohmann::json::array();
      DWORD self = GetCurrentProcessId();
    } ctx;
    EnumWindows([](HWND w, LPARAM param) -> BOOL {
      auto c = (ctx_t *) param;
      if (!shareable(w)) {
        return TRUE;
      }
      DWORD pid = 0;
      GetWindowThreadProcessId(w, &pid);
      if (pid == c->self) {
        return TRUE;
      }
      std::wstring title(GetWindowTextLengthW(w) + 1, L'\0');
      title.resize(GetWindowTextW(w, title.data(), (int) title.size()));
      c->list.push_back({
        {"id", std::to_string((std::uint64_t) (std::uintptr_t) w)},
        {"title", to_utf8(title)},
        {"exe", exe_of(pid)},
        {"minimized", IsIconic(w) != FALSE},
      });
      return TRUE;
    }, (LPARAM) &ctx);
    return ctx.list;
  }

  bool window_exists(std::uint64_t id) {
    return id && IsWindow((HWND) (std::uintptr_t) id);
  }

  std::uint32_t window_process(std::uint64_t id) {
    DWORD pid = 0;
    if (window_exists(id)) {
      GetWindowThreadProcessId((HWND) (std::uintptr_t) id, &pid);
    }
    return pid;
  }

  std::string window_display(std::uint64_t id) {
    if (!window_exists(id)) {
      return {};
    }
    MONITORINFOEXW info {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow((HWND) (std::uintptr_t) id, MONITOR_DEFAULTTONEAREST), &info)) {
      return {};
    }
    return to_utf8(info.szDevice);
  }

  std::string window_title(std::uint64_t id) {
    if (!window_exists(id)) {
      return {};
    }
    auto w = (HWND) (std::uintptr_t) id;
    std::wstring title(GetWindowTextLengthW(w) + 1, L'\0');
    title.resize(GetWindowTextW(w, title.data(), (int) title.size()));
    return to_utf8(title);
  }
}  // namespace platf
