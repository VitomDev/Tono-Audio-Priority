#include "app/tray_app.h"

#include <commctrl.h>
#include <objbase.h>

#include <cwchar>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    // "--background" is used by the Start with Windows entry: start silently.
    const bool startedAtSignIn = wcsstr(commandLine, L"--background") != nullptr;

    // One instance per user session. A second launch only opens the panel of
    // the running one.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\TonoAudioPriority.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND running = FindWindowW(tono::kTrayWindowClass, nullptr); running && !startedAtSignIn) {
            DWORD process = 0;
            GetWindowThreadProcessId(running, &process);
            AllowSetForegroundWindow(process);
            PostMessageW(running, tono::WM_APP_ALREADY_RUNNING, 0, 0);
        }
        return 0;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    int exitCode = 0;
    {
        tono::TrayApp app;
        if (app.Initialize(instance, startedAtSignIn)) {
            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } else {
            exitCode = 1;
        }
    }
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return exitCode;
}
