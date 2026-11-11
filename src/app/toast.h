#pragma once

#include "app/theme.h"

#include <windows.h>

#include <string>

namespace tono {

// A small pill next to the tray that tells what Tono just did, then fades
// away. It never takes focus or clicks, and stays hidden during full-screen
// games, presentations and Do not disturb.
class Toast {
public:
    ~Toast() { Close(); }
    void Show(HINSTANCE instance, const RECT& anchor, const wchar_t* icon, const std::wstring& title, const std::wstring& detail);
    void Close();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    int Scale(int value) const;
    void Paint(HDC dc, const RECT& client);

    HWND window_ = nullptr;
    const wchar_t* icon_ = nullptr;
    std::wstring title_;
    std::wstring detail_;
    int alpha_ = 255;
    Theme theme_{};
    HFONT titleFont_ = nullptr;
    HFONT detailFont_ = nullptr;
    HFONT iconFont_ = nullptr;
};

// False while a full-screen app, a presentation or Do not disturb is on.
bool CanShowToast();

}  // namespace tono
