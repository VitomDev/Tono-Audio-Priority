#pragma once

#include "core/priority.h"

#include <windows.h>

#include <string>
#include <vector>

namespace tono {

// Registry locations. The installer and the tests rely on these names.
inline constexpr wchar_t kSettingsKey[] = L"Software\\Tono Audio Priority";
inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kRunValue[] = L"Tono Audio Priority";
inline constexpr wchar_t kStartupApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

struct Settings {
    std::vector<DeviceEntry> devices;  // In priority order
    bool paused = false;
    // Also switch the default communications output (calls, Discord).
    bool communications = false;
    // Small notes next to the tray when Tono switches or pauses.
    bool toasts = true;
    bool configured = false;  // A list was saved before

    // `keyPath` is relative to HKEY_CURRENT_USER; tests use a scratch key.
    static Settings Load(const wchar_t* keyPath = kSettingsKey);
    bool Save(const wchar_t* keyPath = kSettingsKey) const;
};

// One entry per line of a REG_MULTI_SZ value, fields separated by tabs.
std::wstring SerializeEntry(const DeviceEntry& entry);
bool ParseEntry(const std::wstring& line, DeviceEntry& entry);

bool ConsumeFirstRun(const wchar_t* keyPath = kSettingsKey);

bool IsAutostartEnabled(const wchar_t* valueName = kRunValue);
bool SetAutostartEnabled(bool enabled, const std::wstring& exePath, const wchar_t* valueName = kRunValue);
std::wstring AutostartCommand(const std::wstring& exePath);

}  // namespace tono
