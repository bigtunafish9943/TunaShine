/**
 * @file src/platform/windows/cheer_overlay.cpp
 * @brief Cheers from viewers on the PC's screen: a small card in the corner of the shared screen
 *        with the reaction and who sent it, for a few seconds. It lets clicks through, never takes
 *        focus, and shows in the shared picture too, so the viewer sees it arrive.
 */
// standard includes
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// platform includes
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

// local includes
#include "cheer_overlay.h"
#include "misc.h"
#include "src/logging.h"

using namespace std::literals;

namespace {
  constexpr int WIDTH = 340;
  constexpr int HEIGHT = 92;
  constexpr int MARGIN = 32;
  constexpr UINT SHOW_MS = 3500;
  constexpr UINT FADE_STEP_MS = 30;
  constexpr UINT WM_TUNA_CHEER = WM_APP + 1;
  // D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT (Windows 8.1): emoji in colour
  constexpr auto COLOR_FONT = (D2D1_DRAW_TEXT_OPTIONS) 4;

  struct cheer_t {
    std::wstring emoji;
    std::wstring text;
    std::wstring monitor;
  };

  std::mutex lock;
  std::deque<cheer_t> pending;
  HWND window = nullptr;
  std::once_flag started;

  RECT monitor_rect(const std::wstring &device) {
    struct find_t {
      std::wstring device;
      RECT rect {};
      bool found = false;
    } f {device};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
      auto f = (find_t *) p;
      MONITORINFOEXW info {};
      info.cbSize = sizeof(info);
      if (GetMonitorInfoW(m, &info) && (f->device.empty() ? (info.dwFlags & MONITORINFOF_PRIMARY) != 0 : f->device == info.szDevice)) {
        f->rect = info.rcMonitor;
        f->found = true;
        return FALSE;
      }
      return TRUE;
    }, (LPARAM) &f);
    if (!f.found) {
      f.rect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    return f.rect;
  }

  struct painter_t {
    ID2D1Factory *factory = nullptr;
    IDWriteFactory *dwrite = nullptr;
    ID2D1DCRenderTarget *target = nullptr;
    IDWriteTextFormat *emoji_format = nullptr;
    IDWriteTextFormat *text_format = nullptr;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    bool ok = false;

    painter_t() {
      if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void **) &factory)) ||
          FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **) &dwrite))) {
        return;
      }
      D2D1_RENDER_TARGET_PROPERTIES props {};
      props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
      props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
      props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
      if (FAILED(factory->CreateDCRenderTarget(&props, &target))) {
        return;
      }
      dwrite->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 44.0f, L"en-us", &emoji_format);
      dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", &text_format);
      if (!emoji_format || !text_format) {
        return;
      }
      emoji_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      DWRITE_TRIMMING trim {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      text_format->SetTrimming(&trim, nullptr);

      BITMAPINFO bi {};
      bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
      bi.bmiHeader.biWidth = WIDTH;
      bi.bmiHeader.biHeight = -HEIGHT;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      void *bits = nullptr;
      dc = CreateCompatibleDC(nullptr);
      bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
      if (!dc || !bitmap) {
        return;
      }
      old = SelectObject(dc, bitmap);
      ok = true;
    }

    ~painter_t() {
      if (old) {
        SelectObject(dc, old);
      }
      if (bitmap) {
        DeleteObject(bitmap);
      }
      if (dc) {
        DeleteDC(dc);
      }
      for (IUnknown *p : std::initializer_list<IUnknown *> {text_format, emoji_format, target, dwrite, factory}) {
        if (p) {
          p->Release();
        }
      }
    }

    bool paint(const cheer_t &c) {
      RECT r {0, 0, WIDTH, HEIGHT};
      if (FAILED(target->BindDC(dc, &r))) {
        return false;
      }
      ID2D1SolidColorBrush *card = nullptr;
      ID2D1SolidColorBrush *ink = nullptr;
      target->CreateSolidColorBrush(D2D1_COLOR_F {0.02f, 0.08f, 0.19f, 0.88f}, &card);
      target->CreateSolidColorBrush(D2D1_COLOR_F {1.0f, 1.0f, 1.0f, 1.0f}, &ink);
      target->BeginDraw();
      target->Clear(D2D1_COLOR_F {0, 0, 0, 0});
      if (card && ink) {
        D2D1_ROUNDED_RECT box {{1.0f, 1.0f, WIDTH - 1.0f, HEIGHT - 1.0f}, 18.0f, 18.0f};
        target->FillRoundedRectangle(box, card);
        D2D1_RECT_F emoji_box {16.0f, 0.0f, 86.0f, (float) HEIGHT};
        target->DrawText(c.emoji.c_str(), (UINT32) c.emoji.size(), emoji_format, emoji_box, ink, COLOR_FONT);
        D2D1_RECT_F text_box {94.0f, 0.0f, WIDTH - 18.0f, (float) HEIGHT};
        target->DrawText(c.text.c_str(), (UINT32) c.text.size(), text_format, text_box, ink, D2D1_DRAW_TEXT_OPTIONS_CLIP);
      }
      const bool drawn = SUCCEEDED(target->EndDraw());
      if (card) {
        card->Release();
      }
      if (ink) {
        ink->Release();
      }
      return drawn;
    }
  };

  painter_t *painter = nullptr;
  BYTE alpha = 255;
  POINT position {};

  void present() {
    SIZE size {WIDTH, HEIGHT};
    POINT source {0, 0};
    BLENDFUNCTION blend {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(window, nullptr, &position, &size, painter->dc, &source, 0, &blend, ULW_ALPHA);
  }

  void show_next() {
    cheer_t c;
    {
      std::lock_guard lg {lock};
      if (pending.empty()) {
        return;
      }
      c = std::move(pending.front());
      pending.pop_front();
    }
    if (!painter || !painter->ok || !painter->paint(c)) {
      return;
    }
    const auto r = monitor_rect(c.monitor);
    position = {r.right - WIDTH - MARGIN, r.bottom - HEIGHT - MARGIN * 3};
    alpha = 255;
    present();
    ShowWindow(window, SW_SHOWNOACTIVATE);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    KillTimer(window, 2);
    SetTimer(window, 1, SHOW_MS, nullptr);
  }

  LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_TUNA_CHEER:
        show_next();
        return 0;
      case WM_TIMER:
        if (wp == 1) {
          KillTimer(hwnd, 1);
          SetTimer(hwnd, 2, FADE_STEP_MS, nullptr);
        } else if (wp == 2) {
          if (alpha <= 25) {
            KillTimer(hwnd, 2);
            ShowWindow(hwnd, SW_HIDE);
            show_next();  // the next cheer, if more came in
          } else {
            alpha -= 25;
            present();
          }
        }
        return 0;
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  void run() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Show on the desktop the user sees
    if (auto desk = OpenInputDesktop(0, FALSE, GENERIC_ALL)) {
      SetThreadDesktop(desk);
    }
    painter = new painter_t;
    WNDCLASSW wc {};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TunaStreamCheer";
    RegisterClassW(&wc);
    const HWND created = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"TunaStream", WS_POPUP, 0, 0, WIDTH, HEIGHT, nullptr, nullptr, wc.hInstance, nullptr);
    {
      std::lock_guard lg {lock};
      window = created;
    }
    if (!created || !painter->ok) {
      BOOST_LOG(warning) << "Cheers: can't show them on this PC's screen"sv;
      return;
    }
    PostMessageW(created, WM_TUNA_CHEER, 0, 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
}  // namespace

namespace platf {
  void show_cheer(const std::string &monitor, const std::string &emoji, const std::string &text) {
    {
      std::lock_guard lg {lock};
      if (pending.size() >= 5) {
        pending.pop_front();  // a flurry: the latest ones matter
      }
      pending.push_back({from_utf8(emoji), from_utf8(text), from_utf8(monitor)});
    }
    std::call_once(started, []() {
      std::thread {run}.detach();
    });
    HWND w;
    {
      std::lock_guard lg {lock};
      w = window;
    }
    if (w && !IsWindowVisible(w)) {
      PostMessageW(w, WM_TUNA_CHEER, 0, 0);
    }
  }
}  // namespace platf
