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
 *
 * Shared PC mode keeps this up for as long as the app runs, since the PC's own screen stays the
 * main one and games would otherwise open there. It also sends the app's sound to the stream's
 * sound device while everything else keeps playing on the PC's speakers, using the per-app
 * sound device setting Windows has had since 10 (version 1803).
 */
// standard includes
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// platform includes
#include <windows.h>
#include <audiopolicy.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

// local includes
#include "app_windows.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf {
  namespace {
    std::mutex stream_audio_lock;
    std::wstring stream_audio_device;  // endpoint id, as in {0.0.0.00000000}.{guid}

    // When a streaming device's controller was last used (steady clock, milliseconds)
    std::atomic<std::int64_t> last_controller_ms {0};

    std::int64_t steady_ms() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::wstring current_stream_audio_device() {
      std::lock_guard lg {stream_audio_lock};
      return stream_audio_device;
    }

    /**
     * Windows' per-app sound device setting (Settings > Sound > App volume and device preferences).
     * The interface is undocumented and has two IDs depending on the Windows version; the layout
     * is the same. SetPersistedDefaultAudioEndpoint is the 26th method, after IInspectable's six.
     */
    class app_audio_routing_t {
    public:
      app_audio_routing_t() {
        combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!combase) {
          return;
        }
        ro_get_activation_factory = (ro_get_activation_factory_t) GetProcAddress(combase, "RoGetActivationFactory");
        windows_create_string = (windows_create_string_t) GetProcAddress(combase, "WindowsCreateString");
        windows_delete_string = (windows_delete_string_t) GetProcAddress(combase, "WindowsDeleteString");
        if (!ro_get_activation_factory || !windows_create_string || !windows_delete_string) {
          return;
        }
        static const wchar_t class_name[] = L"Windows.Media.Internal.AudioPolicyConfig";
        void *name = nullptr;
        if (FAILED(windows_create_string(class_name, (UINT32) (std::size(class_name) - 1), &name))) {
          return;
        }
        // Windows 10 21H2 and later, then earlier versions
        static const GUID ids[] {
          {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}},
          {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}},
        };
        for (const auto &id : ids) {
          if (SUCCEEDED(ro_get_activation_factory(name, id, &factory)) && factory) {
            break;
          }
          factory = nullptr;
        }
        windows_delete_string(name);
        if (!factory) {
          BOOST_LOG(warning) << "Shared PC mode: this Windows can't send one app's sound elsewhere"sv;
        }
      }

      ~app_audio_routing_t() {
        if (factory) {
          ((IUnknown *) factory)->Release();
        }
      }

      explicit operator bool() const {
        return factory != nullptr;
      }

      /// Send the process's sound to the device, or back to the PC's default when the id is empty
      bool route(DWORD pid, const std::wstring &endpoint_id) {
        if (!factory) {
          return false;
        }
        void *device = nullptr;
        if (!endpoint_id.empty()) {
          // The form the setting expects: the endpoint's device interface path
          const std::wstring path = L"\\\\?\\SWD#MMDEVAPI#" + endpoint_id + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
          if (FAILED(windows_create_string(path.c_str(), (UINT32) path.size(), &device))) {
            return false;
          }
        }
        using set_t = HRESULT(STDMETHODCALLTYPE *)(void *, UINT32, EDataFlow, ERole, void *);
        auto set = (set_t) (*(void ***) factory)[25];
        const bool ok = SUCCEEDED(set(factory, (UINT32) pid, eRender, eConsole, device)) &&
                        SUCCEEDED(set(factory, (UINT32) pid, eRender, eMultimedia, device));
        if (device) {
          windows_delete_string(device);
        }
        return ok;
      }

    private:
      using ro_get_activation_factory_t = HRESULT(WINAPI *)(void *, REFIID, void **);
      using windows_create_string_t = HRESULT(WINAPI *)(const wchar_t *, UINT32, void **);
      using windows_delete_string_t = HRESULT(WINAPI *)(void *);
      HMODULE combase = nullptr;
      ro_get_activation_factory_t ro_get_activation_factory = nullptr;
      windows_create_string_t windows_create_string = nullptr;
      windows_delete_string_t windows_delete_string = nullptr;
      void *factory = nullptr;
    };

    // Spelled out here: this file doesn't define the audio GUIDs itself
    const GUID clsid_device_enumerator {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    const GUID iid_device_enumerator {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
    const GUID iid_session_manager2 {0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};
    const GUID iid_session_control2 {0xBFB7FF88, 0x7239, 0x4FC9, {0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D}};

    /// Processes playing sound right now, on any output device
    std::set<DWORD> processes_with_sound() {
      std::set<DWORD> pids;
      IMMDeviceEnumerator *enumerator = nullptr;
      if (FAILED(CoCreateInstance(clsid_device_enumerator, nullptr, CLSCTX_ALL, iid_device_enumerator, (void **) &enumerator))) {
        return pids;
      }
      IMMDeviceCollection *devices = nullptr;
      if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
          IMMDevice *device = nullptr;
          if (FAILED(devices->Item(i, &device))) {
            continue;
          }
          IAudioSessionManager2 *manager = nullptr;
          if (SUCCEEDED(device->Activate(iid_session_manager2, CLSCTX_ALL, nullptr, (void **) &manager))) {
            IAudioSessionEnumerator *sessions = nullptr;
            if (SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
              int n = 0;
              sessions->GetCount(&n);
              for (int k = 0; k < n; ++k) {
                IAudioSessionControl *control = nullptr;
                if (FAILED(sessions->GetSession(k, &control))) {
                  continue;
                }
                IAudioSessionControl2 *control2 = nullptr;
                if (SUCCEEDED(control->QueryInterface(iid_session_control2, (void **) &control2))) {
                  DWORD pid = 0;
                  if (SUCCEEDED(control2->GetProcessId(&pid)) && pid) {
                    pids.insert(pid);
                  }
                  control2->Release();
                }
                control->Release();
              }
              sessions->Release();
            }
            manager->Release();
          }
          device->Release();
        }
        devices->Release();
      }
      enumerator->Release();
      return pids;
    }

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

    /// Programs that start games on the app's behalf, so what they start belongs to the stream
    bool is_launcher(const std::wstring &exe) {
      static const std::set<std::wstring> launchers {
        L"steam.exe", L"epicgameslauncher.exe", L"eadesktop.exe", L"galaxyclient.exe", L"upc.exe",
        L"battle.net.exe", L"amazon games ui.exe", L"playnite.desktopapp.exe", L"playnite.fullscreenapp.exe",
        L"tunastream.exe", L"tunastreamsvc.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"rundll32.exe",
      };
      return launchers.count(exe) != 0;
    }

    std::wstring class_of(HWND hwnd) {
      wchar_t name[96] {};
      GetClassNameW(hwnd, name, 96);
      return name;
    }

    /**
     * Windows' own shell (taskbar, Start, search, Alt+Tab, notification flyouts, the desktop) stays
     * where Windows puts it. Its ordinary windows do move like any program's: File Explorer, the
     * Run box, copy progress, Settings, Task Manager, and dialogs.
     */
    bool is_shell_surface(HWND hwnd, const std::wstring &exe) {
      static const std::set<std::wstring> shell_programs {
        L"startmenuexperiencehost.exe", L"searchhost.exe", L"searchapp.exe", L"shellexperiencehost.exe",
        L"shellhost.exe", L"textinputhost.exe", L"lockapp.exe", L"widgets.exe", L"widgetboard.exe",
        L"screenclippinghost.exe", L"snippingtool.exe",
      };
      if (shell_programs.count(exe)) {
        return true;
      }
      const auto cls = class_of(hwnd);
      if (cls == L"Windows.UI.Core.CoreWindow") {
        return true;
      }
      if (exe == L"explorer.exe") {
        // Of Explorer's windows, only folders, the Run box and copy progress are the person's
        return cls != L"CabinetWClass" && cls != L"#32770" && cls != L"OperationStatusWindow";
      }
      return false;
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
      window_herder_impl_t(std::wstring display, bool include_steam, bool shared, std::wstring home = {}):
          display {std::move(display)},
          home_display {std::move(home)},
          include_steam {include_steam},
          shared {shared} {
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
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

        {
          std::unique_ptr<app_audio_routing_t> audio;
          if (shared) {
            audio = std::make_unique<app_audio_routing_t>();
          }
          auto next_audio = std::chrono::steady_clock::now();
          const auto until = std::chrono::steady_clock::now() + watch_for;
          // Shared PC mode watches for as long as the app runs: the PC's own screen stays the main
          // one, so games would open there at any time
          while (!stop && (shared || std::chrono::steady_clock::now() < until)) {
            RECT target {};
            if (monitor_rect(display, target)) {
              herd_once(target);
              if (shared) {
                if (config::stream.shared_pc_borderless) {
                  make_borderless(target);
                }
                return_focus(target);
              }
            }
            if (audio && *audio && std::chrono::steady_clock::now() >= next_audio) {
              next_audio = std::chrono::steady_clock::now() + 2s;
              route_sound(*audio);
            }
            const auto wait = home_display.empty() ? poll_every : 150ms;
            for (auto waited = 0ms; waited < wait && !stop; waited += 50ms) {
              std::this_thread::sleep_for(50ms);
            }
          }

          // Back to the PC's own speakers for everything that was sent to the stream
          if (audio && *audio) {
            for (DWORD pid : routed) {
              audio->route(pid, {});
            }
            if (!routed.empty()) {
              BOOST_LOG(info) << "Shared PC mode: sound of "sv << routed.size() << " program(s) back on the PC's own device"sv;
            }
          }
        }
        if (com) {
          CoUninitialize();
        }
      }

      /// Where the person at the PC's windows go: the screen their mouse is on, or their main one
      bool home_rect(const RECT &target, RECT &home) {
        POINT cursor {};
        if (GetCursorPos(&cursor) && !PtInRect(&target, cursor)) {
          MONITORINFO info {};
          info.cbSize = sizeof(info);
          if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info)) {
            home = info.rcWork;
            return true;
          }
        }
        return monitor_rect(home_display, home);
      }

      /**
       * Windows has one keyboard focus. When the person at the PC clicks their own windows, the
       * game loses it, and some games pause or stop reading the controller. So when the controller
       * is being used and the person at the PC has left their mouse and keyboard alone for a few
       * seconds, the game gets focus back. Their own typing is never interrupted: any key press or
       * mouse movement of theirs resets the wait.
       */
      void return_focus(const RECT &target) {
        constexpr std::int64_t CONTROLLER_RECENT_MS = 3000;
        constexpr DWORD PC_IDLE_MS = 8000;
        const auto now = steady_ms();
        if (now - last_controller_ms.load() > CONTROLLER_RECENT_MS || now < next_focus_try_ms) {
          return;
        }
        LASTINPUTINFO idle {sizeof(LASTINPUTINFO)};
        if (!GetLastInputInfo(&idle) || GetTickCount() - idle.dwTime < PC_IDLE_MS) {
          return;
        }

        // Already focused on something on the stream's screen?
        HWND foreground = GetForegroundWindow();
        if (foreground) {
          RECT rect {};
          GetWindowRect(foreground, &rect);
          POINT centre {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
          if (PtInRect(&target, centre)) {
            return;
          }
        }

        // The stream's biggest window that belongs to the app
        HWND game = nullptr;
        LONG best_area = 0;
        for (HWND hwnd : app_windows()) {
          RECT rect {};
          GetWindowRect(hwnd, &rect);
          POINT centre {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
          if (!PtInRect(&target, centre) || IsIconic(hwnd)) {
            continue;
          }
          DWORD pid = 0;
          GetWindowThreadProcessId(hwnd, &pid);
          process_info_t proc;
          if (!pid || !process_info(pid, proc) || !(belongs_to_stream(pid, proc) || (include_steam && is_steam(proc.exe)))) {
            continue;
          }
          const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
          if (area > best_area) {
            best_area = area;
            game = hwnd;
          }
        }
        next_focus_try_ms = now + 5000;
        if (!game) {
          return;
        }

        // A program in the background may only take focus with the help of the one that has it
        const DWORD ours = GetCurrentThreadId();
        const DWORD theirs = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        const bool attached = theirs && theirs != ours && AttachThreadInput(ours, theirs, TRUE);
        const bool ok = SetForegroundWindow(game);
        BringWindowToTop(game);
        if (attached) {
          AttachThreadInput(ours, theirs, FALSE);
        }
        if (ok && !focus_logged) {
          focus_logged = true;
          BOOST_LOG(info) << "Shared PC mode: gave the game focus back while the PC was idle"sv;
        }
      }

      /**
       * Shared PC mode: a game set to "Windowed" in its own settings loses its border and fills the
       * stream's screen, so it looks full screen on the TV without taking over a monitor (true
       * full screen picks the PC's main monitor in many games). Only the app's own top-level
       * window with a title bar and the screen's shape (16:9 on a 16:9 TV) is changed, once it has
       * kept its size for 3 seconds, as games resize while they start; launchers, dialogs and
       * settings windows are left alone. Nothing is done inside the game: it's the same as
       * dragging the window, done by Windows.
       */
      void make_borderless(const RECT &target) {
        constexpr std::int64_t STEADY_FOR_MS = 3000;
        constexpr int MAX_TRIES = 2;  // a game that puts its border back is left as it wants
        const auto now = steady_ms();
        const LONG target_w = target.right - target.left;
        const LONG target_h = target.bottom - target.top;
        if (target_w <= 0 || target_h <= 0) {
          return;
        }
        for (HWND hwnd : app_windows()) {
          if (borderless_tries[hwnd] >= MAX_TRIES) {
            continue;
          }
          RECT rect {};
          GetWindowRect(hwnd, &rect);
          POINT centre {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
          if (!PtInRect(&target, centre) || IsIconic(hwnd)) {
            continue;
          }
          const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
          if ((style & WS_CAPTION) != WS_CAPTION || (style & WS_CHILD) || GetWindow(hwnd, GW_OWNER)) {
            // Already borderless, or a dialog of another window
            continue;
          }
          const auto cls = class_of(hwnd);
          if (cls == L"#32770" || cls == L"ApplicationFrameWindow" || cls == L"CabinetWClass" ||
              cls == L"ConsoleWindowClass" || cls == L"CASCADIA_HOSTING_WINDOW_CLASS") {
            continue;
          }
          DWORD pid = 0;
          GetWindowThreadProcessId(hwnd, &pid);
          process_info_t proc;
          if (!pid || pid == GetCurrentProcessId() || !process_info(pid, proc) || !belongs_to_stream(pid, proc) ||
              is_launcher(proc.exe) || proc.exe.find(L"launcher") != std::wstring::npos ||
              proc.exe.find(L"setup") != std::wstring::npos || proc.exe.find(L"crash") != std::wstring::npos) {
            continue;
          }
          wchar_t title_buf[128] {};
          GetWindowTextW(hwnd, title_buf, 128);
          std::wstring title {title_buf};
          std::wstring lower_title = title;
          for (auto &c : lower_title) {
            c = static_cast<wchar_t>(towlower(c));
          }
          if (lower_title.find(L"launcher") != std::wstring::npos || lower_title.find(L"setup") != std::wstring::npos ||
              lower_title.find(L"configuration") != std::wstring::npos) {
            continue;
          }

          // The picture inside the border has the screen's shape
          RECT client {};
          if (!GetClientRect(hwnd, &client) || client.right < 640 || client.bottom < 360) {
            continue;
          }
          const double shape = double(client.right) / client.bottom;
          const double screen = double(target_w) / target_h;
          if (std::abs(shape - screen) / screen > 0.03) {
            continue;
          }

          // Same size for a few seconds
          auto &seen = borderless_seen[hwnd];
          if (!EqualRect(&seen.first, &rect)) {
            seen = {rect, now};
            continue;
          }
          if (now - seen.second < STEADY_FOR_MS) {
            continue;
          }

          ++borderless_tries[hwnd];
          if (IsZoomed(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
          }
          SetWindowLongW(hwnd, GWL_STYLE, style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU));
          const LONG ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
          SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
          SetWindowPos(hwnd, nullptr, target.left, target.top, target_w, target_h,
                       SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
          GetWindowRect(hwnd, &seen.first);
          seen.second = now;
          BOOST_LOG(info) << "Shared PC mode: made ["sv << to_utf8(title) << "] ("sv << to_utf8(proc.exe)
                          << ") borderless, filling the stream's screen"sv;
        }
      }

      /// Whether a process belongs to the streamed app rather than to the person at the PC
      bool belongs_to_stream(DWORD pid, const process_info_t &proc) {
        if (to_u64(proc.created) < launched_at) {
          return false;
        }
        // Follow the chain of parents: a program the person at the PC already had open (the
        // taskbar, a browser, a chat app) means it's theirs; a launcher or a process that has
        // already exited (the app's own short-lived starter) means it's the stream's
        DWORD current = pid;
        for (int depth = 0; depth < 8; ++depth) {
          DWORD parent_pid = parent_of(current);
          process_info_t parent;
          if (!parent_pid || !process_info(parent_pid, parent)) {
            return true;
          }
          if (parent.exe == L"explorer.exe") {
            return false;
          }
          if (is_launcher(parent.exe)) {
            return true;
          }
          if (to_u64(parent.created) < launched_at) {
            return false;
          }
          current = parent_pid;
        }
        return true;
      }

      void route_sound(app_audio_routing_t &audio) {
        const auto device = current_stream_audio_device();
        if (device.empty()) {
          return;
        }
        for (DWORD pid : processes_with_sound()) {
          if (routed.count(pid) || checked.count(pid) || pid == GetCurrentProcessId()) {
            continue;
          }
          process_info_t proc;
          if (!process_info(pid, proc)) {
            continue;
          }
          if (!belongs_to_stream(pid, proc) || is_steam(proc.exe)) {
            // Steam's own sounds are the person at the PC's too; leave them
            checked.insert(pid);
            continue;
          }
          if (audio.route(pid, device)) {
            routed.insert(pid);
            BOOST_LOG(info) << "Shared PC mode: "sv << to_utf8(proc.exe) << "'s sound goes to the stream"sv;
          } else {
            checked.insert(pid);
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
          const bool on_stream = PtInRect(&target, centre);
          if (on_stream && home_display.empty()) {
            continue;
          }

          DWORD pid = 0;
          GetWindowThreadProcessId(hwnd, &pid);
          process_info_t proc;
          if (!pid || !process_info(pid, proc)) {
            continue;
          }
          bool steam_window = include_steam && is_steam(proc.exe);
          if (steam_window && shared) {
            // In shared PC mode the person at the PC may be using Steam too: only Big Picture goes
            wchar_t steam_title[128] {};
            GetWindowTextW(hwnd, steam_title, 128);
            steam_window = std::wstring_view {steam_title}.find(L"Big Picture") != std::wstring_view::npos;
          }
          const bool stream_window = steam_window || belongs_to_stream(pid, proc);

          if (on_stream) {
            // The stream's screen is the main one, so the person at the PC's new windows open there:
            // send them back to their own screen
            if (stream_window || is_shell_surface(hwnd, proc.exe)) {
              existing.insert(hwnd);
              continue;
            }
            RECT home {};
            if (!home_rect(target, home)) {
              continue;
            }
            int &moves = move_count[hwnd];
            if (moves >= max_moves_per_window) {
              continue;
            }
            ++moves;
            wchar_t title[128] {};
            GetWindowTextW(hwnd, title, 128);
            move_to(hwnd, home);
            BOOST_LOG(info) << "Moved a window off the stream's screen, back to the PC's own: ["sv << to_utf8(title) << "] ("sv << to_utf8(proc.exe) << ')';
            continue;
          }

          if (!stream_window) {
            // Opened by the person at the PC (from the taskbar, or a program already running)
            existing.insert(hwnd);
            continue;
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
      std::wstring home_display;  // the PC's own main screen, when the stream's is the main one
      bool include_steam;
      bool shared;
      std::int64_t next_focus_try_ms = 0;
      bool focus_logged = false;
      std::set<DWORD> routed;  // processes whose sound was sent to the stream
      std::set<DWORD> checked;  // processes looked at and left alone
      std::uint64_t launched_at = 0;
      std::set<HWND> existing;
      std::map<HWND, int> move_count;
      std::map<HWND, int> borderless_tries;  // windows made borderless (or tried)
      std::map<HWND, std::pair<RECT, std::int64_t>> borderless_seen;  // last size, and since when
      std::atomic<bool> stop {false};
      std::thread worker;
    };
  }  // namespace

  bool move_window_to_display(std::uint64_t id, const std::string &display_name) {
    auto hwnd = (HWND) (std::uintptr_t) id;
    if (!id || !IsWindow(hwnd)) {
      return false;
    }
    RECT target {};
    if (!monitor_rect(from_utf8(display_name), target)) {
      return false;
    }
    if (IsIconic(hwnd)) {
      ShowWindow(hwnd, SW_RESTORE);
    }
    move_to(hwnd, target);
    SetForegroundWindow(hwnd);
    return true;
  }

  std::unique_ptr<window_herder_t> herd_new_windows(const std::string &display_name, bool include_steam) {
    if (display_name.empty()) {
      return nullptr;
    }
    return std::make_unique<window_herder_impl_t>(from_utf8(display_name), include_steam, false);
  }

  std::unique_ptr<window_herder_t> keep_app_on_stream(const std::string &display_name, bool include_steam, const std::string &home_display) {
    if (display_name.empty()) {
      return nullptr;
    }
    return std::make_unique<window_herder_impl_t>(from_utf8(display_name), include_steam, true, from_utf8(home_display));
  }

  void note_controller_input() {
    last_controller_ms.store(steady_ms(), std::memory_order_relaxed);
  }

  void set_stream_audio_device(const std::wstring &device_id) {
    std::lock_guard lg {stream_audio_lock};
    stream_audio_device = device_id;
  }
}  // namespace platf
