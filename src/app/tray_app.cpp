#include "app/tray_app.h"
#include "app/vitool_tray.h"

#include "app/resource.h"
#include "app/theme.h"
#include "core/paths.h"
#include "version.h"

#include <objbase.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

namespace tono {

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 10;
constexpr UINT WM_APP_OPEN_PANEL = WM_APP + 14;
constexpr UINT WM_APP_DOWNLOADED = WM_APP + 23;

constexpr UINT_PTR kDecideTimer = 1;
constexpr UINT_PTR kTestTimer = 2;
constexpr UINT_PTR kReadyTimer = 3;

// Bluetooth and monitors going to sleep flicker; this lets them settle.
constexpr UINT kSettleMs = 2500;
// After sign-in the audio service may still be setting up outputs.
constexpr UINT kSignInDelayMs = 10000;
constexpr unsigned kPollMs = 3000;
constexpr unsigned kTestPollMs = 2000;
constexpr ULONGLONG kOffWaitMs = 20000;
constexpr ULONGLONG kOnWaitMs = 60000;

enum MenuId {
    kMenuOpen = 1,
    kMenuPause,
    kMenuCommunications,
    kMenuToasts,
    kMenuAutostart,
    kMenuAbout,
    kMenuExit,
    kMenuManualFirst = 100,  // One per output in manual mode
};

enum ItemMenuId {
    kItemTest = 1,
    kItemAutomatic,
    kItemHeadset,
    kItemManual,
    kItemMarkOn,
    kItemTop,
    kItemBottom,
    kItemForget,
};

enum TestButtonId { kTestDownload = 1, kTestManual, kTestClose, kTestRetry };

uint32_t HeadsetKey(uint16_t vid, uint16_t pid) { return (static_cast<uint32_t>(vid) << 16) | pid; }

void TrimWorkingSet() { SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1)); }

// The logo in grey while paused.
HICON CreateTrayIcon(int size, bool paused) {
    HICON logo = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_TONO), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    if (!logo || !paused) return logo;
    ICONINFO info{};
    if (!GetIconInfo(logo, &info)) return logo;
    HICON grey = nullptr;
    BITMAPINFO bitmap{};
    bitmap.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    std::vector<uint32_t> pixels(static_cast<size_t>(size) * size);
    HDC dc = GetDC(nullptr);
    if (info.hbmColor && GetDIBits(dc, info.hbmColor, 0, size, pixels.data(), &bitmap, DIB_RGB_COLORS) == size) {
        for (uint32_t& pixel : pixels) {
            const uint32_t r = (pixel >> 16) & 255, g = (pixel >> 8) & 255, b = pixel & 255;
            const uint32_t luma = (r * 299 + g * 587 + b * 114) / 1000;
            const uint32_t alpha = (pixel >> 24) * 3 / 4;
            pixel = (alpha << 24) | (luma << 16) | (luma << 8) | luma;
        }
        void* bits = nullptr;
        HBITMAP color = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (color) {
            std::copy(pixels.begin(), pixels.end(), static_cast<uint32_t*>(bits));
            ICONINFO greyInfo{TRUE, 0, 0, info.hbmMask, color};
            grey = CreateIconIndirect(&greyInfo);
            DeleteObject(color);
        }
    }
    ReleaseDC(nullptr, dc);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (!grey) return logo;
    DestroyIcon(logo);
    return grey;
}

const wchar_t* DetectionName(Detection detection) {
    switch (detection) {
        case Detection::Untested: return L"Not tested";
        case Detection::Automatic: return L"Automatic";
        case Detection::HeadsetControl: return L"HeadsetControl";
        case Detection::Manual: return L"Manual";
    }
    return L"";
}

}  // namespace

