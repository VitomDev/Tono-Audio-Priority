#include "app/panel.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdlib>

namespace tono {

namespace {

constexpr wchar_t kClassName[] = L"TonoPanel";

// Layout in 96-DPI pixels.
constexpr int kWidth = 380;
constexpr int kPadding = 16;
constexpr int kHeaderHeight = 48;
constexpr int kBannerHeight = 36;
constexpr int kRowHeight = 56;
constexpr int kFooterHeight = 40;
constexpr int kTestHeight = 230;
constexpr int kMargin = 12;
constexpr int kDragThreshold = 5;

// Hot spots: rows use index * 4 + part, the rest these.
constexpr int kPartRow = 0;
constexpr int kPartToggle = 1;
constexpr int kPartMore = 2;
constexpr int kHotPause = 10000;
constexpr int kHotMenu = 10001;
constexpr int kHotBack = 10002;
constexpr int kHotTestButton = 20000;

}  // namespace

int Panel::Scale(int value) const { return MulDiv(value, static_cast<int>(GetDpiForWindow(window_)), 96); }

int Panel::ListTop() const { return Scale(kHeaderHeight) + (paused_ ? Scale(kBannerHeight) : 0); }

SIZE Panel::Measure() const {
    if (test_.active) return {Scale(kWidth), Scale(kHeaderHeight) + Scale(kTestHeight)};
    const int rows = std::max<int>(1, static_cast<int>(items_.size()));
    return {Scale(kWidth), ListTop() + Scale(kRowHeight) * rows + Scale(kFooterHeight)};
}

RECT Panel::RowRect(size_t index) const {
    const int top = ListTop() + Scale(kRowHeight) * static_cast<int>(index);
    return {0, top, Scale(kWidth), top + Scale(kRowHeight)};
}

RECT Panel::MoreRect(size_t index) const {
    const RECT row = RowRect(index);
    const int size = Scale(28);
    const int right = row.right - Scale(10);
    const int top = (row.top + row.bottom - size) / 2;
    return {right - size, top, right, top + size};
}

RECT Panel::ToggleRect(size_t index) const {
    const RECT more = MoreRect(index);
    const int width = Scale(40), height = Scale(20);
    const int right = more.left - Scale(8);
    const int top = (more.top + more.bottom - height) / 2;
    return {right - width, top, right, top + height};
}

RECT Panel::MenuRect() const {
    const int size = Scale(32);
    const int right = Scale(kWidth) - Scale(8);
    const int top = (Scale(kHeaderHeight) - size) / 2;
    return {right - size, top, right, top + size};
}

RECT Panel::PauseRect() const {
    const RECT menu = MenuRect();
    const int size = menu.right - menu.left;
    return {menu.left - size - Scale(4), menu.top, menu.left - Scale(4), menu.bottom};
}

RECT Panel::BackRect() const { return {Scale(8), Scale(8), Scale(kWidth) - Scale(8), Scale(kHeaderHeight) - Scale(8)}; }

RECT Panel::TestButtonRect(size_t index) const {
    // Right-aligned, the main action last on the right like Windows dialogs.
    const int height = Scale(32);
    const int width = Scale(170);
    const int bottom = Scale(kHeaderHeight) + Scale(kTestHeight) - Scale(kPadding);
    const int count = static_cast<int>(test_.buttons.size());
    const int position = count - 1 - static_cast<int>(index);
    const int right = Scale(kWidth) - Scale(kPadding) - position * (width + Scale(8));
    return {right - width, bottom - height, right, bottom};
}

RECT Panel::HotRect(int hot) const {
    if (hot == kHotPause) return PauseRect();
    if (hot == kHotMenu) return MenuRect();
    if (hot == kHotBack) return BackRect();
    if (hot >= kHotTestButton) return TestButtonRect(static_cast<size_t>(hot - kHotTestButton));
    return RowRect(static_cast<size_t>(hot / 4));
}

int Panel::HitTest(POINT point) const {
    if (test_.active) {
        for (size_t i = 0; i < test_.buttons.size(); ++i) {
            const RECT button = TestButtonRect(i);
            if (PtInRect(&button, point)) return kHotTestButton + static_cast<int>(i);
        }
        const RECT back = BackRect();
        return PtInRect(&back, point) ? kHotBack : -1;
    }
    const RECT pause = PauseRect(), menu = MenuRect();
    if (PtInRect(&pause, point)) return kHotPause;
    if (PtInRect(&menu, point)) return kHotMenu;
    for (size_t i = 0; i < items_.size(); ++i) {
        const RECT row = RowRect(i);
        if (!PtInRect(&row, point)) continue;
        const RECT toggle = ToggleRect(i), more = MoreRect(i);
        const int base = static_cast<int>(i) * 4;
        if (PtInRect(&more, point)) return base + kPartMore;
        if (PtInRect(&toggle, point)) return base + kPartToggle;
        return base + kPartRow;
    }
    return -1;
}

void Panel::SetHot(int hot) {
    if (hot == hot_) return;
    if (hot_ >= 0) {
        const RECT old = HotRect(hot_);
        InvalidateRect(window_, &old, FALSE);
    }
    hot_ = hot;
    if (hot_ >= 0) {
        const RECT now = HotRect(hot_);
        InvalidateRect(window_, &now, FALSE);
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        TrackMouseEvent(&track);
    }
}

size_t Panel::DropIndex() const {
    if (items_.empty()) return 0;
    const int row = Scale(kRowHeight);
    const int index = (dragY_ - ListTop() + row / 2 - (pressY_ - RowRect(static_cast<size_t>(pressed_)).top)) / row;
    return static_cast<size_t>(std::clamp(index, 0, static_cast<int>(items_.size()) - 1));
}

void Panel::RefreshStyle() {
    theme_ = Theme::Current();
    for (HFONT font : {titleFont_, labelFont_, boldFont_, detailFont_, iconFont_, bigIconFont_}) {
        if (font) DeleteObject(font);
    }
    const UINT dpi = GetDpiForWindow(window_);
    titleFont_ = CreateUiFont(dpi, 11, FW_SEMIBOLD);
    labelFont_ = CreateUiFont(dpi, 10);
    boldFont_ = CreateUiFont(dpi, 10, FW_SEMIBOLD);
    detailFont_ = CreateUiFont(dpi, 9);
    iconFont_ = CreateIconFont(dpi, 11);
    bigIconFont_ = CreateIconFont(dpi, 14);
    ApplyWindowStyle(window_, theme_);
}

void Panel::Show(HINSTANCE instance, const RECT& anchor, std::vector<PanelItem> items, bool paused, TestView test) {
    if (window_) {
        SetForegroundWindow(window_);
        return;
    }
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
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"Tono", WS_POPUP, work.left, work.top, 1, 1,
                              nullptr, nullptr, instance, this);
    if (!window_) return;

