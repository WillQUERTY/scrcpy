#include "mfvcam_source.h"

#include <new>
#include <strsafe.h>

std::atomic<long> g_dllRefCount{0};
static HMODULE g_hModule = nullptr;

namespace {

class ScrcpyClassFactory final : public IClassFactory {
public:
    ScrcpyClassFactory() { ++g_dllRefCount; }
    ~ScrcpyClassFactory() { --g_dllRefCount; }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refCount; }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG c = --m_refCount;
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;

        if (pUnkOuter) return CLASS_E_NOAGGREGATION;

        if (riid == IID_IUnknown || riid == IID_IMFActivate || riid == IID_IMFAttributes) {
            auto *act = new (std::nothrow) ScrcpyActivate();
            if (!act) return E_OUTOFMEMORY;
            HRESULT hr = act->QueryInterface(riid, ppv);
            act->Release();
            return hr;
        }

        auto *src = new (std::nothrow) ScrcpyMediaSource();
        if (!src) return E_OUTOFMEMORY;

        HRESULT hr = src->Initialize();
        if (SUCCEEDED(hr)) {
            hr = src->QueryInterface(riid, ppv);
        }
        src->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) ++g_dllRefCount;
        else       --g_dllRefCount;
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refCount{1};
};

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    return (g_dllRefCount.load() == 0) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_ScrcpyVirtualCameraSource) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto *factory = new (std::nothrow) ScrcpyClassFactory();
    if (!factory) return E_OUTOFMEMORY;

    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_hModule, modulePath, MAX_PATH)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    wchar_t keyPath[256] = {};
    StringCchPrintfW(keyPath, ARRAYSIZE(keyPath),
                     L"SOFTWARE\\Classes\\CLSID\\%s", SCRCPY_VCAM_CLSID_STR);

    HKEY hClsidKey = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, nullptr,
                                 REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                 &hClsidKey, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    const wchar_t *desc = SCRCPY_VCAM_FRIENDLY_NAME;
    RegSetValueExW(hClsidKey, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE *>(desc),
                   static_cast<DWORD>((wcslen(desc) + 1) * sizeof(wchar_t)));

    HKEY hInprocKey = nullptr;
    st = RegCreateKeyExW(hClsidKey, L"InprocServer32", 0, nullptr,
                         REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                         &hInprocKey, nullptr);
    if (st == ERROR_SUCCESS) {
        RegSetValueExW(hInprocKey, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(modulePath),
                       static_cast<DWORD>((wcslen(modulePath) + 1) * sizeof(wchar_t)));
        const wchar_t *threading = L"Both";
        RegSetValueExW(hInprocKey, L"ThreadingModel", 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(threading),
                       static_cast<DWORD>((wcslen(threading) + 1) * sizeof(wchar_t)));
        RegCloseKey(hInprocKey);
    }

    RegCloseKey(hClsidKey);
    return HRESULT_FROM_WIN32(st);
}

STDAPI DllUnregisterServer() {
    wchar_t keyPath[256] = {};
    StringCchPrintfW(keyPath, ARRAYSIZE(keyPath),
                     L"SOFTWARE\\Classes\\CLSID\\%s", SCRCPY_VCAM_CLSID_STR);
    LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);
    if (st == ERROR_FILE_NOT_FOUND) return S_OK;
    return HRESULT_FROM_WIN32(st);
}