bool TrayApp::Initialize(HINSTANCE instance, bool startedAtSignIn) {
    instance_ = instance;
    settings_ = Settings::Load();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kTrayWindowClass;
    if (!RegisterClassExW(&wc)) return false;
    // A hidden top-level window: it must receive the TaskbarCreated broadcast,
    // and the installer finds it by class.
    window_ = CreateWindowExW(0, kTrayWindowClass, L"Tono Audio Priority", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!window_) return false;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    EnableDarkMenus();

    panel_.onClosed = [this] {
        if (test_.active) EndTest();
        highlight_.clear();
        TrimWorkingSet();
    };
    panel_.onMove = [this](size_t from, size_t to) {
        MoveEntry(settings_.devices, from, to);
        Save();
        RefreshPanel();
        ScheduleDecision(500);
    };
    panel_.onToggle = [this](size_t index) {
        if (index >= settings_.devices.size()) return;
        settings_.devices[index].enabled = !settings_.devices[index].enabled;
        Save();
        RefreshPanel();
        ScheduleDecision(500);
    };
    panel_.onItemMenu = [this](size_t index, POINT point) { ShowItemMenu(index, point); };
    panel_.onPause = [this] { SetPaused(!settings_.paused); };
    panel_.onMenu = [this](POINT point) { ShowTrayMenu(point, panel_.window()); };
    panel_.onTestButton = [this](int id) { OnTestButton(id); };
    panel_.onTestBack = [this] { EndTest(); };

    watcher_.Start(window_);
    poller_.Start(window_);
    currentDefault_ = DefaultOutput();
    intended_ = currentDefault_;
    RefreshOutputs(false);
    AddTrayIcon();

    if (startedAtSignIn) {
        SetTimer(window_, kReadyTimer, kSignInDelayMs, nullptr);
    } else {
        ready_ = true;
        ScheduleDecision(1000);
    }

    if (ConsumeFirstRun()) {
        PostMessageW(window_, WM_APP_OPEN_PANEL, 0, 0);
    } else if (!startedAtSignIn) {
        Notify(L"Tono is running in the notification area. Click its icon to set the order of your audio outputs.");
    }
    TrimWorkingSet();
    return true;
}

TrayApp::~TrayApp() {
    if (download_.joinable()) download_.join();
    if (currentIcon_) DestroyIcon(currentIcon_);
}

// --- Outputs and decisions -------------------------------------------------------------

const EndpointInfo* TrayApp::FindEndpoint(const std::wstring& id) const {
    for (const EndpointInfo& endpoint : endpoints_) {
        if (endpoint.id == id) return &endpoint;
    }
    return nullptr;
}

