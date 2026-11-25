/**
 * @file src/platform/windows/app_sound.cpp
 * @brief The PC's sound without one program's: for Share my screen, so viewers hear the game
 *        but not the voice chat (Discord, TeamSpeak...). Uses Windows' process loopback capture
 *        (Windows 10 2004 and later), which leaves out a program and everything it started.
 */
// standard includes
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

// platform includes
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <TlHelp32.h>
#include <windows.h>

// local includes
#include "app_sound.h"
#include "misc.h"
#include "src/logging.h"
#include "src/utility.h"

using namespace std::literals;

namespace {
  // From audioclientactivationparams.h (Windows 10 2004 SDK), which older toolchains lack
  enum tuna_activation_type_e {
    TUNA_ACTIVATION_TYPE_DEFAULT = 0,
    TUNA_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1,
  };

  enum tuna_loopback_mode_e {
    TUNA_LOOPBACK_INCLUDE_TREE = 0,
    TUNA_LOOPBACK_EXCLUDE_TREE = 1,
  };

  struct tuna_loopback_params_t {
    DWORD target_process_id;
    tuna_loopback_mode_e mode;
  };

  struct tuna_activation_params_t {
    tuna_activation_type_e type;
    tuna_loopback_params_t loopback;
  };

  constexpr wchar_t PROCESS_LOOPBACK_DEVICE[] = L"VAD\\Process_Loopback";

  // {94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90}
  constexpr GUID IID_AGILE_OBJECT = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

  /// Waits for ActivateAudioInterfaceAsync to finish
  class activation_t: public IActivateAudioInterfaceCompletionHandler {
  public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    ~activation_t() {
      CloseHandle(done);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **out) override {
      if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) || riid == IID_AGILE_OBJECT) {
        *out = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        AddRef();
        return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
      return InterlockedIncrement(&refs);
    }

    STDMETHODIMP_(ULONG) Release() override {
      auto left = InterlockedDecrement(&refs);
      if (left == 0) {
        delete this;
      }
      return left;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *) override {
      SetEvent(done);
      return S_OK;
    }

  private:
    LONG refs = 1;
  };

  using activate_fn = HRESULT(WINAPI *)(LPCWSTR, REFIID, PROPVARIANT *, IActivateAudioInterfaceCompletionHandler *, IActivateAudioInterfaceAsyncOperation **);

  std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
      return (wchar_t) std::towlower(c);
    });
    return s;
  }
}  // namespace

namespace platf {
  std::uint32_t find_program(const std::vector<std::string> &exe_names) {
    std::vector<std::wstring> wanted;
    for (const auto &n : exe_names) {
      wanted.push_back(lower(from_utf8(n)));
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
      return 0;
    }
    struct proc_t {
      DWORD pid;
      DWORD parent;
      std::wstring exe;
    };

    std::vector<proc_t> all;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
      all.push_back({entry.th32ProcessID, entry.th32ParentProcessID, lower(entry.szExeFile)});
    }
    CloseHandle(snap);
    // The first of the program's processes: the one whose parent isn't the same program
    for (const auto &p : all) {
      if (std::find(wanted.begin(), wanted.end(), p.exe) == wanted.end()) {
        continue;
      }
      auto parent = std::find_if(all.begin(), all.end(), [&](const proc_t &x) {
        return x.pid == p.parent;
      });
      if (parent == all.end() || parent->exe != p.exe) {
        return p.pid;
      }
    }
    return 0;
  }

  bool sound_without(std::uint32_t pid, const std::function<void(const float *, std::size_t)> &sink, const std::atomic_bool &stop) {
    auto mmdevapi = LoadLibraryW(L"mmdevapi.dll");
    auto activate = mmdevapi ? (activate_fn) GetProcAddress(mmdevapi, "ActivateAudioInterfaceAsync") : nullptr;
    if (!activate) {
      BOOST_LOG(warning) << "Game sound only: this version of Windows can't leave out one program's sound"sv;
      return false;
    }
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    tuna_activation_params_t params {};
    params.type = TUNA_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.loopback.target_process_id = pid;
    params.loopback.mode = TUNA_LOOPBACK_EXCLUDE_TREE;
    PROPVARIANT pv {};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = (BYTE *) &params;

    auto handler = new activation_t;
    IActivateAudioInterfaceAsyncOperation *op = nullptr;
    IAudioClient *client = nullptr;
    IAudioCaptureClient *capture = nullptr;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool started = false;

    auto cleanup = [&]() {
      if (capture) {
        capture->Release();
      }
      if (client) {
        if (started) {
          client->Stop();
        }
        client->Release();
      }
      if (op) {
        op->Release();
      }
      handler->Release();
      CloseHandle(ready);
      if (com) {
        CoUninitialize();
      }
    };

    HRESULT hr = activate(PROCESS_LOOPBACK_DEVICE, __uuidof(IAudioClient), &pv, handler, &op);
    if (FAILED(hr) || WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0) {
      BOOST_LOG(warning) << "Game sound only: couldn't start [0x"sv << util::hex(hr).to_string_view() << ']';
      cleanup();
      return false;
    }
    HRESULT result = E_FAIL;
    IUnknown *unknown = nullptr;
    if (FAILED(op->GetActivateResult(&result, &unknown)) || FAILED(result) || !unknown) {
      BOOST_LOG(warning) << "Game sound only: Windows refused [0x"sv << util::hex(result).to_string_view() << ']';
      cleanup();
      return false;
    }
    unknown->QueryInterface(__uuidof(IAudioClient), (void **) &client);
    unknown->Release();
    if (!client) {
      cleanup();
      return false;
    }

    WAVEFORMATEX format {};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 200000, 0, &format, nullptr);
    if (FAILED(hr) || FAILED(client->SetEventHandle(ready)) || FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void **) &capture)) || FAILED(client->Start())) {
      BOOST_LOG(warning) << "Game sound only: couldn't capture [0x"sv << util::hex(hr).to_string_view() << ']';
      cleanup();
      return false;
    }
    started = true;
    BOOST_LOG(info) << "Game sound only: leaving out the sound of process "sv << pid;

    std::vector<float> silence;
    while (!stop.load()) {
      if (WaitForSingleObject(ready, 100) != WAIT_OBJECT_0) {
        continue;
      }
      UINT32 frames_ready = 0;
      while (SUCCEEDED(capture->GetNextPacketSize(&frames_ready)) && frames_ready > 0) {
        BYTE *data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
          break;
        }
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          silence.assign((std::size_t) frames * 2, 0.0f);
          sink(silence.data(), frames);
        } else {
          sink((const float *) data, frames);
        }
        capture->ReleaseBuffer(frames);
      }
    }
    cleanup();
    return true;
  }
}  // namespace platf
