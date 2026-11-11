#include "core/paths.h"

#include <windows.h>
#include <shlobj.h>

namespace tono {

std::filesystem::path ExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::filesystem::path DataFolder() {
    PWSTR local = nullptr;
    std::filesystem::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) folder = local;
    CoTaskMemFree(local);
    return folder / L"Tono Audio Priority";
}

}  // namespace tono