DeviceEntry* TrayApp::FindEntry(const std::wstring& id) {
    for (DeviceEntry& entry : settings_.devices) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

std::wstring TrayApp::NameOf(const std::wstring& id) const {
    for (const DeviceEntry& entry : settings_.devices) {
        if (entry.id == id) return DisplayName(entry.name, settings_.devices);
    }
    for (const EndpointInfo& endpoint : endpoints_) {
        if (endpoint.id == id) return DisplayName(endpoint.name, settings_.devices);
    }
    return L"another output";
}

void TrayApp::RefreshOutputs(bool announceNew) {
    endpoints_ = EnumerateOutputs();
    const std::vector<DeviceEntry> before = settings_.devices;
    const bool firstList = !settings_.configured;
    const std::vector<std::wstring> added = MergeEndpoints(settings_.devices, endpoints_);
    if (firstList && !endpoints_.empty()) {
        // The first list starts with what the user is using now.
        settings_.configured = true;
        for (size_t i = 0; i < settings_.devices.size(); ++i) {
            if (settings_.devices[i].id == currentDefault_) MoveEntry(settings_.devices, i, 0);
        }
    } else if (announceNew && !added.empty()) {
        highlight_ = added.back();
        Notify(L"New audio output: " + NameOf(added.back()) +
               L". It was added at the end of your list. Click here to put it where you want.");
    }
    if (settings_.devices != before || firstList) Save();
    UpdatePolling();
    UpdateTray();
    RefreshPanel();
}

HeadsetState TrayApp::HeadsetStateOf(const DeviceEntry& entry) const {
    const auto it = headsetStates_.find(HeadsetKey(entry.vid, entry.pid));
    return it == headsetStates_.end() ? HeadsetState::Unknown : it->second;
}

Availability TrayApp::AvailabilityOf(const DeviceEntry& entry) const {
    const EndpointInfo* endpoint = FindEndpoint(entry.id);
    if (!endpoint || !endpoint->active) return Availability::No;
    switch (entry.detection) {
        case Detection::HeadsetControl:
            switch (HeadsetStateOf(entry)) {
                case HeadsetState::On: return Availability::Yes;
                case HeadsetState::Off: return Availability::No;
                case HeadsetState::Unknown: return Availability::Unknown;
            }
            break;
        case Detection::Manual: return entry.manualOn ? Availability::Yes : Availability::No;
        case Detection::Untested:
        case Detection::Automatic: break;
    }
    return Availability::Yes;
}

void TrayApp::ScheduleDecision(UINT delayMs) {
    if (exiting_) return;
    // Every new event restarts the wait, so a burst ends in a single decision.
    SetTimer(window_, kDecideTimer, delayMs, nullptr);
}

void TrayApp::Decide() {
    KillTimer(window_, kDecideTimer);
    if (settings_.paused || !ready_ || exiting_) return;
    const Choice choice = ChooseTarget(settings_.devices, [this](const DeviceEntry& entry) { return AvailabilityOf(entry); });
    // Unsure, or nothing on the list can be used: leave things as they are.
    if (!choice.decided || choice.id.empty()) return;

    currentDefault_ = DefaultOutput();
    const bool communications = settings_.communications && DefaultCommunicationsOutput() != choice.id;
    if (currentDefault_ != choice.id || communications) {
        ownSwitch_ = choice.id;
        ownSwitchAt_ = GetTickCount64();
        if (!SetDefaultOutput(choice.id, settings_.communications)) {
            ownSwitch_.clear();
            Notify(L"Tono could not switch to " + NameOf(choice.id) + L". Your output was left as it was.");
            return;
        }
        currentDefault_ = choice.id;
    }
    // Also when Windows' own fallback happened to be the right one: the user
    // still wants to know where the sound went.
    if (choice.id != intended_) {
        std::wstring reason = L"First on your list that is on";
        if (!intended_.empty()) {
            const DeviceEntry* before = FindEntry(intended_);
            const bool beforeUsable = before && AvailabilityOf(*before) == Availability::Yes;
            reason = beforeUsable ? L"Higher on your list than " + NameOf(intended_) : NameOf(intended_) + L" is off";
        }
        ShowToast(IconOf(choice.id), NameOf(choice.id), reason);
    }
    intended_ = choice.id;
    UpdateTray();
    RefreshPanel();
}

const wchar_t* TrayApp::IconOf(const std::wstring& id) const {
    const EndpointInfo* endpoint = FindEndpoint(id);
    return !endpoint || endpoint->needsTest ? icons::kHeadphones : icons::kSpeakers;
}

void TrayApp::ShowToast(const wchar_t* icon, const std::wstring& title, const std::wstring& detail) {
    // The open panel already shows it.
    if (!settings_.toasts || panel_.IsOpen() || exiting_) return;
    NOTIFYICONIDENTIFIER id{sizeof(id), window_, icon_.uID};
    RECT anchor;
    if (FAILED(Shell_NotifyIconGetRect(&id, &anchor))) {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY), &monitor);
        anchor = {monitor.rcWork.right - 1, monitor.rcWork.bottom - 1, monitor.rcWork.right, monitor.rcWork.bottom};
    }
    toast_.Show(instance_, anchor, icon, title, detail);
}

void TrayApp::OnDefaultChanged() {
    const std::wstring now = DefaultOutput();
    if (now == currentDefault_) return;
    const std::wstring previous = currentDefault_;
    currentDefault_ = now;
    const bool ours = now == ownSwitch_ && GetTickCount64() - ownSwitchAt_ < 10000;
    if (!ours && !previous.empty()) {
        // Someone else picked an output: the user, a game, SteamVR. If the one
        // in use is still fine, that choice wins over a decision still waiting.
        // If it went away, Windows only fell back to something: Tono decides.
        const DeviceEntry* entry = nullptr;
        for (const DeviceEntry& candidate : settings_.devices) {
            if (candidate.id == previous) entry = &candidate;
        }
        const bool stillUsable = IsOutputActive(previous) && (!entry || AvailabilityOf(*entry) != Availability::No);
        if (stillUsable) {
            KillTimer(window_, kDecideTimer);
            intended_ = now;
        }
    } else if (ours) {
        intended_ = now;
    }
    UpdateTray();
    RefreshPanel();
}

