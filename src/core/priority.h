#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tono {

// How Tono knows whether an output can be used.
enum class Detection : uint32_t {
    Untested = 0,         // Trusts Windows until the user tests it
    Automatic = 1,        // Windows notices when it goes away
    HeadsetControl = 2,   // Asked through HeadsetControl, Windows does not notice
    Manual = 3,           // The user marks it on or off
};

// One output in the user's list. The list order is the priority.
struct DeviceEntry {
    std::wstring id;    // Core Audio endpoint id
    std::wstring name;  // Friendly name, used to find it again if the id changes
    bool enabled = true;
    Detection detection = Detection::Untested;
    bool manualOn = true;
    uint16_t vid = 0;  // USB ids, for HeadsetControl
    uint16_t pid = 0;

    bool operator==(const DeviceEntry&) const = default;
};

enum class Availability { No, Yes, Unknown };

// The best output to use: the first enabled entry that is available. When an
// entry above it cannot be known right now, nothing is decided (nullopt):
// doing nothing beats switching to the wrong output.
struct Choice {
    bool decided = false;
    std::wstring id;  // Empty when nothing on the list can be used
};
Choice ChooseTarget(const std::vector<DeviceEntry>& entries, const std::function<Availability(const DeviceEntry&)>& available);

// What Windows reports for one output, reduced to what the list needs.
struct EndpointInfo {
    std::wstring id;
    std::wstring name;
    bool active = false;  // DEVICE_STATE_ACTIVE
    bool present = false; // Active or unplugged: known to be around
    bool needsTest = false;  // Headphones or headsets, the ones that can be switched off
    uint16_t vid = 0;
    uint16_t pid = 0;
};

// The hands-free profile of Bluetooth headphones: same device, worse sound.
bool IsHandsFree(const std::wstring& name);

// Reads VID_xxxx and PID_xxxx from a USB instance id.
bool ParseVidPid(const std::wstring& instanceId, uint16_t& vid, uint16_t& pid);

// Brings the saved list up to date with the outputs Windows has now. An
// entry whose id is gone takes over a new id with the same name (another USB
// port, reinstalled drivers). Outputs never seen before go to the end, and
// their ids are returned so the user can be asked where they belong.
std::vector<std::wstring> MergeEndpoints(std::vector<DeviceEntry>& entries, const std::vector<EndpointInfo>& endpoints);

// The short name shown in the panel. Windows names outputs "Speakers
// (2- Device)": the device part is what people recognize, unless another
// output shares it (two monitors on one graphics card), then the first part.
std::wstring DisplayName(const std::wstring& name, const std::vector<DeviceEntry>& all);

// Moves the entry at `from` so it ends up at `to`.
void MoveEntry(std::vector<DeviceEntry>& entries, size_t from, size_t to);

}  // namespace tono
