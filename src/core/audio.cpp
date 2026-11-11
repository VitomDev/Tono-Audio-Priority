#include "core/audio.h"

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <atomic>

namespace tono {

namespace {

// Form factor of the output and the instance id of the device behind it.
constexpr PROPERTYKEY kFormFactor{{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};
constexpr PROPERTYKEY kDeviceInstance{{0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2};
constexpr UINT kHeadphones = 3;
constexpr UINT kHeadset = 5;

MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR id, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
constexpr CLSID kPolicyConfigClient{0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

IMMDeviceEnumerator* CreateEnumerator() {
    IMMDeviceEnumerator* enumerator = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    return enumerator;
}

std::wstring StringProperty(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring text;
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) text = value.pwszVal;
    PropVariantClear(&value);
    return text;
}

UINT UintProperty(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    UINT number = 0;
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_UI4) number = value.ulVal;
    PropVariantClear(&value);
    return number;
}

std::wstring DefaultFor(ERole role) {
    Com<IMMDeviceEnumerator> enumerator;
    enumerator.p = CreateEnumerator();
    if (!enumerator) return {};
    Com<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, role, &device))) return {};
    LPWSTR id = nullptr;
    std::wstring result;
    if (SUCCEEDED(device->GetId(&id))) result = id;
    CoTaskMemFree(id);
    return result;
}

}  // namespace

std::vector<EndpointInfo> EnumerateOutputs() {
    std::vector<EndpointInfo> outputs;
    Com<IMMDeviceEnumerator> enumerator;
    enumerator.p = CreateEnumerator();
    if (!enumerator) return outputs;
    Com<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &collection))) {
        return outputs;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Com<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) continue;
        EndpointInfo info;
        info.id = id;
        CoTaskMemFree(id);
        DWORD state = 0;
        device->GetState(&state);
        info.active = state == DEVICE_STATE_ACTIVE;
        info.present = state == DEVICE_STATE_ACTIVE || state == DEVICE_STATE_UNPLUGGED;
        Com<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
            info.name = StringProperty(store.p, PKEY_Device_FriendlyName);
            const UINT form = UintProperty(store.p, kFormFactor);
            ParseVidPid(StringProperty(store.p, kDeviceInstance), info.vid, info.pid);
            // Wireless USB headsets often call themselves speakers: anything USB may be switched off.
            info.needsTest = form == kHeadphones || form == kHeadset || info.vid != 0;
        }
        outputs.push_back(std::move(info));
    }
    return outputs;
}

bool IsOutputActive(const std::wstring& id) {
    Com<IMMDeviceEnumerator> enumerator;
    enumerator.p = CreateEnumerator();
    if (!enumerator) return false;
    Com<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(id.c_str(), &device))) return false;
    DWORD state = 0;
    return SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE;
}

std::wstring DefaultOutput() { return DefaultFor(eConsole); }
std::wstring DefaultCommunicationsOutput() { return DefaultFor(eCommunications); }

bool SetDefaultOutput(const std::wstring& id, bool communications) {
    if (id.empty() || !IsOutputActive(id)) return false;
    Com<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), reinterpret_cast<void**>(&policy)))) {
        return false;
    }
    const std::wstring previous = DefaultFor(eConsole);
    const std::wstring previousMedia = DefaultFor(eMultimedia);
    bool ok = SUCCEEDED(policy->SetDefaultEndpoint(id.c_str(), eConsole));
    ok = ok && SUCCEEDED(policy->SetDefaultEndpoint(id.c_str(), eMultimedia));
    if (ok && communications) ok = SUCCEEDED(policy->SetDefaultEndpoint(id.c_str(), eCommunications));
    if (!ok) {
        // Half a switch is worse than none: put back what was there.
        if (!previous.empty()) policy->SetDefaultEndpoint(previous.c_str(), eConsole);
        if (!previousMedia.empty()) policy->SetDefaultEndpoint(previousMedia.c_str(), eMultimedia);
    }
    return ok;
}

// --- Notifications ------------------------------------------------------------------

struct OutputWatcher::Client final : IMMNotificationClient {
    explicit Client(HWND window) : window(window) {}
    HWND window;
    std::atomic<ULONG> refs{1};

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return Post(WM_APP_OUTPUTS_CHANGED); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return Post(WM_APP_OUTPUTS_CHANGED); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return Post(WM_APP_OUTPUTS_CHANGED); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        return flow == eRender && role == eConsole ? Post(WM_APP_DEFAULT_CHANGED) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    HRESULT Post(UINT message) {
        PostMessageW(window, message, 0, 0);
        return S_OK;
    }
};

bool OutputWatcher::Start(HWND window) {
    Stop();
    IMMDeviceEnumerator* enumerator = CreateEnumerator();
    if (!enumerator) return false;
    client_ = new Client(window);
    if (FAILED(enumerator->RegisterEndpointNotificationCallback(client_))) {
        client_->Release();
        client_ = nullptr;
        enumerator->Release();
        return false;
    }
    enumerator_ = enumerator;
    return true;
}

void OutputWatcher::Stop() {
    if (auto* enumerator = static_cast<IMMDeviceEnumerator*>(enumerator_)) {
        enumerator->UnregisterEndpointNotificationCallback(client_);
        enumerator->Release();
        enumerator_ = nullptr;
    }
    if (client_) {
        client_->Release();
        client_ = nullptr;
    }
}

}  // namespace tono
