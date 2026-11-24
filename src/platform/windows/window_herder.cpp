/**
 * @file src/platform/windows/window_herder.cpp
 * @brief Moves the windows a streamed app opens onto the stream's virtual display.
 *
 * Making the virtual display primary is usually enough, but some programs ignore that: Steam
 * opens Big Picture on the monitor it used last, and some games remember their monitor too. So
 * for a few minutes after an app starts, windows it opens anywhere else are moved onto the
 * stream's display.
 *
 * Only windows that didn't exist before the launch are touched, and only when they belong to a
 * process started after the launch (the game and anything it starts) or, for a Steam app, to
 * Steam itself. Windows the person at the PC opens from programs that were already running are
 * left where they are.
 */
// standard includes
#include <atomic>
#include <chrono>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

// platform includes
#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>

// local includes
#include "misc.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf {
  namespace {
    // How long after the launch new windows are watched for. Games can take a while to start.
    constexpr auto watch_for = 3min;
    constexpr auto poll_every = 500ms;
    // A window that keeps jumping back is given up on after this many moves
    constexpr int max_moves_per_window = 3;

    struct monitor_find_t {
      std::wstring device;
      RECT rect {};
      bool found = false;
    };

    BOOL CALLBACK find_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
      auto *find = reinterpret_cast<monitor_find_t *>(param);
      MONITORINFOEXW info {};
      info.cbSize = sizeof(info);
      if (GetMonitorInfoW(monitor, &info) && find->device == info.szDevice) {
        find->rect = info.rcMonitor;
        find->found = true;
        return FALSE;
      }
      return TRUE;
    }

    bool monitor_rect(const std::wstring &device, RECT &rect) {
      monitor_find_t find;
      find.device = device;
      EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(&find));
      rect = find.rect;
      return find.found;
    }

    // Visible, ordinary top-level windows: not tool windows, not hidden by DWM, not tiny
    bool is_app_window(HWND hwnd) {
      if (!IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return false;
      }
      LONG ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
      if (ex_style & WS_EX_TOOLWINDOW) {
        return false;
      }
      BOOL cloaked = FALSE;
      if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
      }
      RECT rect {};
      if (!GetWindowRect(hwnd, &rect)) {
        return false;
      }
      return (rect.right - rect.left) >= 200 && (rect.bottom - rect.top) >= 150;
    }

    BOOL CALLBACK collect_windows(HWND hwnd, LPARAM param) {
      if (is_app_window(hwnd)) {
        reinterpret_cast<std::vector<HWND> *>(param)->push_back(hwnd);
      }
      return TRUE;
    }

    std::vector<HWND> app_windows() {
      std::vector<HWND> windows;
      EnumWindows(collect_windows, reinterpret_cast<LPARAM>(&windows));
      return windows;
    }

    struct process_info_t {
      FILETIME created {};
      std::wstring exe;  // lower case file name, e.g. steam.exe
    };

    bool process_info(DWORD pid, process_info_t &info) {
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!process) {
        return false;
      }
      auto close = util::fail_guard([process]() {
        CloseHandle(process);
      });
      FILETIME exited, kernel, user;
      if (!GetProcessTimes(process, &info.created, &exited, &kernel, &user)) {
        return false;
      }
      wchar_t path[MAX_PATH] {};
      DWORD size = MAX_PATH;
      if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        std::wstring full(path, size);
        auto slash = full.find_last_of(L"\\/");
        info.exe = slash == std::wstring::npos ? full : full.substr(slash + 1);
        for (auto &c : info.exe) {
          c = static_cast<wchar_t>(towlower(c));
        }
      }
      return true;
    }

    // The process that started this one, or 0 if it can't be found
    DWORD parent_of(DWORD pid) {
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
      }
      auto close = util::fail_guard([snapshot]() {
        CloseHandle(snapshot);
      });
      PROCESSENTRY32W entry {};
      entry.dwSize = sizeof(entry);
      for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == pid) {
          return entry.th32ParentProcessID;
        }
      }
      return 0;
    }

    bool is_steam(const std::wstring &exe) {
      return exe == L"steam.exe" || exe == L"steamwebhelper.exe";
    }

    std::uint64_t to_u64(const FILETIME &time) {
      return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    // Moves a window onto the target display, keeping it maximised or full screen if it was
    void move_to(HWND hwnd, const RECT &target) {
      RECT rect {};
      GetWindowRect(hwnd, &rect);
      HMONITOR from_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
      MONITORINFO from {};
      from.cbSize = sizeof(from);
      GetMonitorInfoW(from_monitor, &from);

      const bool maximised = IsZoomed(hwnd);
      const bool covers_monitor = rect.left <= from.rcMonitor.left && rect.top <= from.rcMonitor.top &&
                                  rect.right >= from.rcMonitor.right && rect.bottom >= from.rcMonitor.bottom;
      const int target_w = target.right - target.left;
      const int target_h = target.bottom - target.top;

      if (maximised) {
        // A maximised window has to be restored, moved, then maximised again on its new display
        ShowWindow(hwnd, SW_RESTORE);
        SetWindowPos(hwnd, nullptr, target.left + 40, target.top + 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_MAXIMIZE);
      } else if (covers_monitor) {
        // Borderless full screen (Steam Big Picture, most games): fill the target display
        SetWindowPos(hwnd, nullptr, target.left, target.top, target_w, target_h, SWP_NOZORDER | SWP_NOACTIVATE);
      } else {
        // An ordinary window: same size, centred on the target display
        int w = std::min<int>(rect.right - rect.left, target_w);
        int h = std::min<int>(rect.bottom - rect.top, target_h);
        SetWindowPos(hwnd, nullptr, target.left + (target_w - w) / 2, target.top + (target_h - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }

    class window_herder_impl_t: public window_herder_t {
    public:
      window_herder_impl_t(std::wstring display, bool include_steam):
          display {std::move(display)},
          include_steam {include_steam} {
        // Everything already on screen belongs to the person at the PC
        for (HWND hwnd : app_windows()) {
          existing.insert(hwnd);
        }
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        // A little slack: the app's first process can start a moment before this runs
        launched_at = to_u64(now) - 20'000'000ull;  // 2 seconds, in 100 ns units
        worker = std::thread([this]() {
          run();
        });
      }

      ~window_herder_impl_t() override {
        stop = true;
        if (worker.joinable()) {
          worker.join();
        }
      }

    private:
      void run() {
        // Windows belong to the signed-in user's desktop; this thread has to be on it to see them
        syncThreadDesktop();

        const auto until = std::chrono::steady_clock::now() + watch_for;
        while (!stop && std::chrono::steady_clock::now() < until) {
          RECT target {};
          if (monitor_rect(display, target)) {
            herd_once(target);
          }
          for (auto waited = 0ms; waited < poll_every && !stop; waited += 50ms) {
            std::this_thread::sleep_for(50ms);
          }
        }
      }

      void herd_once(const RECT &target) {
        for (HWND hwnd : app_windows()) {
          if (existing.count(hwnd)) {
            continue;
          }

          // Already on the stream's display?
          RECT rect {};
          GetWindowRect(hwnd, &rect);
          POINT centre {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
          if (PtInRect(&target, centre)) {
            continue;
          }

          DWORD pid = 0;
          GetWindowThreadProcessId(hwnd, &pid);
          process_info_t proc;
          if (!pid || !process_info(pid, proc)) {
            continue;
          }
          const bool started_for_stream = to_u64(proc.created) >= launched_at;
          if (!started_for_stream && !(include_steam && is_steam(proc.exe))) {
            // Opened by a program that was already running: the person at the PC's, so leave it
            existing.insert(hwnd);
            continue;
          }
          if (started_for_stream && !is_steam(proc.exe)) {
            // Started from the taskbar, Start menu or desktop: the person at the PC opened it
            process_info_t parent;
            DWORD parent_pid = parent_of(pid);
            if (parent_pid && process_info(parent_pid, parent) && parent.exe == L"explorer.exe") {
              existing.insert(hwnd);
              continue;
            }
          }

          int &moves = move_count[hwnd];
          if (moves >= max_moves_per_window) {
            continue;
          }
          ++moves;

          wchar_t title[128] {};
          GetWindowTextW(hwnd, title, 128);
          move_to(hwnd, target);
          BOOST_LOG(info) << "Moved a window onto the stream's screen: ["sv << to_utf8(title) << "] ("sv << to_utf8(proc.exe) << ')';
        }
      }

      std::wstring display;
      bool include_steam;
      std::uint64_t launched_at = 0;
      std::set<HWND> existing;
      std::map<HWND, int> move_count;
      std::atomic<bool> stop {false};
      std::thread worker;
    };
  }  // namespace

  std::unique_ptr<window_herder_t> herd_new_windows(const std::string &display_name, bool include_steam) {
    if (display_name.empty()) {
      return nullptr;
    }
    return std::make_unique<window_herder_impl_t>(from_utf8(display_name), include_steam);
  }
}  // namespace platf
