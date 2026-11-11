#pragma once

#include "app/theme.h"

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tono {

// One output in the panel, already worded.
struct PanelItem {
    std::wstring name;
    std::wstring detail;
    const wchar_t* icon = nullptr;
    bool enabled = true;
    bool inUse = false;
    bool available = false;
    bool highlight = false;  // Just added: the user should place it

    bool operator==(const PanelItem&) const = default;
};

// The step-by-step test of one output, shown instead of the list.
struct TestView {
    bool active = false;
    std::wstring title;
    std::wstring message;
    std::wstring status;
    bool done = false;
    bool success = false;
    std::vector<std::pair<int, std::wstring>> buttons;  // The first one is the main action

    bool operator==(const TestView&) const = default;
};

// The flyout opened from the tray icon, in the style of the Windows 11 quick
// settings: the priority list, dragged into order. It only exists while
// open, so it costs nothing when closed.
class Panel {
public:
    std::function<void()> onClosed;
    std::function<void(size_t from, size_t to)> onMove;
    std::function<void(size_t index)> onToggle;
    std::function<void(size_t index, POINT screen)> onItemMenu;
    std::function<void()> onPause;
    std::function<void(POINT screen)> onMenu;
    std::function<void(int id)> onTestButton;
    std::function<void()> onTestBack;

    bool IsOpen() const { return window_ != nullptr; }
    HWND window() const { return window_; }
    bool JustClosed() const { return GetTickCount64() - closedAt_ < 300; }

    void Show(HINSTANCE instance, const RECT& anchor, std::vector<PanelItem> items, bool paused, TestView test);
    void Update(std::vector<PanelItem> items, bool paused, TestView test);
    void Close();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void RefreshStyle();
    int Scale(int value) const;
    SIZE Measure() const;
    void Relayout();
    int ListTop() const;
    RECT RowRect(size_t index) const;
    RECT ToggleRect(size_t index) const;
    RECT MoreRect(size_t index) const;
    RECT PauseRect() const;
    RECT MenuRect() const;
    RECT BackRect() const;
    RECT TestButtonRect(size_t index) const;
    RECT HotRect(int hot) const;
    int HitTest(POINT point) const;
    void SetHot(int hot);
    size_t DropIndex() const;

    void Paint(HDC dc, const RECT& client);
    void PaintRow(HDC dc, const PanelItem& item, const RECT& rect, size_t index, bool lifted);
    void PaintToggle(HDC dc, const RECT& rect, bool on, bool hot);
    void PaintIconButton(HDC dc, const RECT& rect, const wchar_t* icon, bool hot);
    void PaintTest(HDC dc);

    HWND window_ = nullptr;
    ULONGLONG closedAt_ = 0;
    bool anchoredTop_ = false;
    std::vector<PanelItem> items_;
    bool paused_ = false;
    TestView test_;
    int hot_ = -1;
    bool activated_ = false;

    // Dragging a row: pressed at `pressY_`, lifted once moved a little.
    int pressed_ = -1;
    int pressY_ = 0;
    int dragY_ = 0;
    bool dragging_ = false;

    Theme theme_{};
    HFONT titleFont_ = nullptr;
    HFONT labelFont_ = nullptr;
    HFONT boldFont_ = nullptr;
    HFONT detailFont_ = nullptr;
    HFONT iconFont_ = nullptr;
    HFONT bigIconFont_ = nullptr;
};

}  // namespace tono