void TrayApp::OnHeadsets() {
    lastReadings_ = poller_.Latest();
    bool changed = false;
    for (const HeadsetReading& reading : lastReadings_) {
        if (reading.state == HeadsetState::Unknown) continue;  // Keep the last definite answer
        HeadsetState& known = headsetStates_[HeadsetKey(reading.vid, reading.pid)];
        if (known == reading.state) continue;
        known = reading.state;
        for (const DeviceEntry& entry : settings_.devices) {
            if (entry.enabled && entry.detection == Detection::HeadsetControl && entry.vid == reading.vid && entry.pid == reading.pid) {
                changed = true;
            }
        }
    }
    if (changed) ScheduleDecision(kSettleMs);
    if (test_.active) TestTick();
    UpdateTray();
    RefreshPanel();
}

void TrayApp::UpdatePolling() {
    bool needed = false;
    for (const DeviceEntry& entry : settings_.devices) {
        if (entry.enabled && entry.detection == Detection::HeadsetControl && entry.vid) needed = true;
    }
    const bool testing = test_.active && test_.probeHeadset;
    poller_.SetActive((needed || testing) && IsHeadsetControlReady(), testing ? kTestPollMs : kPollMs);
}

void TrayApp::Save() {
    if (!settings_.Save()) Notify(L"Tono could not save your list.");
}

// --- Tray icon ----------------------------------------------------------------------------

void TrayApp::AddTrayIcon() {
    icon_ = {sizeof(icon_)};
    icon_.hWnd = window_;
    icon_.uID = 1;
    icon_.uFlags = NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    icon_.uCallbackMessage = WM_APP_TRAY;
    wcscpy_s(icon_.szTip, L"Tono Audio Priority");
    vitool_tray::NotifyIcon(NIM_ADD, &icon_);
    icon_.uVersion = NOTIFYICON_VERSION_4;
    vitool_tray::NotifyIcon(NIM_SETVERSION, &icon_);
    tooltip_.clear();
    UpdateTray(true);
}

void TrayApp::UpdateTray(bool forceIcon) {
    if (exiting_ || !icon_.hWnd) return;
    if (forceIcon || iconPaused_ != settings_.paused || !currentIcon_) {
        const int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(window_));
        if (HICON icon = CreateTrayIcon(size, settings_.paused)) {
            NOTIFYICONDATAW data{sizeof(data)};
            data.hWnd = window_;
            data.uID = icon_.uID;
            data.uFlags = NIF_ICON;
            data.hIcon = icon;
            vitool_tray::NotifyIcon(NIM_MODIFY, &data);
            if (currentIcon_) DestroyIcon(currentIcon_);
            currentIcon_ = icon;
            iconPaused_ = settings_.paused;
        }
    }
    std::wstring tooltip = L"Tono Audio Priority\n" + (currentDefault_.empty() ? std::wstring(L"No output in use") : NameOf(currentDefault_));
    if (settings_.paused) tooltip += L"\nPaused";
    if (tooltip != tooltip_) {
        tooltip_ = tooltip;
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window_;
        data.uID = icon_.uID;
        data.uFlags = NIF_TIP | NIF_SHOWTIP;
        wcsncpy_s(data.szTip, tooltip_.c_str(), _TRUNCATE);
        vitool_tray::NotifyIcon(NIM_MODIFY, &data);
    }
}

void TrayApp::ShowTrayMenu(POINT point, HWND owner) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open Tono");
    AppendMenuW(menu, MF_STRING | (settings_.paused ? MF_CHECKED : 0), kMenuPause, L"Pause switching");
    // Outputs Windows cannot tell about are marked on and off from here.
    std::vector<std::wstring> manual;
    for (const DeviceEntry& entry : settings_.devices) {
        if (!entry.enabled || entry.detection != Detection::Manual) continue;
        if (manual.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const std::wstring label = NameOf(entry.id) + L" is on";
        AppendMenuW(menu, MF_STRING | (entry.manualOn ? MF_CHECKED : 0), kMenuManualFirst + manual.size(), label.c_str());
        manual.push_back(entry.id);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (settings_.communications ? MF_CHECKED : 0), kMenuCommunications,
                L"Also switch the communications output");
    AppendMenuW(menu, MF_STRING | (settings_.toasts ? MF_CHECKED : 0), kMenuToasts, L"Small notifications");
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), kMenuAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, kMenuAbout, L"About Tono");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit Tono");
    SetMenuDefaultItem(menu, kMenuOpen, FALSE);

    SetForegroundWindow(owner);
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, point.x, point.y, owner, nullptr);
    DestroyMenu(menu);
    if (command >= kMenuManualFirst && command < kMenuManualFirst + manual.size()) {
        if (DeviceEntry* entry = FindEntry(manual[command - kMenuManualFirst])) {
            entry->manualOn = !entry->manualOn;
            Save();
            RefreshPanel();
            ScheduleDecision(500);
        }
        return;
    }
    switch (command) {
        case kMenuOpen: ShowPanel(); break;
        case kMenuPause: SetPaused(!settings_.paused); break;
        case kMenuCommunications:
            settings_.communications = !settings_.communications;
            Save();
            break;
        case kMenuToasts:
            settings_.toasts = !settings_.toasts;
            Save();
            break;
        case kMenuAutostart:
            if (!SetAutostartEnabled(!IsAutostartEnabled(), ExecutablePath().wstring())) {
                Notify(L"Could not change Start with Windows.");
            }
            break;
        case kMenuAbout: ShowAbout(); break;
        case kMenuExit: Exit(); break;
    }
}

