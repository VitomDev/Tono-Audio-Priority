#include "core/headset.h"

#include "core/paths.h"

#include <bcrypt.h>
#include <urlmon.h>

#include <fstream>

namespace tono {

namespace {

// The text of the string value right after `key`, searching from `from`.
std::string StringAfter(const std::string& text, const std::string& key, size_t from = 0) {
    const size_t at = text.find("\"" + key + "\"", from);
    if (at == std::string::npos) return {};
    const size_t colon = text.find(':', at);
    if (colon == std::string::npos) return {};
    const size_t open = text.find('"', colon);
    if (open == std::string::npos) return {};
    const size_t close = text.find('"', open + 1);
    if (close == std::string::npos) return {};
    return text.substr(open + 1, close - open - 1);
}

uint16_t Hex(const std::string& text) {
    try {
        return static_cast<uint16_t>(std::stoul(text, nullptr, 16));
    } catch (...) {
        return 0;
    }
}

// The top-level objects of the "devices" array.
std::vector<std::string> DeviceObjects(const std::string& json) {
    std::vector<std::string> objects;
    size_t at = json.find("\"devices\"");
    if (at == std::string::npos) return objects;
    at = json.find('[', at);
    if (at == std::string::npos) return objects;
    int depth = 0;
    bool inString = false;
    size_t start = 0;
    for (size_t i = at + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (inString) {
            if (c == '\\') ++i;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') {
            if (depth++ == 0) start = i;
        } else if (c == '}') {
            if (--depth == 0) objects.push_back(json.substr(start, i - start + 1));
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

}  // namespace

std::vector<HeadsetReading> ParseHeadsetJson(const std::string& json) {
    std::vector<HeadsetReading> readings;
    for (const std::string& device : DeviceObjects(json)) {
        HeadsetReading reading;
        reading.vid = Hex(StringAfter(device, "id_vendor"));
        reading.pid = Hex(StringAfter(device, "id_product"));
        if (!reading.vid) continue;
        const size_t battery = device.find("\"battery\"");
        const std::string level = battery == std::string::npos ? std::string{} : StringAfter(device, "status", battery);
        const size_t errors = device.find("\"errors\"");
        const std::string error = errors == std::string::npos ? std::string{} : device.substr(errors);
        if (level == "BATTERY_AVAILABLE" || level == "BATTERY_CHARGING") {
            reading.state = HeadsetState::On;
        } else if (error.find("offline") != std::string::npos) {
            reading.state = HeadsetState::Off;
        }
        readings.push_back(reading);
    }
    return readings;
}

std::filesystem::path HeadsetControlPath() { return DataFolder() / L"headsetcontrol.exe"; }

std::string Sha256File(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::string hex;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buffer(64 * 1024);
        bool ok = true;
        while (ok && file) {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto read = static_cast<ULONG>(file.gcount());
            if (read) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), read, 0) == 0;
        }
        UCHAR digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char digits[] = "0123456789abcdef";
            for (UCHAR byte : digest) {
                hex += digits[byte >> 4];
                hex += digits[byte & 15];
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return hex;
}

bool IsHeadsetControlReady() {
    static std::mutex mutex;
    static std::filesystem::file_time_type checkedTime;
    static bool checkedOk = false;
    std::error_code error;
    const auto path = HeadsetControlPath();
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) return false;
    std::lock_guard lock(mutex);
    // Hashed once per file version, not on every poll.
    if (time != checkedTime) {
        checkedTime = time;
        checkedOk = Sha256File(path) == kHeadsetControlSha256;
    }
    return checkedOk;
}

bool DownloadHeadsetControl() {
    std::error_code error;
    std::filesystem::create_directories(DataFolder(), error);
    const auto target = HeadsetControlPath();
    const auto temporary = target.wstring() + L".download";
    std::filesystem::remove(temporary, error);
    if (FAILED(URLDownloadToFileW(nullptr, kHeadsetControlUrl, temporary.c_str(), 0, nullptr))) return false;
    if (Sha256File(temporary) != kHeadsetControlSha256) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return IsHeadsetControlReady();
}

bool QueryHeadsets(std::vector<HeadsetReading>& readings) {
    readings.clear();
    if (!IsHeadsetControlReady()) return false;

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &security, 64 * 1024)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;
    startup.hStdInput = nullptr;
    std::wstring command = L"\"" + HeadsetControlPath().wstring() + L"\" -b -o json";
    PROCESS_INFORMATION process{};
    const std::wstring folder = DataFolder().wstring();
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        folder.c_str(), &startup, &process);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        return false;
    }

    std::string output;
    char buffer[4096];
    DWORD read = 0;
    const ULONGLONG deadline = GetTickCount64() + 8000;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr)) break;  // Closed: done
        if (available) {
            if (!ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) || !read) break;
            output.append(buffer, read);
            continue;
        }
        if (GetTickCount64() > deadline) {
            TerminateProcess(process.hProcess, 1);
            break;
        }
        if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) {
            // Exited: collect whatever is left.
            while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) && available &&
                   ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) && read) {
                output.append(buffer, read);
            }
            break;
        }
    }
    WaitForSingleObject(process.hProcess, 2000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readEnd);
    readings = ParseHeadsetJson(output);
    return output.find("\"devices\"") != std::string::npos;
}

// --- Poller ---------------------------------------------------------------------------

void HeadsetPoller::Start(HWND window) {
    if (thread_.joinable()) return;
    window_ = window;
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
}

void HeadsetPoller::Stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void HeadsetPoller::SetActive(bool active, unsigned intervalMs) {
    {
        std::lock_guard lock(mutex_);
        if (active == active_ && intervalMs == interval_) return;
        active_ = active;
        interval_ = intervalMs;
    }
    wake_.notify_all();
}

std::vector<HeadsetReading> HeadsetPoller::Latest() {
    std::lock_guard lock(mutex_);
    return latest_;
}

void HeadsetPoller::Run() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        if (!active_) {
            wake_.wait(lock, [this] { return stop_ || active_; });
            continue;
        }
        lock.unlock();
        std::vector<HeadsetReading> readings;
        const bool ran = QueryHeadsets(readings);
        lock.lock();
        if (stop_) break;
        if (ran) {
            latest_ = std::move(readings);
            PostMessageW(window_, WM_APP_HEADSETS, 0, 0);
        }
        wake_.wait_for(lock, std::chrono::milliseconds(interval_), [this] { return stop_; });
    }
}

}  // namespace tono
