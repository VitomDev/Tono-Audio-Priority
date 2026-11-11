#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tono {

// HeadsetControl (https://github.com/Sapd/HeadsetControl, GPL v3) is not part
// of Tono: it is downloaded on request from its official release, checked
// against this hash and run as a separate program.
inline constexpr wchar_t kHeadsetControlUrl[] =
    L"https://github.com/Sapd/HeadsetControl/releases/download/4.2.0/headsetcontrol-windows-x86_64.exe";
inline constexpr char kHeadsetControlSha256[] = "81acae4c083aaa40a2e9c1bce7b3dd9625e74b9d9ee96384bb43b2dff7ff0fa5";
inline constexpr wchar_t kHeadsetControlVersion[] = L"4.2.0";

enum class HeadsetState { Unknown, On, Off };

struct HeadsetReading {
    uint16_t vid = 0;
    uint16_t pid = 0;
    HeadsetState state = HeadsetState::Unknown;
};

// Reads `headsetcontrol -b -o json`. On: the battery answered. Off: the
// headset is "offline or not responding". Anything else, such as a timeout
// while it is waking up, is Unknown.
std::vector<HeadsetReading> ParseHeadsetJson(const std::string& json);

std::filesystem::path HeadsetControlPath();
// Present and matching the pinned hash.
bool IsHeadsetControlReady();
// Blocking: downloads to a temporary file, checks the hash, then moves it in place.
bool DownloadHeadsetControl();
// Blocking, at most a few seconds. False when it could not be run.
bool QueryHeadsets(std::vector<HeadsetReading>& readings);

std::string Sha256File(const std::filesystem::path& path);

inline constexpr UINT WM_APP_HEADSETS = WM_APP + 22;

// Asks HeadsetControl every few seconds, only while some output needs it,
// on its own thread. Each round posts WM_APP_HEADSETS; Latest() has it.
class HeadsetPoller {
public:
    ~HeadsetPoller() { Stop(); }
    void Start(HWND window);
    void Stop();
    // Nothing to ask for stops the polling; `intervalMs` while it runs.
    void SetActive(bool active, unsigned intervalMs);
    std::vector<HeadsetReading> Latest();

private:
    void Run();

    HWND window_ = nullptr;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    bool active_ = false;
    unsigned interval_ = 3000;
    std::vector<HeadsetReading> latest_;
};

}  // namespace tono
