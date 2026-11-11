// Tono's automated tests: a minimal runner without dependencies.
//
//   TonoTests.exe
//
// Only the logic is tested here; nothing touches the real audio settings.

#include "core/headset.h"
#include "core/priority.h"
#include "core/settings.h"

#include <windows.h>

#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace tono;

namespace {

struct TestCase {
    const char* name;
    std::function<void()> run;
};

std::vector<TestCase>& Tests() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> run) { Tests().push_back({name, std::move(run)}); }
};

struct Failure {
    std::string message;
};

#define TEST(name)                                  \
    static void name();                             \
    static Registrar name##_registrar(#name, name); \
    static void name()

#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) throw Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #expression}; \
    } while (false)

DeviceEntry Entry(const wchar_t* id, bool enabled = true) {
    DeviceEntry entry;
    entry.id = id;
    entry.name = id;
    entry.enabled = enabled;
    return entry;
}

std::function<Availability(const DeviceEntry&)> Table(std::map<std::wstring, Availability> table) {
    return [table](const DeviceEntry& entry) {
        const auto it = table.find(entry.id);
        return it == table.end() ? Availability::No : it->second;
    };
}

// What HeadsetControl 4.2.0 printed for a Logitech PRO X Wireless.
const char* kHeadsetOn = R"({
  "name": "HeadsetControl", "version": "4.2.0", "device_count": 1,
  "devices": [ { "status": "success", "device": "Logitech G PRO Series", "vendor": "Logitech",
      "product": "PRO X Wireless Gaming Headset", "id_vendor": "0x046d", "id_product": "0x0aba",
      "capabilities": [ "CAP_SIDETONE", "CAP_BATTERY_STATUS" ],
      "battery": { "status": "BATTERY_AVAILABLE", "level": 21 } } ] })";
const char* kHeadsetOff = R"({
  "devices": [ { "status": "partial", "id_vendor": "0x046d", "id_product": "0x0aba",
      "battery": { "status": "BATTERY_UNAVAILABLE", "level": -1 },
      "errors": { "battery": "Device is offline or not responding" } } ] })";
const char* kHeadsetTimeout = R"({
  "devices": [ { "status": "partial", "id_vendor": "0x046d", "id_product": "0x0aba",
      "battery": { "status": "BATTERY_UNAVAILABLE", "level": -1 },
      "errors": { "battery": "Operation timed out" } } ] })";

}  // namespace

// --- Choosing the output ------------------------------------------------------------------

TEST(ChoosesFirstAvailable) {
    const std::vector<DeviceEntry> list{Entry(L"jbl"), Entry(L"prox"), Entry(L"monitor")};
    const Choice choice = ChooseTarget(list, Table({{L"prox", Availability::Yes}, {L"monitor", Availability::Yes}}));
    CHECK(choice.decided);
    CHECK(choice.id == L"prox");
}

TEST(SkipsDisabled) {
    const std::vector<DeviceEntry> list{Entry(L"jbl", false), Entry(L"monitor")};
    const Choice choice = ChooseTarget(list, Table({{L"jbl", Availability::Yes}, {L"monitor", Availability::Yes}}));
    CHECK(choice.id == L"monitor");
}

TEST(UnknownAboveBlocksDecision) {
    // HeadsetControl did not answer for the PRO X: the monitor must not be picked.
    const std::vector<DeviceEntry> list{Entry(L"prox"), Entry(L"monitor")};
    const Choice choice = ChooseTarget(list, Table({{L"prox", Availability::Unknown}, {L"monitor", Availability::Yes}}));
    CHECK(!choice.decided);
}

TEST(UnknownBelowDoesNotMatter) {
    const std::vector<DeviceEntry> list{Entry(L"jbl"), Entry(L"prox")};
    const Choice choice = ChooseTarget(list, Table({{L"jbl", Availability::Yes}, {L"prox", Availability::Unknown}}));
    CHECK(choice.decided);
    CHECK(choice.id == L"jbl");
}

TEST(NothingAvailable) {
    const std::vector<DeviceEntry> list{Entry(L"jbl"), Entry(L"prox")};
    const Choice choice = ChooseTarget(list, Table({}));
    CHECK(choice.decided);
    CHECK(choice.id.empty());
}

// --- Keeping the list up to date ------------------------------------------------------------

TEST(HandsFreeIsRecognized) {
    CHECK(IsHandsFree(L"Auriculares con micrófono (JBL Tune Buds 2 Hands-Free)"));
    CHECK(!IsHandsFree(L"Auriculares (JBL Tune Buds 2)"));
}

TEST(VidPidFromInstanceId) {
    uint16_t vid = 0, pid = 0;
    CHECK(ParseVidPid(L"{1}.USB\\VID_046D&PID_0ABA&MI_00\\6&3B87A6D9&0&0000", vid, pid));
    CHECK(vid == 0x046d && pid == 0x0aba);
    CHECK(!ParseVidPid(L"{1}.HDAUDIO\\FUNC_01&VEN_10DE", vid, pid));
}