    items_ = std::move(items);
    paused_ = paused;
    test_ = std::move(test);
    RefreshStyle();

    const SIZE size = Measure();
    const int margin = Scale(kMargin);
    const int center = (anchor.left + anchor.right) / 2;
    const int x = std::clamp<int>(center - size.cx / 2, work.left + margin, work.right - margin - size.cx);
    anchoredTop_ = anchor.bottom <= work.top + (work.bottom - work.top) / 2;
    const int y = anchoredTop_ ? work.top + margin : work.bottom - margin - size.cy;
    SetWindowPos(window_, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_SHOWWINDOW);
    SetForegroundWindow(window_);
}

void Panel::Update(std::vector<PanelItem> items, bool paused, TestView test) {
    if (!window_) return;
    // A drag in progress keeps its rows until it is dropped.
    if (dragging_ && items.size() != items_.size()) {
        dragging_ = false;
        pressed_ = -1;
        ReleaseCapture();
    }
    const bool resize = items.size() != items_.size() || paused != paused_ || test.active != test_.active;
    if (items == items_ && paused == paused_ && test == test_) return;
    items_ = std::move(items);
    paused_ = paused;
    test_ = std::move(test);
    if (resize) {
        hot_ = -1;
        Relayout();
    } else {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void Panel::Relayout() {
    RECT bounds;
    GetWindowRect(window_, &bounds);
    const SIZE size = Measure();
    const int y = anchoredTop_ ? bounds.top : bounds.bottom - size.cy;
    SetWindowPos(window_, nullptr, bounds.left, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(window_, nullptr, FALSE);
}

void Panel::Close() {
    if (!window_) return;
    HWND window = window_;
    window_ = nullptr;
    closedAt_ = GetTickCount64();
    activated_ = false;
    hot_ = -1;
    pressed_ = -1;
    dragging_ = false;
    DestroyWindow(window);
    for (HFONT* font : {&titleFont_, &labelFont_, &boldFont_, &detailFont_, &iconFont_, &bigIconFont_}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
    std::vector<PanelItem>().swap(items_);
    test_ = {};
    if (onClosed) onClosed();
}

// --- Painting -------------------------------------------------------------------------

void Panel::PaintIconButton(HDC dc, const RECT& rect, const wchar_t* icon, bool hot) {
    if (hot) FillRounded(dc, rect, theme_.hover, Scale(4));
    SelectObject(dc, iconFont_);
    SetTextColor(dc, theme_.text);
    RECT text = rect;
    DrawTextW(dc, icon, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void Panel::PaintToggle(HDC dc, const RECT& rect, bool on, bool hot) {
    const int height = rect.bottom - rect.top;
    const int radius = height / 2;
    const int knob = on ? height - Scale(8) : height - Scale(10);
    if (on) {
        COLORREF fill = theme_.accent;
        if (hot) fill = RGB(GetRValue(fill) * 9 / 10, GetGValue(fill) * 9 / 10, GetBValue(fill) * 9 / 10);
        FillRounded(dc, rect, fill, radius);
    } else {
        FillRounded(dc, rect, hot ? theme_.hover : theme_.background, radius);
        OutlineRounded(dc, rect, theme_.secondaryText, radius);
    }
    const int cy = (rect.top + rect.bottom) / 2;
    const int cx = on ? rect.right - radius : rect.left + radius;
    const RECT dot{cx - knob / 2, cy - knob / 2, cx + knob / 2 + 1, cy + knob / 2 + 1};
    FillRounded(dc, dot, on ? theme_.accentText : theme_.secondaryText, knob / 2 + 1);
}

void Panel::PaintRow(HDC dc, const PanelItem& item, const RECT& rect, size_t index, bool lifted) {
    const int padding = Scale(kPadding);
    const int base = static_cast<int>(index) * 4;
    const bool rowHot = !dragging_ && hot_ == base + kPartRow;

    if (lifted) {
        RECT card{rect.left + Scale(6), rect.top + Scale(2), rect.right - Scale(6), rect.bottom - Scale(2)};
        FillRounded(dc, card, theme_.hover, Scale(6));
        OutlineRounded(dc, card, theme_.divider, Scale(6));
    } else if (rowHot || item.highlight) {
        RECT highlight{rect.left + Scale(6), rect.top + Scale(2), rect.right - Scale(6), rect.bottom - Scale(2)};
        FillRounded(dc, highlight, theme_.hover, Scale(4));
        if (item.highlight) OutlineRounded(dc, highlight, theme_.accent, Scale(4));
    }
    if (item.inUse) {
        // The accent mark of the selected item in Windows 11 lists.
        const int height = Scale(18);
        const int top = (rect.top + rect.bottom - height) / 2;
        FillRounded(dc, {rect.left + Scale(6), top, rect.left + Scale(9), top + height}, theme_.accent, Scale(2));
    }

    SelectObject(dc, iconFont_);
    SetTextColor(dc, theme_.secondaryText);
    RECT grip{rect.left + padding - Scale(2), rect.top, rect.left + padding + Scale(14), rect.bottom};
    DrawTextW(dc, icons::kGrip, -1, &grip, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, bigIconFont_);
    SetTextColor(dc, item.inUse ? theme_.accent : item.enabled && item.available ? theme_.text : theme_.secondaryText);
    RECT icon{rect.left + padding + Scale(22), rect.top, rect.left + padding + Scale(46), rect.bottom};
    DrawTextW(dc, item.icon, -1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const RECT toggle = ToggleRect(index);
    const int textLeft = rect.left + padding + Scale(56);
    const int textRight = toggle.left - Scale(10);
    const int middle = (rect.top + rect.bottom) / 2;
    SelectObject(dc, item.inUse ? boldFont_ : labelFont_);
    SetTextColor(dc, item.enabled ? theme_.text : theme_.secondaryText);
    RECT name{textLeft, rect.top + Scale(8), textRight, middle + Scale(1)};
    DrawTextW(dc, item.name.c_str(), -1, &name, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, detailFont_);
    SetTextColor(dc, item.inUse ? theme_.accent : theme_.secondaryText);
    RECT detail{textLeft, middle + Scale(2), textRight, rect.bottom - Scale(6)};
    DrawTextW(dc, item.detail.c_str(), -1, &detail, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    PaintToggle(dc, toggle, item.enabled, !dragging_ && hot_ == base + kPartToggle);
    PaintIconButton(dc, MoreRect(index), icons::kMore, !dragging_ && hot_ == base + kPartMore);
}

void Panel::PaintTest(HDC dc) {
    const int padding = Scale(kPadding);
    const int right = Scale(kWidth) - padding;
    int top = Scale(kHeaderHeight) + Scale(4);

    if (test_.done) {
        SelectObject(dc, bigIconFont_);
        SetTextColor(dc, test_.success ? theme_.accent : theme_.secondaryText);
        RECT mark{padding, top, padding + Scale(24), top + Scale(24)};
        DrawTextW(dc, test_.success ? icons::kCheck : icons::kWarning, -1, &mark, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, labelFont_);
    SetTextColor(dc, theme_.text);
    RECT message{padding + (test_.done ? Scale(32) : 0), top, right, top + Scale(130)};
    DrawTextW(dc, test_.message.c_str(), -1, &message, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    SelectObject(dc, detailFont_);
    SetTextColor(dc, theme_.secondaryText);
    RECT status{padding, top + Scale(136), right, top + Scale(160)};
    DrawTextW(dc, test_.status.c_str(), -1, &status, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    for (size_t i = 0; i < test_.buttons.size(); ++i) {
        const RECT button = TestButtonRect(i);
        const bool primary = i == 0;
        const bool hot = hot_ == kHotTestButton + static_cast<int>(i);
        if (primary) {
            COLORREF fill = theme_.accent;
            if (hot) fill = RGB(GetRValue(fill) * 9 / 10, GetGValue(fill) * 9 / 10, GetBValue(fill) * 9 / 10);
            FillRounded(dc, button, fill, Scale(4));
            SetTextColor(dc, theme_.accentText);
        } else {
            FillRounded(dc, button, hot ? theme_.hover : theme_.pressed, Scale(4));
            OutlineRounded(dc, button, theme_.divider, Scale(4));
            SetTextColor(dc, theme_.text);
        }
        SelectObject(dc, labelFont_);
        RECT text = button;
        DrawTextW(dc, test_.buttons[i].second.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

void Panel::Paint(HDC dc, const RECT& client) {
    HBRUSH background = CreateSolidBrush(theme_.background);
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ previous = SelectObject(dc, titleFont_);
    const int padding = Scale(kPadding);

    RECT title{padding, 0, Scale(kWidth) - padding, Scale(kHeaderHeight)};
    if (test_.active) {
        const RECT back = BackRect();
        if (hot_ == kHotBack) FillRounded(dc, back, theme_.hover, Scale(4));
        SelectObject(dc, iconFont_);
        SetTextColor(dc, theme_.secondaryText);
        DrawTextW(dc, icons::kBack, -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        title.left += Scale(26);
        SelectObject(dc, titleFont_);
        SetTextColor(dc, theme_.text);
        DrawTextW(dc, test_.title.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        PaintTest(dc);
        SelectObject(dc, previous);
        return;
    }

    SetTextColor(dc, theme_.text);
    DrawTextW(dc, L"Audio priority", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    PaintIconButton(dc, PauseRect(), paused_ ? icons::kPlay : icons::kPause, hot_ == kHotPause);
    PaintIconButton(dc, MenuRect(), icons::kMore, hot_ == kHotMenu);

    if (paused_) {
        RECT banner{Scale(8), Scale(kHeaderHeight), Scale(kWidth) - Scale(8), Scale(kHeaderHeight) + Scale(kBannerHeight) - Scale(6)};
        FillRounded(dc, banner, theme_.hover, Scale(4));
        SelectObject(dc, detailFont_);
        SetTextColor(dc, theme_.text);
        banner.left += Scale(10);
        DrawTextW(dc, L"Paused: Tono will not switch outputs until you resume.", -1, &banner,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (items_.empty()) {
        RECT empty = RowRect(0);
        empty.left += padding;
        SelectObject(dc, labelFont_);
        SetTextColor(dc, theme_.secondaryText);
        DrawTextW(dc, L"No audio outputs found.", -1, &empty, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (dragging_) {
        // The other rows close up around the place the lifted one would drop into.
        const size_t from = static_cast<size_t>(pressed_);
        const size_t to = DropIndex();
        size_t slot = 0;
        for (size_t i = 0; i < items_.size(); ++i) {
            if (i == from) continue;
            if (slot == to) ++slot;
            PaintRow(dc, items_[i], RowRect(slot), i, false);
            ++slot;
        }
        RECT lifted = RowRect(from);
        const int offset = dragY_ - pressY_;
        OffsetRect(&lifted, 0, offset);
        // The toggle and menu are drawn where the row is, not where it was.
        HRGN clip = CreateRectRgnIndirect(&lifted);
        SelectClipRgn(dc, clip);
        HBRUSH under = CreateSolidBrush(theme_.background);
        FillRect(dc, &lifted, under);
        DeleteObject(under);
        SetViewportOrgEx(dc, 0, offset, nullptr);
        PaintRow(dc, items_[from], RowRect(from), from, true);
        SetViewportOrgEx(dc, 0, 0, nullptr);
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
    } else {
        RECT overlap;
        for (size_t i = 0; i < items_.size(); ++i) {
            const RECT rect = RowRect(i);
            if (IntersectRect(&overlap, &rect, &client)) PaintRow(dc, items_[i], rect, i, false);
        }
    }

    const int listBottom = ListTop() + Scale(kRowHeight) * std::max<int>(1, static_cast<int>(items_.size()));
    RECT footer{padding, listBottom, Scale(kWidth) - padding, listBottom + Scale(kFooterHeight)};
    SelectObject(dc, detailFont_);
    SetTextColor(dc, theme_.secondaryText);
    DrawTextW(dc, L"Drag to reorder. Tono uses the first output that is on.", -1, &footer,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, previous);
}

// --- Window procedure -------------------------------------------------------------------

LRESULT CALLBACK Panel::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* panel = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        panel->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
    }
    auto* panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!panel || panel->window_ != window) return DefWindowProcW(window, message, wParam, lParam);
    return panel->HandleMessage(message, wParam, lParam);
}

LRESULT Panel::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window_, &paint);
            RECT client;
            GetClientRect(window_, &client);
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ previous = SelectObject(memory, bitmap);
            Paint(memory, dragging_ ? client : paint.rcPaint);
            BitBlt(dc, paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right - paint.rcPaint.left,
                   paint.rcPaint.bottom - paint.rcPaint.top, memory, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
            SelectObject(memory, previous);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                const bool clickable = hot_ >= kHotPause || (hot_ >= 0 && hot_ % 4 != kPartRow);
                SetCursor(LoadCursorW(nullptr, dragging_ ? IDC_SIZENS : clickable ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
            if (pressed_ >= 0 && (wParam & MK_LBUTTON)) {
                if (!dragging_ && std::abs(point.y - pressY_) >= Scale(kDragThreshold)) {
                    dragging_ = true;
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                }
                if (dragging_) {
                    const int listTop = ListTop();
                    const int listBottom = listTop + Scale(kRowHeight) * static_cast<int>(items_.size());
                    const int grab = pressY_ - RowRect(static_cast<size_t>(pressed_)).top;
                    dragY_ = std::clamp<int>(point.y, listTop + grab, listBottom - Scale(kRowHeight) + grab);
                    InvalidateRect(window_, nullptr, FALSE);
                }
                return 0;
            }
            SetHot(HitTest(point));
            return 0;
        case WM_MOUSELEAVE:
            if (!dragging_) SetHot(-1);
            return 0;
        case WM_LBUTTONDOWN: {
            const int hit = HitTest(point);
            if (!test_.active && hit >= 0 && hit < kHotPause && hit % 4 == kPartRow) {
                pressed_ = hit / 4;
                pressY_ = dragY_ = point.y;
                SetCapture(window_);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (pressed_ >= 0) {
                const size_t from = static_cast<size_t>(pressed_);
                const bool moved = dragging_;
                const size_t to = moved ? DropIndex() : from;
                pressed_ = -1;
                dragging_ = false;
                ReleaseCapture();
                InvalidateRect(window_, nullptr, FALSE);
                if (moved && to != from && onMove) onMove(from, to);
                return 0;
            }
            const int hit = HitTest(point);
            if (hit < 0) return 0;
            POINT screen = point;
            ClientToScreen(window_, &screen);
            if (hit == kHotPause) {
                if (onPause) onPause();
            } else if (hit == kHotMenu) {
                if (onMenu) onMenu(screen);
            } else if (hit == kHotBack) {
                if (onTestBack) onTestBack();
            } else if (hit >= kHotTestButton) {
                const size_t index = static_cast<size_t>(hit - kHotTestButton);
                if (index < test_.buttons.size() && onTestButton) onTestButton(test_.buttons[index].first);
            } else if (hit % 4 == kPartToggle) {
                if (onToggle) onToggle(static_cast<size_t>(hit / 4));
            } else if (hit % 4 == kPartMore) {
                if (onItemMenu) onItemMenu(static_cast<size_t>(hit / 4), screen);
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            const int hit = HitTest(point);
            if (!test_.active && hit >= 0 && hit < kHotPause && onItemMenu) {
                POINT screen = point;
                ClientToScreen(window_, &screen);
                onItemMenu(static_cast<size_t>(hit / 4), screen);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != window_ && pressed_ >= 0) {
                pressed_ = -1;
                dragging_ = false;
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                if (dragging_) {
                    pressed_ = -1;
                    dragging_ = false;
                    ReleaseCapture();
                    InvalidateRect(window_, nullptr, FALSE);
                } else if (test_.active) {
                    if (onTestBack) onTestBack();
                } else {
                    Close();
                }
            }
            return 0;
        case WM_ACTIVATE:
            // Closes when clicking elsewhere, like the system flyouts, except
            // during a test: the user is busy with the headset.
            if (LOWORD(wParam) != WA_INACTIVE) {
                activated_ = true;
            } else if (activated_ && !test_.active) {
                PostMessageW(window_, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_CLOSE:
            Close();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            RefreshStyle();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED:
            Close();
            return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace tono
