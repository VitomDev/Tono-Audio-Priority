#include "app/toast.h"

#include <shellapi.h>

#include <algorithm>

namespace tono {

namespace {

constexpr wchar_t kClassName[] = L"TonoToast";
constexpr UINT_PTR kHoldTimer = 1;
constexpr UINT_PTR kFadeTimer = 2;
constexpr UINT kHoldMs = 2600;
constexpr UINT kFadeStepMs = 25;
constexpr int kFadeStep = 24;

// Layout in 96-DPI pixels.
constexpr int kHeight = 52;
constexpr int kMinWidth = 200;
constexpr int kMaxWidth = 340;
constexpr int kMargin = 12;

}  // namespace

bool CanShowToast() {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&state))) return true;
    return state == QUNS_ACCEPTS_NOTIFICATIONS || state == QUNS_APP;
}

int Toast::Scale(int value) const { return MulDiv(value, static_cast<int>(GetDpiForWindow(window_)), 96); }

void Toast::Show(HINSTANCE instance, const RECT& anchor, const wchar_t* icon, const std::wstring& title, const std::wstring& detail) {
    if (!CanShowToast()) return;
    static const ATOM windowClass = [instance] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        return RegisterClassExW(&wc);
    }();
    if (!windowClass) return;

    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& work = monitor.rcWork;
    if (!window_) {
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                  kClassName, L"Tono", WS_POPUP, work.left, work.top, 1, 1, nullptr, nullptr, instance, this);
        if (!window_) return;
    }
    icon_ = icon;
    title_ = title;
    detail_ = detail;
    alpha_ = 255;

    theme_ = Theme::Current();
    for (HFONT* font : {&titleFont_, &detailFont_, &iconFont_}) {
        if (*font) DeleteObject(*font);
    }
    const UINT dpi = GetDpiForWindow(window_);
    titleFont_ = CreateUiFont(dpi, 10, FW_SEMIBOLD);
    detailFont_ = CreateUiFont(dpi, 9);
    iconFont_ = CreateIconFont(dpi, 14);
    ApplyWindowStyle(window_, theme_);

    // As wide as the text needs, within limits.
    const int textWidth = std::max(TextWidth(window_, titleFont_, title_.c_str()), TextWidth(window_, detailFont_, detail_.c_str()));
    const int width = std::clamp(textWidth + Scale(16 + 24 + 12 + 18), Scale(kMinWidth), Scale(kMaxWidth));
    const int height = Scale(kHeight);
    const int margin = Scale(kMargin);
    const int center = (anchor.left + anchor.right) / 2;
    const int x = std::clamp<int>(center - width / 2, work.left + margin, work.right - margin - width);
    const bool top = anchor.bottom <= work.top + (work.bottom - work.top) / 2;
    const int y = top ? work.top + margin : work.bottom - margin - height;

    SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
    KillTimer(window_, kFadeTimer);
    SetTimer(window_, kHoldTimer, kHoldMs, nullptr);
}

void Toast::Close() {
    if (window_) {
        HWND window = window_;
        window_ = nullptr;
        DestroyWindow(window);
    }
    for (HFONT* font : {&titleFont_, &detailFont_, &iconFont_}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
}

void Toast::Paint(HDC dc, const RECT& client) {
    HBRUSH background = CreateSolidBrush(theme_.background);
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);

    const int left = Scale(16);
    HGDIOBJ previous = SelectObject(dc, iconFont_);
    SetTextColor(dc, theme_.accent);
    RECT icon{left, 0, left + Scale(24), client.bottom};
    DrawTextW(dc, icon_ ? icon_ : L"", -1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const int textLeft = left + Scale(36);
    const int right = client.right - Scale(16);
    const int middle = client.bottom / 2;
    SelectObject(dc, titleFont_);
    SetTextColor(dc, theme_.text);
    RECT title{textLeft, 0, right, detail_.empty() ? client.bottom : middle + Scale(2)};
    DrawTextW(dc, title_.c_str(), -1, &title,
              (detail_.empty() ? DT_VCENTER : DT_BOTTOM) | DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (!detail_.empty()) {
        SelectObject(dc, detailFont_);
        SetTextColor(dc, theme_.secondaryText);
        RECT detail{textLeft, middle + Scale(3), right, client.bottom};
        DrawTextW(dc, detail_.c_str(), -1, &detail, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, previous);
}

LRESULT CALLBACK Toast::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* toast = static_cast<Toast*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        toast->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(toast));
    }
    auto* toast = reinterpret_cast<Toast*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!toast || toast->window_ != window) return DefWindowProcW(window, message, wParam, lParam);
    return toast->HandleMessage(message, wParam, lParam);
}

LRESULT Toast::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window_, &paint);
            RECT client;
            GetClientRect(window_, &client);
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ previous = SelectObject(memory, bitmap);
            Paint(memory, client);
            BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, previous);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_TIMER:
            if (wParam == kHoldTimer) {
                KillTimer(window_, kHoldTimer);
                SetTimer(window_, kFadeTimer, kFadeStepMs, nullptr);
            } else if (wParam == kFadeTimer) {
                alpha_ -= kFadeStep;
                if (alpha_ <= 0) {
                    KillTimer(window_, kFadeTimer);
                    // Gone, and the memory of its window with it.
                    Close();
                } else {
                    SetLayeredWindowAttributes(window_, 0, static_cast<BYTE>(alpha_), LWA_ALPHA);
                }
            }
            return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace tono