TEST(MergeAddsNewSkipsHandsFreeAndRebindsByName) {
    std::vector<DeviceEntry> list;
    DeviceEntry prox = Entry(L"old-prox");
    prox.name = L"Altavoces (Logitech PRO X)";
    prox.detection = Detection::HeadsetControl;
    list.push_back(prox);

    std::vector<EndpointInfo> endpoints;
    endpoints.push_back({L"new-prox", L"Altavoces (Logitech PRO X)", true, true, true, 0x046d, 0x0aba});
    endpoints.push_back({L"jbl", L"Auriculares (JBL Tune Buds 2)", true, true, true});
    endpoints.push_back({L"jbl-hf", L"Auriculares con micrófono (JBL Tune Buds 2 Hands-Free)", true, true, true});
    endpoints.push_back({L"monitor", L"VG2791R (NVIDIA High Definition Audio)", true, true, false});
    endpoints.push_back({L"iphone", L"Altavoces (iPhone Hands-Free HF Audio)", false, true, true});

    const std::vector<std::wstring> added = MergeEndpoints(list, endpoints);
    CHECK(list.size() == 3);
    CHECK(list[0].id == L"new-prox");  // Same name, new USB port: kept its place and settings
    CHECK(list[0].detection == Detection::HeadsetControl);
    CHECK(list[0].vid == 0x046d);
    CHECK(list[1].id == L"jbl");
    CHECK(list[1].detection == Detection::Untested);
    CHECK(list[2].id == L"monitor");
    CHECK(list[2].detection == Detection::Automatic);
    CHECK(added.size() == 2);

    // Unplugged outputs already on the list stay; nothing is added twice.
    endpoints[1].active = false;
    CHECK(MergeEndpoints(list, endpoints).empty());
    CHECK(list.size() == 3);
}

TEST(DisplayNames) {
    std::vector<DeviceEntry> list{Entry(L"a"), Entry(L"b"), Entry(L"c")};
    list[0].name = L"Altavoces (2- Logitech PRO X Wireless Gaming Headset)";
    list[1].name = L"VG2791R (NVIDIA High Definition Audio)";
    list[2].name = L"MAG 274 X30MV (NVIDIA High Definition Audio)";
    CHECK(DisplayName(list[0].name, list) == L"Logitech PRO X Wireless Gaming Headset");
    CHECK(DisplayName(list[1].name, list) == L"VG2791R");
    CHECK(DisplayName(list[2].name, list) == L"MAG 274 X30MV");
    CHECK(DisplayName(L"Speakers", list) == L"Speakers");
}

TEST(MoveEntryReorders) {
    std::vector<DeviceEntry> list{Entry(L"a"), Entry(L"b"), Entry(L"c")};
    MoveEntry(list, 2, 0);
    CHECK(list[0].id == L"c" && list[1].id == L"a" && list[2].id == L"b");
    MoveEntry(list, 0, 2);
    CHECK(list[0].id == L"a" && list[2].id == L"c");
}

// --- HeadsetControl ----------------------------------------------------------------------------

TEST(HeadsetJsonOnOffTimeout) {
    auto readings = ParseHeadsetJson(kHeadsetOn);
    CHECK(readings.size() == 1);
    CHECK(readings[0].vid == 0x046d && readings[0].pid == 0x0aba);
    CHECK(readings[0].state == HeadsetState::On);
    readings = ParseHeadsetJson(kHeadsetOff);
    CHECK(readings.size() == 1 && readings[0].state == HeadsetState::Off);
    // While waking up it once timed out: that must never read as off.
    readings = ParseHeadsetJson(kHeadsetTimeout);
    CHECK(readings.size() == 1 && readings[0].state == HeadsetState::Unknown);
    CHECK(ParseHeadsetJson("").empty());
    CHECK(ParseHeadsetJson("garbage {").empty());
}

// --- Settings --------------------------------------------------------------------------------

TEST(EntryRoundTrip) {
    DeviceEntry entry = Entry(L"{0.0.0.00000000}.{2adf6346}");
    entry.name = L"Altavoces (Logitech PRO X Wireless Gaming Headset)";
    entry.enabled = false;
    entry.detection = Detection::Manual;
    entry.manualOn = false;
    entry.vid = 0x046d;
    entry.pid = 0x0aba;
    DeviceEntry back;
    CHECK(ParseEntry(SerializeEntry(entry), back));
    CHECK(back == entry);
    CHECK(!ParseEntry(L"broken", back));
}

TEST(SettingsRoundTrip) {
    const wchar_t* key = L"Software\\Tono Audio Priority Tests";
    RegDeleteTreeW(HKEY_CURRENT_USER, key);
    CHECK(!Settings::Load(key).configured);
    Settings settings;
    settings.devices = {Entry(L"a"), Entry(L"b", false)};
    settings.paused = true;
    settings.communications = true;
    CHECK(settings.Save(key));
    const Settings loaded = Settings::Load(key);
    CHECK(loaded.configured);
    CHECK(loaded.paused && loaded.communications);
    CHECK(loaded.devices.size() == 2);
    CHECK(loaded.devices[1].id == L"b" && !loaded.devices[1].enabled);
    RegDeleteTreeW(HKEY_CURRENT_USER, key);
}

// --- Runner ------------------------------------------------------------------------------------

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    int passed = 0, failed = 0;
    for (const auto& test : Tests()) {
        try {
            test.run();
            ++passed;
            std::cout << "PASS " << test.name << "\n";
        } catch (const Failure& failure) {
            ++failed;
            std::cout << "FAIL " << test.name << ": " << failure.message << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << test.name << ": exception: " << e.what() << "\n";
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