void TrayApp::ShowItemMenu(size_t index, POINT point) {
    if (index >= settings_.devices.size() || test_.active) return;
    const DeviceEntry entry = settings_.devices[index];
    const EndpointInfo* endpoint = FindEndpoint(entry.id);
    const auto radio = [&entry](Detection detection) {
        return MF_STRING | ((entry.detection == detection ||
                             (detection == Detection::Automatic && entry.detection == Detection::Untested))
                                ? MF_CHECKED
                                : 0);
    };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (endpoint && endpoint->active ? 0 : MF_GRAYED), kItemTest, L"Test detection…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, radio(Detection::Automatic), kItemAutomatic, L"Windows notices when it is off");
    AppendMenuW(menu, radio(Detection::HeadsetControl) | (entry.vid ? 0 : MF_GRAYED), kItemHeadset, L"Ask it with HeadsetControl");
    AppendMenuW(menu, radio(Detection::Manual), kItemManual, L"I mark it on or off myself");
    if (entry.detection == Detection::Manual) {
        AppendMenuW(menu, MF_STRING | (entry.manualOn ? MF_CHECKED : 0), kItemMarkOn, L"It is on");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (index == 0 ? MF_GRAYED : 0), kItemTop, L"Move to top");
    AppendMenuW(menu, MF_STRING | (index + 1 == settings_.devices.size() ? MF_GRAYED : 0), kItemBottom, L"Move to bottom");
    if (!endpoint || !endpoint->present) AppendMenuW(menu, MF_STRING, kItemForget, L"Remove from the list");

    HWND owner = panel_.IsOpen() ? panel_.window() : window_;
    const int command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, owner, nullptr);
    DestroyMenu(menu);
    DeviceEntry* target = FindEntry(entry.id);
    if (!target || command == 0) return;
    switch (command) {
        case kItemTest: StartTest(entry.id); return;
        case kItemAutomatic: target->detection = Detection::Automatic; break;
        case kItemHeadset:
            if (!IsHeadsetControlReady()) {
                // Downloading it is offered, with an explanation, by the test.
                StartTest(entry.id);
                return;
            }
            target->detection = Detection::HeadsetControl;
            break;
        case kItemManual: target->detection = Detection::Manual; break;
        case kItemMarkOn: target->manualOn = !target->manualOn; break;
        case kItemTop: MoveEntry(settings_.devices, index, 0); break;
        case kItemBottom: MoveEntry(settings_.devices, index, settings_.devices.size() - 1); break;
        case kItemForget: settings_.devices.erase(settings_.devices.begin() + static_cast<std::ptrdiff_t>(index)); break;
    }
    Save();
    UpdatePolling();
    RefreshPanel();
    ScheduleDecision(500);
}

void TrayApp::Notify(const std::wstring& message) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_;
    data.uID = icon_.uID;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcscpy_s(data.szInfoTitle, L"Tono Audio Priority");
    wcsncpy_s(data.szInfo, message.c_str(), _TRUNCATE);
    vitool_tray::NotifyIcon(NIM_MODIFY, &data);
}

void TrayApp::SetPaused(bool paused) {
    settings_.paused = paused;
    Save();
    UpdateTray();
    RefreshPanel();
    ShowToast(paused ? icons::kPause : icons::kPlay, paused ? L"Tono paused" : L"Tono resumed",
              paused ? L"Outputs will not be switched" : L"Following your list again");
    if (!paused) ScheduleDecision(500);
}

