#include "core/settings.h"

#include <sstream>

namespace tono {

namespace {

bool ReadDword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

bool WriteDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> parts;
    std::wstring part;
    std::wistringstream stream(text);
    while (std::getline(stream, part, separator)) parts.push_back(part);
    if (!text.empty() && text.back() == separator) parts.emplace_back();
    return parts;
}

}  // namespace

std::wstring SerializeEntry(const DeviceEntry& e) {
    wchar_t ids[16];
    swprintf_s(ids, L"%04x\t%04x", e.vid, e.pid);
    return e.id + L"\t" + e.name + L"\t" + (e.enabled ? L"1" : L"0") + L"\t" +
           std::to_wstring(static_cast<uint32_t>(e.detection)) + L"\t" + (e.manualOn ? L"1" : L"0") + L"\t" + ids;
}

bool ParseEntry(const std::wstring& line, DeviceEntry& e) {
    const std::vector<std::wstring> f = Split(line, L'\t');
    if (f.size() < 7 || f[0].empty()) return false;
    try {
        e.id = f[0];
        e.name = f[1];
        e.enabled = f[2] != L"0";
        const unsigned long detection = std::stoul(f[3]);
        e.detection = detection <= static_cast<unsigned long>(Detection::Manual) ? static_cast<Detection>(detection)
                                                                                  : Detection::Untested;
        e.manualOn = f[4] != L"0";
        e.vid = static_cast<uint16_t>(std::stoul(f[5], nullptr, 16));
        e.pid = static_cast<uint16_t>(std::stoul(f[6], nullptr, 16));
    } catch (...) {
        return false;
    }
    return true;
}

Settings Settings::Load(const wchar_t* keyPath) {
    Settings settings;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return settings;
    DWORD value = 0;
    if (ReadDword(key, L"Paused", value)) settings.paused = value != 0;
    if (ReadDword(key, L"Communications", value)) settings.communications = value != 0;
    if (ReadDword(key, L"Toasts", value)) settings.toasts = value != 0;

    DWORD size = 0;
    if (RegGetValueW(key, nullptr, L"Devices", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS) {
        settings.configured = true;
        std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
        if (RegGetValueW(key, nullptr, L"Devices", RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &size) == ERROR_SUCCESS) {
            for (const wchar_t* line = buffer.c_str(); *line; line += wcslen(line) + 1) {
                DeviceEntry entry;
                if (ParseEntry(line, entry)) settings.devices.push_back(std::move(entry));
            }
        }
    }
    RegCloseKey(key);
    return settings;
}

bool Settings::Save(const wchar_t* keyPath) const {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    std::wstring lines;
    for (const DeviceEntry& entry : devices) lines += SerializeEntry(entry) + L'\0';
    lines += L'\0';
    const bool saved = WriteDword(key, L"Paused", paused ? 1 : 0) &&
                       WriteDword(key, L"Communications", communications ? 1 : 0) &&
                       WriteDword(key, L"Toasts", toasts ? 1 : 0) &&
                       RegSetValueExW(key, L"Devices", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(lines.data()),
                                      static_cast<DWORD>(lines.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    return saved;
}

bool ConsumeFirstRun(const wchar_t* keyPath) {
    DWORD shown = 0;
    DWORD size = sizeof(shown);
    if (RegGetValueW(HKEY_CURRENT_USER, keyPath, L"FirstRunDone", RRF_RT_REG_DWORD, nullptr, &shown, &size) == ERROR_SUCCESS &&
        shown) {
        return false;
    }
    shown = 1;
    RegSetKeyValueW(HKEY_CURRENT_USER, keyPath, L"FirstRunDone", REG_DWORD, &shown, sizeof(shown));
    return true;
}

std::wstring AutostartCommand(const std::wstring& exePath) { return L"\"" + exePath + L"\" --background"; }

bool IsAutostartEnabled(const wchar_t* valueName) {
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, valueName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    // Task Manager's "Disable" keeps the Run value and flags it here instead:
    // an odd first byte means disabled.
    BYTE state[12]{};
    DWORD size = sizeof(state);
    if (RegGetValueW(HKEY_CURRENT_USER, kStartupApprovedKey, valueName, RRF_RT_REG_BINARY, nullptr, state, &size) ==
            ERROR_SUCCESS &&
        size > 0) {
        return (state[0] & 1) == 0;
    }
    return true;
}

bool SetAutostartEnabled(bool enabled, const std::wstring& exePath, const wchar_t* valueName) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS status;
    if (enabled) {
        const std::wstring command = AutostartCommand(exePath);
        status = RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, valueName);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kStartupApprovedKey, valueName);
    return status == ERROR_SUCCESS;
}

}  // namespace tono
