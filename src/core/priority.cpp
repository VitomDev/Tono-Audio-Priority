#include "core/priority.h"

#include <algorithm>
#include <cwctype>

namespace tono {

Choice ChooseTarget(const std::vector<DeviceEntry>& entries, const std::function<Availability(const DeviceEntry&)>& available) {
    for (const DeviceEntry& entry : entries) {
        if (!entry.enabled) continue;
        switch (available(entry)) {
            case Availability::Yes: return {true, entry.id};
            case Availability::Unknown: return {false, {}};
            case Availability::No: break;
        }
    }
    return {true, {}};
}

bool IsHandsFree(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return lower.find(L"hands-free") != std::wstring::npos || lower.find(L"handsfree") != std::wstring::npos;
}

bool ParseVidPid(const std::wstring& instanceId, uint16_t& vid, uint16_t& pid) {
    std::wstring upper = instanceId;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    const auto hex = [&upper](const wchar_t* tag, uint16_t& value) {
        const size_t at = upper.find(tag);
        if (at == std::wstring::npos || at + 8 > upper.size()) return false;
        try {
            size_t used = 0;
            const unsigned long parsed = std::stoul(upper.substr(at + 4, 4), &used, 16);
            if (used != 4) return false;
            value = static_cast<uint16_t>(parsed);
            return true;
        } catch (...) {
            return false;
        }
    };
    uint16_t v = 0, p = 0;
    if (!hex(L"VID_", v) || !hex(L"PID_", p)) return false;
    vid = v;
    pid = p;
    return true;
}

std::vector<std::wstring> MergeEndpoints(std::vector<DeviceEntry>& entries, const std::vector<EndpointInfo>& endpoints) {
    const auto findId = [&entries](const std::wstring& id) {
        return std::find_if(entries.begin(), entries.end(), [&id](const DeviceEntry& e) { return e.id == id; });
    };
    const auto known = [&endpoints](const std::wstring& id) {
        return std::any_of(endpoints.begin(), endpoints.end(), [&id](const EndpointInfo& e) { return e.id == id && e.present; });
    };

    std::vector<std::wstring> added;
    for (const EndpointInfo& endpoint : endpoints) {
        if (!endpoint.present || IsHandsFree(endpoint.name)) continue;
        if (auto it = findId(endpoint.id); it != entries.end()) {
            it->name = endpoint.name;
            if (endpoint.vid) {
                it->vid = endpoint.vid;
                it->pid = endpoint.pid;
            }
            continue;
        }
        // Same name, old id no longer around: the same output under a new id.
        auto same = std::find_if(entries.begin(), entries.end(), [&](const DeviceEntry& e) {
            return e.name == endpoint.name && !known(e.id);
        });
        if (same != entries.end()) {
            same->id = endpoint.id;
            if (endpoint.vid) {
                same->vid = endpoint.vid;
                same->pid = endpoint.pid;
            }
            continue;
        }
        // Only outputs that are on right now are worth asking about.
        if (!endpoint.active) continue;
        DeviceEntry entry;
        entry.id = endpoint.id;
        entry.name = endpoint.name;
        entry.detection = endpoint.needsTest ? Detection::Untested : Detection::Automatic;
        entry.vid = endpoint.vid;
        entry.pid = endpoint.pid;
        entries.push_back(entry);
        added.push_back(endpoint.id);
    }
    return added;
}

namespace {

// "Altavoces (2- Logitech PRO X)" -> "Logitech PRO X"; empty when there are no parentheses.
std::wstring DevicePart(const std::wstring& name) {
    const size_t open = name.find(L'(');
    if (open == std::wstring::npos || name.empty() || name.back() != L')' || open + 2 > name.size()) return {};
    std::wstring part = name.substr(open + 1, name.size() - open - 2);
    size_t digits = 0;
    while (digits < part.size() && std::iswdigit(part[digits])) ++digits;
    if (digits > 0 && part.compare(digits, 2, L"- ") == 0) part.erase(0, digits + 2);
    return part;
}

std::wstring LabelPart(const std::wstring& name) {
    const size_t open = name.find(L'(');
    std::wstring label = open == std::wstring::npos ? name : name.substr(0, open);
    while (!label.empty() && label.back() == L' ') label.pop_back();
    return label;
}

}  // namespace

std::wstring DisplayName(const std::wstring& name, const std::vector<DeviceEntry>& all) {
    const std::wstring device = DevicePart(name);
    if (device.empty()) return name;
    const bool shared = std::any_of(all.begin(), all.end(), [&](const DeviceEntry& e) {
        return e.name != name && DevicePart(e.name) == device;
    });
    if (!shared) return device;
    const std::wstring label = LabelPart(name);
    return label.empty() ? name : label;
}

void MoveEntry(std::vector<DeviceEntry>& entries, size_t from, size_t to) {
    if (from >= entries.size() || to >= entries.size() || from == to) return;
    DeviceEntry entry = std::move(entries[from]);
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(from));
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(to), std::move(entry));
}

}  // namespace tono