void TrayApp::ShowAbout() {
    const std::wstring text =
        L"Tono Audio Priority " TONO_VERSION_STRING_W L"\n"
        L"Copyright (c) 2026 VitomDev. MIT License.\n"
        L"https://github.com/VitomDev/Tono-Audio-Priority\n\n"
        L"Optional: HeadsetControl " + std::wstring(kHeadsetControlVersion) +
        L" by Denis Arnst (Sapd) and contributors, GPL v3.\n"
        L"https://github.com/Sapd/HeadsetControl\n"
        L"It is not part of Tono: it is only downloaded, from its official page, if you ask for it, and runs as a separate program.";
    MessageBoxW(nullptr, text.c_str(), L"About Tono", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

// --- Panel ---------------------------------------------------------------------------------

std::vector<PanelItem> TrayApp::PanelItems() const {
    std::vector<PanelItem> items;
    for (const DeviceEntry& entry : settings_.devices) {
        const EndpointInfo* endpoint = FindEndpoint(entry.id);
        PanelItem item;
        item.name = NameOf(entry.id);
        item.icon = !endpoint || endpoint->needsTest ? icons::kHeadphones : icons::kSpeakers;
        item.enabled = entry.enabled;
        item.inUse = entry.id == currentDefault_;
        const Availability availability = AvailabilityOf(entry);
        item.available = availability == Availability::Yes;
        item.highlight = entry.id == highlight_;
        std::wstring state;
        if (!entry.enabled) state = L"Never used";
        else if (item.inUse) state = L"In use";
        else if (!endpoint || !endpoint->present) state = L"Not connected";
        else if (!endpoint->active) state = L"Unplugged";
        else if (availability == Availability::Yes) state = L"Ready";
        else if (availability == Availability::Unknown) state = IsHeadsetControlReady() ? L"Checking…" : L"HeadsetControl is missing";
        else state = entry.detection == Detection::Manual ? L"Marked off" : L"Off";
        item.detail = state + L" · " + DetectionName(entry.detection);
        items.push_back(std::move(item));
    }
    return items;
}

TestView TrayApp::CurrentTestView() const {
    TestView view;
    if (!test_.active) return view;
    view.active = true;
    const std::wstring name = NameOf(test_.id);
    view.title = L"Test · " + name;
    const ULONGLONG now = GetTickCount64();
    const auto secondsLeft = [&] {
        return std::to_wstring(test_.deadline > now ? (test_.deadline - now + 999) / 1000 : 0);
    };
    switch (test_.step) {
        case TestStep::WaitOff:
            view.message = L"Turn off " + name + L", or put it away in its case.\n\nTono is watching whether " +
                           (test_.probeHeadset ? L"Windows or HeadsetControl notice." : L"Windows notices.");
            view.status = L"Waiting… " + secondsLeft() + L" s";
            view.buttons = {{kTestClose, L"Cancel"}};
            break;
        case TestStep::WaitOn:
            view.message = (test_.viaHeadset ? L"HeadsetControl" : L"Windows") + std::wstring(L" noticed it turning off.\n\nNow turn ") +
                           name + L" back on.";
            view.status = L"Waiting… " + secondsLeft() + L" s";
            view.buttons = {{kTestClose, L"Cancel"}};
            break;
        case TestStep::Offer:
            view.message = L"Windows still shows " + name +
                           L" as connected, so it does not notice when it is off.\n\nHeadsetControl, a free open-source tool, can "
                           L"ask many wireless headsets directly. Tono can download it from its official GitHub page (1.3 MB) and "
                           L"run it every few seconds while this output needs it.";
            view.buttons = {{kTestDownload, L"Download HeadsetControl"}, {kTestManual, L"Mark it myself"}};
            break;
        case TestStep::Downloading:
            view.message = L"Downloading HeadsetControl " + std::wstring(kHeadsetControlVersion) +
                           L" from github.com/Sapd/HeadsetControl and checking it is the genuine file…";
            break;
        case TestStep::DownloadFailed:
            view.message = L"HeadsetControl could not be downloaded, or the file was not the expected one. Nothing was kept.";
            view.buttons = {{kTestRetry, L"Try again"}, {kTestManual, L"Mark it myself"}};
            break;
        case TestStep::Done:
            view.done = true;
            view.success = test_.success;
            view.message = test_.result;
            view.buttons = {{kTestClose, L"Done"}};
            break;
    }
    return view;
}

void TrayApp::RefreshPanel() {
    if (panel_.IsOpen()) panel_.Update(PanelItems(), settings_.paused, CurrentTestView());
}

void TrayApp::TogglePanel() {
    if (panel_.IsOpen()) {
        if (!test_.active) panel_.Close();
    } else if (!panel_.JustClosed()) {
        ShowPanel();
    }
}

void TrayApp::ShowPanel() {
    if (exiting_) return;
    if (panel_.IsOpen()) {
        SetForegroundWindow(panel_.window());
        return;
    }
    NOTIFYICONIDENTIFIER id{sizeof(id), window_, icon_.uID};
    RECT anchor;
    if (FAILED(Shell_NotifyIconGetRect(&id, &anchor))) {
        POINT cursor;
        GetCursorPos(&cursor);
        anchor = {cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
    }
    currentDefault_ = DefaultOutput();
    panel_.Show(instance_, anchor, PanelItems(), settings_.paused, CurrentTestView());
}

// --- Test ------------------------------------------------------------------------------------

void TrayApp::StartTest(const std::wstring& id) {
    const DeviceEntry* entry = FindEntry(id);
    if (!entry) return;
    test_ = {};
    test_.active = true;
    test_.id = id;
    test_.step = TestStep::WaitOff;
    test_.deadline = GetTickCount64() + kOffWaitMs;
    test_.probeHeadset = entry->vid && IsHeadsetControlReady();
    // Old answers must not count: only what is seen from now on.
    lastReadings_.clear();
    SetTimer(window_, kTestTimer, 500, nullptr);
    UpdatePolling();
    if (!panel_.IsOpen()) ShowPanel();
    RefreshPanel();
}

void TrayApp::TestTick() {
    if (!test_.active) return;
    const DeviceEntry* entry = FindEntry(test_.id);
    if (!entry) {
        EndTest();
        return;
    }
    const bool active = IsOutputActive(test_.id);
    HeadsetState headset = HeadsetState::Unknown;
    for (const HeadsetReading& reading : lastReadings_) {
        if (reading.vid == entry->vid && reading.pid == entry->pid) headset = reading.state;
    }
    const ULONGLONG now = GetTickCount64();
    const std::wstring name = NameOf(entry->id);
    switch (test_.step) {
        case TestStep::WaitOff:
            if (!active) {
                test_.viaHeadset = false;
                test_.step = TestStep::WaitOn;
                test_.deadline = now + kOnWaitMs;
            } else if (test_.probeHeadset && headset == HeadsetState::Off) {
                test_.viaHeadset = true;
                test_.step = TestStep::WaitOn;
                test_.deadline = now + kOnWaitMs;
            } else if (now > test_.deadline) {
                if (entry->vid && !IsHeadsetControlReady()) {
                    test_.step = TestStep::Offer;
                } else {
                    FinishTest(false, Detection::Manual,
                               L"Neither Windows nor HeadsetControl noticed " + name +
                                   L" turning off, so Tono cannot tell on its own.\n\nIt is now in manual mode: mark it on or "
                                   L"off from Tono's menu (right-click the tray icon).");
                }
            }
            break;
        case TestStep::WaitOn:
            if (test_.viaHeadset ? headset == HeadsetState::On : active) {
                FinishTest(true, test_.viaHeadset ? Detection::HeadsetControl : Detection::Automatic,
                           test_.viaHeadset ? L"Done! HeadsetControl can tell when " + name +
                                                  L" turns on or off, and Tono will ask it every few seconds."
                                            : L"Done! Windows notices when " + name + L" comes and goes, so Tono follows it right away.");
            } else if (now > test_.deadline) {
                FinishTest(false, entry->detection,
                           L"Tono did not see " + name + L" come back. Turn it on and try the test again.");
            }
            break;
        case TestStep::Offer:
        case TestStep::Downloading:
        case TestStep::DownloadFailed:
        case TestStep::Done:
            break;
    }
    RefreshPanel();
}

void TrayApp::FinishTest(bool success, Detection detection, std::wstring result) {
    if (DeviceEntry* entry = FindEntry(test_.id)) {
        entry->detection = detection;
        Save();
    }
    test_.step = TestStep::Done;
    test_.success = success;
    test_.result = std::move(result);
    test_.probeHeadset = false;
    KillTimer(window_, kTestTimer);
    UpdatePolling();
    ScheduleDecision(kSettleMs);
}

void TrayApp::OnTestButton(int id) {
    switch (id) {
        case kTestDownload:
        case kTestRetry: StartDownload(); break;
        case kTestManual: {
            const std::wstring name = NameOf(test_.id);
            FinishTest(false, Detection::Manual,
                       L"" + name + L" is now in manual mode: mark it on or off from Tono's menu (right-click the tray icon).");
            break;
        }
        case kTestClose: EndTest(); return;
    }
    RefreshPanel();
}

void TrayApp::EndTest() {
    if (!test_.active) return;
    test_ = {};
    KillTimer(window_, kTestTimer);
    UpdatePolling();
    RefreshPanel();
}

void TrayApp::StartDownload() {
    if (test_.step == TestStep::Downloading) return;
    if (download_.joinable()) download_.join();
    test_.step = TestStep::Downloading;
    HWND window = window_;
    download_ = std::thread([window] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool ok = DownloadHeadsetControl();
        if (SUCCEEDED(com)) CoUninitialize();
        PostMessageW(window, WM_APP_DOWNLOADED, ok ? 1 : 0, 0);
    });
    RefreshPanel();
}

// --- Exit ------------------------------------------------------------------------------------

void TrayApp::Exit() {
    exiting_ = true;
    panel_.onClosed = nullptr;
    panel_.Close();
    toast_.Close();
    KillTimer(window_, kDecideTimer);
    KillTimer(window_, kTestTimer);
    KillTimer(window_, kReadyTimer);
    watcher_.Stop();
    poller_.Stop();
    vitool_tray::NotifyIcon(NIM_DELETE, &icon_);
    DestroyWindow(window_);
}

// --- Window procedure ------------------------------------------------------------------------

LRESULT CALLBACK TrayApp::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* app = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return app ? app->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT TrayApp::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (vitool_tray::HandleMessage(message, wParam)) return 0;
    if (message == taskbarCreatedMessage_ && taskbarCreatedMessage_ != 0) {
        AddTrayIcon();
        return 0;
    }
    switch (message) {
        case WM_APP_OUTPUTS_CHANGED:
            if (exiting_) return 0;
            RefreshOutputs(true);
            ScheduleDecision(kSettleMs);
            if (test_.active) TestTick();
            return 0;
        case WM_APP_DEFAULT_CHANGED:
            if (!exiting_) OnDefaultChanged();
            return 0;
        case WM_APP_HEADSETS:
            if (!exiting_) OnHeadsets();
            return 0;
        case WM_APP_DOWNLOADED:
            if (download_.joinable()) download_.join();
            if (!test_.active || test_.step != TestStep::Downloading) return 0;
            if (wParam) {
                // Ready: the headset is probably still off, which HeadsetControl will now see.
                test_.step = TestStep::WaitOff;
                test_.deadline = GetTickCount64() + kOffWaitMs;
                test_.probeHeadset = true;
                lastReadings_.clear();
                SetTimer(window_, kTestTimer, 500, nullptr);
                UpdatePolling();
            } else {
                test_.step = TestStep::DownloadFailed;
            }
            RefreshPanel();
            return 0;
        case WM_TIMER:
            if (wParam == kDecideTimer) {
                Decide();
            } else if (wParam == kTestTimer) {
                TestTick();
            } else if (wParam == kReadyTimer) {
                KillTimer(window_, kReadyTimer);
                ready_ = true;
                currentDefault_ = DefaultOutput();
                intended_ = currentDefault_;
                RefreshOutputs(false);
                Decide();
            }
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK:
                    TogglePanel();
                    break;
                case WM_CONTEXTMENU:
                    ShowTrayMenu({GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)}, window_);
                    break;
            }
            return 0;
        case WM_APP_OPEN_PANEL:
        case WM_APP_ALREADY_RUNNING:
            ShowPanel();
            return 0;
        case WM_SETTINGCHANGE:
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            UpdateTray(true);
            return 0;
        case WM_CLOSE:  // Sent by the installer before upgrading or uninstalling.
        case WM_ENDSESSION:
            if (message == WM_CLOSE || wParam) Exit();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace tono
