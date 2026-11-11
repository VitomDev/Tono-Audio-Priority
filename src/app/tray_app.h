#pragma once

#include "app/panel.h"
#include "app/toast.h"
#include "core/audio.h"
#include "core/headset.h"
#include "core/settings.h"

#include <windows.h>
#include <shellapi.h>

#include <map>
#include <string>
#include <thread>
#include <vector>

namespace tono {

inline constexpr wchar_t kTrayWindowClass[] = L"TonoAudioPriorityTray";
inline constexpr UINT WM_APP_ALREADY_RUNNING = WM_APP + 12;

// The background application. It sleeps until Windows reports an output
// connecting or disconnecting (or HeadsetControl a headset turning on or
// off), waits a moment for things to settle, then makes the first available
// output on the list the default. It never fights a choice made by the user
// or another app: that stands until the next connect or disconnect.
class TrayApp {
public:
    bool Initialize(HINSTANCE instance, bool startedAtSignIn);
    ~TrayApp();

private:
    enum class TestStep { WaitOff, WaitOn, Offer, Downloading, DownloadFailed, Done };
    struct Test {
        bool active = false;
        std::wstring id;
        TestStep step = TestStep::WaitOff;
        ULONGLONG deadline = 0;
        bool probeHeadset = false;  // HeadsetControl is asked too
        bool viaHeadset = false;    // It was HeadsetControl that noticed
        bool success = false;
        std::wstring result;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    // Outputs and decisions.
    void RefreshOutputs(bool announceNew);
    const EndpointInfo* FindEndpoint(const std::wstring& id) const;
    DeviceEntry* FindEntry(const std::wstring& id);
    HeadsetState HeadsetStateOf(const DeviceEntry& entry) const;
    Availability AvailabilityOf(const DeviceEntry& entry) const;
    void ScheduleDecision(UINT delayMs);
    void Decide();
    void OnDefaultChanged();
    void OnHeadsets();
    void UpdatePolling();
    void Save();

    // Tray and panel.
    void AddTrayIcon();
    void UpdateTray(bool forceIcon = false);
    void ShowTrayMenu(POINT point, HWND owner);
    void ShowItemMenu(size_t index, POINT point);
    void Notify(const std::wstring& message);
    void ShowToast(const wchar_t* icon, const std::wstring& title, const std::wstring& detail);
    const wchar_t* IconOf(const std::wstring& id) const;
    void TogglePanel();
    void ShowPanel();
    void RefreshPanel();
    std::vector<PanelItem> PanelItems() const;
    TestView CurrentTestView() const;
    std::wstring NameOf(const std::wstring& id) const;
    void SetPaused(bool paused);
    void ShowAbout();
    void Exit();

    // The step-by-step test of one output.
    void StartTest(const std::wstring& id);
    void TestTick();
    void FinishTest(bool success, Detection detection, std::wstring result);
    void OnTestButton(int id);
    void EndTest();
    void StartDownload();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    NOTIFYICONDATAW icon_{};
    HICON currentIcon_ = nullptr;
    bool iconPaused_ = false;
    std::wstring tooltip_;
    bool exiting_ = false;
    bool ready_ = false;  // Audio is up after sign-in; no decisions before

    Settings settings_;
    std::vector<EndpointInfo> endpoints_;
    std::map<uint32_t, HeadsetState> headsetStates_;  // Last definite answer per VID:PID
    std::vector<HeadsetReading> lastReadings_;
    std::wstring currentDefault_;
    // The output meant to be in use: Tono's pick or the user's, not the
    // fallback Windows jumps to when it goes away. Toasts name it.
    std::wstring intended_;
    std::wstring ownSwitch_;  // The output Tono itself just picked
    ULONGLONG ownSwitchAt_ = 0;
    std::wstring highlight_;  // A new output waiting to be placed

    OutputWatcher watcher_;
    HeadsetPoller poller_;
    std::thread download_;
    Panel panel_;
    Toast toast_;
    Test test_;
};

}  // namespace tono
