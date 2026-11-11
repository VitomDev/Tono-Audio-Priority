#pragma once

#include "core/priority.h"

#include <windows.h>

#include <string>
#include <vector>

namespace tono {

// All render outputs Windows knows, in any state. Needs COM on the thread.
std::vector<EndpointInfo> EnumerateOutputs();

// True when the output exists and is active right now.
bool IsOutputActive(const std::wstring& id);

// The default output for games and media (the console role).
std::wstring DefaultOutput();
std::wstring DefaultCommunicationsOutput();

// Makes `id` the default output for the console and multimedia roles, and
// for communications too when asked. Uses IPolicyConfig: undocumented, but
// what Windows' own sound settings use. Returns false and changes nothing it
// can avoid on failure.
bool SetDefaultOutput(const std::wstring& id, bool communications);

// Core Audio notifications, turned into window messages: Core Audio calls
// from its own threads, so nothing is done there.
inline constexpr UINT WM_APP_OUTPUTS_CHANGED = WM_APP + 20;
inline constexpr UINT WM_APP_DEFAULT_CHANGED = WM_APP + 21;

class OutputWatcher {
public:
    ~OutputWatcher() { Stop(); }
    bool Start(HWND window);
    void Stop();

private:
    struct Client;
    Client* client_ = nullptr;
    void* enumerator_ = nullptr;
};

}  // namespace tono
