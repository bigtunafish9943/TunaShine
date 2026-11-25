/**
 * @file src/platform/windows/user_helper.cpp
 * @brief Small jobs done by a copy of TunaStream running as the signed-in user.
 *
 * TunaStream runs as SYSTEM, and a few Windows features only work for a signed-in user. For
 * those, TunaStream starts "tunastream.exe --<job>-helper <pipe> ..." as the user, on the user's
 * desktop, and reads what it sends back on a named pipe that only SYSTEM, administrators and
 * the user can open, after checking the right program connected.
 *
 * Jobs: --sound-helper <pipe> <pid>: the PC's sound without one program's (Share my screen's
 * game sound only), as 48 kHz stereo float samples.
 */
// standard includes
#include <filesystem>
#include <random>
#include <string>

// platform includes
#include <sddl.h>
#include <UserEnv.h>
#include <windows.h>

// local includes
#include "app_sound.h"
#include "misc.h"
#include "src/logging.h"
#include "user_helper.h"

using namespace std::literals;

namespace platf {
  user_helper_t::~user_helper_t() {
    if (pipe != INVALID_HANDLE_VALUE) {
      CancelIoEx(pipe, nullptr);
      CloseHandle(pipe);
    }
    if (process) {
      if (WaitForSingleObject(process, 1000) != WAIT_OBJECT_0) {
        TerminateProcess(process, 0);
      }
      CloseHandle(process);
    }
  }

  bool user_helper_t::read(void *buffer, std::size_t size, std::size_t &got, const std::atomic_bool &stop) {
    got = 0;
    OVERLAPPED ov {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = false;
    if (ReadFile(pipe, buffer, (DWORD) size, nullptr, &ov) || GetLastError() == ERROR_IO_PENDING || GetLastError() == ERROR_MORE_DATA) {
      while (true) {
        DWORD wait = WaitForSingleObject(ov.hEvent, 100);
        if (wait == WAIT_OBJECT_0) {
          DWORD n = 0;
          ok = GetOverlappedResult(pipe, &ov, &n, FALSE) || GetLastError() == ERROR_MORE_DATA;
          got = n;
          break;
        }
        if (stop.load()) {
          CancelIoEx(pipe, &ov);
          DWORD n = 0;
          GetOverlappedResult(pipe, &ov, &n, TRUE);
          break;
        }
      }
    }
    CloseHandle(ov.hEvent);
    return ok && got > 0;
  }

  std::unique_ptr<user_helper_t> start_user_helper(const std::string &job, const std::wstring &args) {
    HANDLE token = retrieve_users_token(false);
    if (!token) {
      return nullptr;
    }
    std::random_device rd;
    std::wstring name = L"TunaStreamHelper-";
    for (int i = 0; i < 4; ++i) {
      wchar_t part[9];
      swprintf(part, 9, L"%08x", (unsigned) rd());
      name += part;
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sd, nullptr)) {
      CloseHandle(token);
      return nullptr;
    }
    SECURITY_ATTRIBUTES sa {sizeof(sa), sd, FALSE};
    auto helper = std::make_unique<user_helper_t>();
    helper->pipe = CreateNamedPipeW((L"\\\\.\\pipe\\" + name).c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 256 * 1024, 256 * 1024, 0, &sa);
    LocalFree(sd);
    if (helper->pipe == INVALID_HANDLE_VALUE) {
      CloseHandle(token);
      return nullptr;
    }

    std::wstring exe(32768, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), (DWORD) exe.size()));
    const auto dir = std::filesystem::path {exe}.parent_path().wstring();
    std::wstring cmd = L"\"" + exe + L"\" --" + from_utf8(job) + L"-helper " + name + L" " + args;
    STARTUPINFOW si {};
    si.cb = sizeof(si);
    wchar_t desktop[] = L"winsta0\\default";
    si.lpDesktop = desktop;
    void *env = nullptr;
    CreateEnvironmentBlock(&env, token, FALSE);
    PROCESS_INFORMATION pi {};
    const BOOL started = CreateProcessAsUserW(token, exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env, dir.c_str(), &si, &pi);
    if (env) {
      DestroyEnvironmentBlock(env);
    }
    CloseHandle(token);
    if (!started) {
      BOOST_LOG(warning) << "Couldn't start the "sv << job << " helper: "sv << GetLastError();
      return nullptr;
    }
    CloseHandle(pi.hThread);
    helper->process = pi.hProcess;

    // Wait for it to connect, and make sure it's our helper that did
    OVERLAPPED ov {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = false;
    if (ConnectNamedPipe(helper->pipe, &ov) || GetLastError() == ERROR_PIPE_CONNECTED) {
      connected = true;
    } else if (GetLastError() == ERROR_IO_PENDING) {
      HANDLE waits[2] = {ov.hEvent, helper->process};
      DWORD n = 0;
      if (WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0) {
        connected = GetOverlappedResult(helper->pipe, &ov, &n, FALSE);
      } else {
        CancelIoEx(helper->pipe, &ov);
        GetOverlappedResult(helper->pipe, &ov, &n, TRUE);
      }
    }
    CloseHandle(ov.hEvent);
    ULONG client = 0;
    if (!connected || !GetNamedPipeClientProcessId(helper->pipe, &client) || client != pi.dwProcessId) {
      BOOST_LOG(warning) << "The "sv << job << " helper didn't connect"sv;
      return nullptr;
    }
    return helper;
  }

  int sound_helper_main(int argc, char *argv[]) {
    // tunastream.exe --sound-helper <pipe> <pid>
    if (argc < 4) {
      return 2;
    }
    const auto path = L"\\\\.\\pipe\\" + from_utf8(argv[2]);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 50 && pipe == INVALID_HANDLE_VALUE; ++i) {
      pipe = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (pipe == INVALID_HANDLE_VALUE) {
        Sleep(100);
      }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
      return 3;
    }
    std::atomic_bool stop {false};
    const auto pid = (std::uint32_t) std::strtoul(argv[3], nullptr, 10);
    const bool ok = sound_without(pid, [&](const float *samples, std::size_t frames) {
      DWORD written = 0;
      const DWORD bytes = (DWORD) (frames * 2 * sizeof(float));
      if (!WriteFile(pipe, samples, bytes, &written, nullptr) || written != bytes) {
        stop = true;  // TunaStream stopped listening
      }
    }, stop);
    CloseHandle(pipe);
    return ok ? 0 : 4;
  }
}  // namespace platf
