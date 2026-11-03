#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <cstdio>
#include <cwchar>
#include <string>

#include "scrcpy_vcam_shm.h"

using Microsoft::WRL::ComPtr;

namespace {

bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2,
                                 SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

std::wstring GetProgramDataBase() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return L"C:\\ProgramData";
    }
    return buf;
}

std::wstring GetExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
    return path;
}

bool ApplyCrossSessionDacl(const std::wstring &path) {
    // Grant SYSTEM, Administrators, LOCAL SERVICE Full Control;
    // Grant Authenticated Users + Users Read/Write/Execute so FrameServer
    // (LOCAL SERVICE) can load the DLL & read SHM, and non-admin scrcpy.exe
    // can write frames to SHM.
    const wchar_t *sddl =
        L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GA;;;LS)"
        L"(A;OICI;GRGWGX;;;AU)(A;OICI;GRGWGX;;;BU)";

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }

    PACL dacl = nullptr;
    BOOL daclPresent = FALSE, daclDefaulted = FALSE;
    bool ok = false;
    if (GetSecurityDescriptorDacl(sd, &daclPresent, &dacl, &daclDefaulted) && daclPresent) {
        DWORD err = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, dacl, nullptr);
        ok = (err == ERROR_SUCCESS);
    }
    LocalFree(sd);
    return ok;
}

bool EnsureSharedMemoryFile(const std::wstring &shmPath) {
    HANDLE hFile = CreateFileW(
        shmPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"[ERROR] Could not open/create %s (err=%lu)\n",
                 shmPath.c_str(), GetLastError());
        return false;
    }

    LARGE_INTEGER sz = {};
    sz.QuadPart = static_cast<LONGLONG>(sizeof(scrcpy_vcam_shm));
    if (!SetFilePointerEx(hFile, sz, nullptr, FILE_BEGIN) || !SetEndOfFile(hFile)) {
        fwprintf(stderr, L"[ERROR] Could not size %s (err=%lu)\n",
                 shmPath.c_str(), GetLastError());
        CloseHandle(hFile);
        return false;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(sizeof(scrcpy_vcam_shm)), nullptr);
    if (hMap) {
        auto *shm = static_cast<scrcpy_vcam_shm *>(
            MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(scrcpy_vcam_shm)));
        if (shm) {
            if (shm->magic != SCRCPY_VCAM_MAGIC || shm->version != SCRCPY_VCAM_VERSION) {
                ZeroMemory(shm, sizeof(scrcpy_vcam_shm));
                shm->magic = SCRCPY_VCAM_MAGIC;
                shm->version = SCRCPY_VCAM_VERSION;
                LARGE_INTEGER freq = {};
                QueryPerformanceFrequency(&freq);
                shm->qpc_freq = freq.QuadPart;
            }
            UnmapViewOfFile(shm);
        }
        CloseHandle(hMap);
    }

    CloseHandle(hFile);
    ApplyCrossSessionDacl(shmPath);
    return true;
}

bool RegisterComServerDll(const std::wstring &dllPath) {
    std::wstring keyPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + SCRCPY_VCAM_CLSID_STR;
    HKEY hClsid = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, nullptr,
                                 REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hClsid, nullptr);
    if (st != ERROR_SUCCESS) {
        fwprintf(stderr, L"[ERROR] RegCreateKeyExW failed: %ld\n", st);
        return false;
    }

    const wchar_t *desc = SCRCPY_VCAM_FRIENDLY_NAME;
    RegSetValueExW(hClsid, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE *>(desc),
                   static_cast<DWORD>((wcslen(desc) + 1) * sizeof(wchar_t)));

    HKEY hInproc = nullptr;
    st = RegCreateKeyExW(hClsid, L"InprocServer32", 0, nullptr,
                         REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hInproc, nullptr);
    if (st == ERROR_SUCCESS) {
        RegSetValueExW(hInproc, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(dllPath.c_str()),
                       static_cast<DWORD>((dllPath.size() + 1) * sizeof(wchar_t)));
        const wchar_t *tm = L"Both";
        RegSetValueExW(hInproc, L"ThreadingModel", 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(tm),
                       static_cast<DWORD>((wcslen(tm) + 1) * sizeof(wchar_t)));
        RegCloseKey(hInproc);
    }
    RegCloseKey(hClsid);
    return st == ERROR_SUCCESS;
}

void StopFrameServerService() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return;
    SC_HANDLE hSvc = OpenServiceW(hSCM, L"FrameServer", SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (hSvc) {
        SERVICE_STATUS status = {};
        ControlService(hSvc, SERVICE_CONTROL_STOP, &status);
        CloseServiceHandle(hSvc);
    }
    CloseServiceHandle(hSCM);
    Sleep(300);
}

void RemoveExistingVirtualCamera() {
    ComPtr<IMFVirtualCamera> vcam;
    HRESULT hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_AllUsers,
        SCRCPY_VCAM_FRIENDLY_NAME,
        SCRCPY_VCAM_CLSID_STR,
        nullptr, 0, &vcam);
    if (SUCCEEDED(hr) && vcam) {
        vcam->Remove();
        vcam->Shutdown();
    }
}

int CmdInstall() {
    if (!IsRunningAsAdmin()) {
        fwprintf(stderr,
                 L"[ERROR] Installing a system-wide persistent virtual camera requires Administrator privileges.\n"
                 L"        Run this command from an Administrator PowerShell/CMD, or run install_vcam.bat.\n");
        return 1;
    }

    BOOL supported = FALSE;
    HRESULT hr = MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported);
    if (FAILED(hr) || !supported) {
        fwprintf(stderr, L"[ERROR] Windows Media Foundation Virtual Camera is not supported on this OS (hr=0x%08lX)\n", hr);
        return 1;
    }

    std::wstring srcDll = GetExeDir() + L"\\scrcpy-mfvcam.dll";
    if (GetFileAttributesW(srcDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"[ERROR] Could not find %s next to scrcpy-vcam-ctl.exe\n", srcDll.c_str());
        return 1;
    }

    // Remove existing camera first in case FrameServer holds scrcpy-mfvcam.dll
    RemoveExistingVirtualCamera();
    StopFrameServerService();

    std::wstring baseDir = GetProgramDataBase() + SCRCPY_VCAM_DIR_REL;
    std::wstring dstDll  = GetProgramDataBase() + SCRCPY_VCAM_DLL_REL;
    std::wstring shmPath = GetProgramDataBase() + SCRCPY_VCAM_SHM_REL;
    std::wstring symPath = GetProgramDataBase() + SCRCPY_VCAM_SYMLINK_REL;

    CreateDirectoryW(baseDir.c_str(), nullptr);
    ApplyCrossSessionDacl(baseDir);
    DeleteFileW((baseDir + L"\\qi.log").c_str());

    if (!CopyFileW(srcDll.c_str(), dstDll.c_str(), FALSE)) {
        // FrameServer may keep the previous DLL mapped briefly; restart FrameServer or move old DLL aside
        std::wstring oldDll = dstDll + L".old";
        DeleteFileW(oldDll.c_str());
        MoveFileExW(dstDll.c_str(), oldDll.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!CopyFileW(srcDll.c_str(), dstDll.c_str(), FALSE)) {
            fwprintf(stderr, L"[ERROR] Failed to copy DLL to %s (err=%lu)\n",
                     dstDll.c_str(), GetLastError());
            return 1;
        }
    }
    ApplyCrossSessionDacl(dstDll);

    if (!EnsureSharedMemoryFile(shmPath)) {
        return 1;
    }

    if (!RegisterComServerDll(dstDll)) {
        return 1;
    }

    ComPtr<IMFVirtualCamera> vcam;
    hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_AllUsers,
        SCRCPY_VCAM_FRIENDLY_NAME,
        SCRCPY_VCAM_CLSID_STR,
        nullptr, 0, &vcam);
    if (FAILED(hr)) {
        fwprintf(stderr, L"[ERROR] MFCreateVirtualCamera failed: 0x%08lX\n", hr);
        return 1;
    }

    hr = vcam->Start(nullptr);
    if (FAILED(hr)) {
        fwprintf(stderr, L"[ERROR] IMFVirtualCamera::Start failed: 0x%08lX\n", hr);
        vcam->Shutdown();
        return 1;
    }

    wchar_t *symLink = nullptr;
    UINT32 symLen = 0;
    if (SUCCEEDED(vcam->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symLink, &symLen)) && symLink) {
        wprintf(L"[OK] Installed persistent camera: \"%s\"\n", SCRCPY_VCAM_FRIENDLY_NAME);
        wprintf(L"     Symbolic Link: %s\n", symLink);
        HANDLE hf = CreateFileW(symPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hf, symLink, symLen * sizeof(wchar_t), &written, nullptr);
            CloseHandle(hf);
        }
        CoTaskMemFree(symLink);
    } else {
        wprintf(L"[OK] Installed persistent camera: \"%s\"\n", SCRCPY_VCAM_FRIENDLY_NAME);
    }

    // Do NOT call vcam->Remove(); Shutdown() releases the handle while keeping
    // MFVirtualCameraLifetime_System active across sessions & reboots!
    vcam->Shutdown();
    wprintf(L"     DLL Path:      %s\n", dstDll.c_str());
    wprintf(L"     Shared Memory: %s\n", shmPath.c_str());
    return 0;
}

int CmdUninstall() {
    if (!IsRunningAsAdmin()) {
        fwprintf(stderr, L"[ERROR] Uninstalling the system virtual camera requires Administrator privileges.\n");
        return 1;
    }

    RemoveExistingVirtualCamera();

    std::wstring keyPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + SCRCPY_VCAM_CLSID_STR;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath.c_str());

    std::wstring symPath = GetProgramDataBase() + SCRCPY_VCAM_SYMLINK_REL;
    DeleteFileW(symPath.c_str());

    wprintf(L"[OK] Uninstalled \"%s\" from Windows.\n", SCRCPY_VCAM_FRIENDLY_NAME);
    return 0;
}

int CmdStatus() {
    ComPtr<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (FAILED(hr)) return 1;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate **devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attr.Get(), &devices, &count);
    if (FAILED(hr)) {
        fwprintf(stderr, L"[ERROR] MFEnumDeviceSources failed: 0x%08lX\n", hr);
        return 1;
    }

    bool found = false;
    wprintf(L"=== Windows Video Capture Devices (%u total) ===\n", count);
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t *name = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len)) && name) {
            bool match = (_wcsicmp(name, SCRCPY_VCAM_FRIENDLY_NAME) == 0);
            wprintf(L"  [%u] %s%s\n", i, name, match ? L"  <-- REGISTERED" : L"");
            if (match) found = true;
            CoTaskMemFree(name);
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);

    std::wstring shmPath = GetProgramDataBase() + SCRCPY_VCAM_SHM_REL;
    HANDLE hFile = CreateFileW(shmPath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (hMap) {
            auto *shm = static_cast<const scrcpy_vcam_shm *>(
                MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(scrcpy_vcam_shm)));
            if (shm && shm->magic == SCRCPY_VCAM_MAGIC) {
                LARGE_INTEGER now = {};
                QueryPerformanceCounter(&now);
                bool live = shm->active && shm->qpc_freq > 0 &&
                            (now.QuadPart - shm->timestamp_qpc) < (shm->qpc_freq * 5) / 2;
                wprintf(L"\n=== Shared Memory Stream Status ===\n");
                wprintf(L"  State:        %s\n", live ? L"LIVE (scrcpy streaming)" : L"STANDBY (waiting for scrcpy --vcam)");
                wprintf(L"  Frames sent:  %llu\n", static_cast<unsigned long long>(shm->frame_seq));
                if (shm->src_width && shm->src_height) {
                    wprintf(L"  Source size:  %ux%u -> 1920x1080 NV12\n", shm->src_width, shm->src_height);
                }
                UnmapViewOfFile(shm);
            }
            CloseHandle(hMap);
        }
        CloseHandle(hFile);
    } else {
        wprintf(L"\n[INFO] Shared memory file not initialized yet (%s)\n", shmPath.c_str());
    }

    return found ? 0 : 2;
}

int CmdTest() {
    ComPtr<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (FAILED(hr)) return 1;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate **devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attr.Get(), &devices, &count);
    if (FAILED(hr)) return 1;

    ComPtr<IMFActivate> target;
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t *name = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len)) && name) {
            if (_wcsicmp(name, SCRCPY_VCAM_FRIENDLY_NAME) == 0) {
                target = devices[i];
            }
            CoTaskMemFree(name);
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);

    if (!target) {
        fwprintf(stderr, L"[ERROR] \"%s\" is not registered. Run: scrcpy-vcam-ctl.exe install\n",
                 SCRCPY_VCAM_FRIENDLY_NAME);
        return 1;
    }

    ComPtr<IMFMediaSource> mediaSource;
    hr = target->ActivateObject(IID_PPV_ARGS(&mediaSource));
    if (FAILED(hr)) {
        fwprintf(stderr, L"[ERROR] ActivateObject failed: 0x%08lX\n", hr);
        return 1;
    }

    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(mediaSource.Get(), nullptr, &reader);
    if (FAILED(hr)) {
        fwprintf(stderr, L"[ERROR] MFCreateSourceReaderFromMediaSource failed: 0x%08lX\n", hr);
        mediaSource->Shutdown();
        return 1;
    }

    ComPtr<IMFMediaType> curType;
    if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &curType))) {
        UINT32 w = 0, h = 0, fn = 0, fd = 1;
        MFGetAttributeSize(curType.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(curType.Get(), MF_MT_FRAME_RATE, &fn, &fd);
        wprintf(L"[OK] Opened \"%s\" via Windows Camera FrameServer (%ux%u @ %u fps)\n",
                SCRCPY_VCAM_FRIENDLY_NAME, w, h, fd ? (fn / fd) : 0);
    }

    int received = 0;
    for (int i = 0; i < 5; ++i) {
        DWORD streamIdx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                &streamIdx, &flags, &ts, &sample);
        if (FAILED(hr)) {
            fwprintf(stderr, L"[ERROR] ReadSample(%d) failed: 0x%08lX\n", i, hr);
            break;
        }
        DWORD totalLen = 0;
        if (sample) {
            sample->GetTotalLength(&totalLen);
            ++received;
        }
        wprintf(L"  Frame #%d: flags=0x%lX, pts=%lld (100ns), bytes=%lu\n",
                i + 1, flags, ts, totalLen);
    }

    reader.Reset();
    mediaSource->Shutdown();
    wprintf(L"[%s] FrameServer test finished (%d/5 frames captured).\n",
            received == 5 ? L"PASS" : L"FAIL", received);
    return (received == 5) ? 0 : 1;
}

} // namespace

int CmdTestSession() {
    wprintf(L"[INFO] Testing MFVirtualCamera with Session lifetime & CurrentUser access...\n");
    ComPtr<IMFVirtualCamera> vcam;
    HRESULT hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser,
        SCRCPY_VCAM_FRIENDLY_NAME,
        SCRCPY_VCAM_CLSID_STR,
        nullptr, 0, &vcam);
    if (FAILED(hr)) {
        fwprintf(stderr, L"[ERROR] MFCreateVirtualCamera failed: 0x%08lX\n", hr);
        return 1;
    }

    hr = vcam->Start(nullptr);
    if (FAILED(hr)) {
        fwprintf(stderr, L"[ERROR] IMFVirtualCamera::Start (Session) failed: 0x%08lX\n", hr);
        vcam->Shutdown();
        return 1;
    }

    wprintf(L"[OK] IMFVirtualCamera::Start (Session) SUCCEEDED!\n");
    vcam->Stop();
    vcam->Shutdown();
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 1;
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        CoUninitialize();
        return 1;
    }

    int exitCode = 0;
    if (argc < 2) {
        wprintf(L"Usage: scrcpy-vcam-ctl.exe <install | uninstall | status | test>\n");
        exitCode = 1;
    } else if (_wcsicmp(argv[1], L"install") == 0) {
        exitCode = CmdInstall();
    } else if (_wcsicmp(argv[1], L"uninstall") == 0) {
        exitCode = CmdUninstall();
    } else if (_wcsicmp(argv[1], L"status") == 0) {
        exitCode = CmdStatus();
    } else if (_wcsicmp(argv[1], L"test") == 0) {
        exitCode = CmdTest();
    } else if (_wcsicmp(argv[1], L"test-session") == 0) {
        exitCode = CmdTestSession();
    } else {
        fwprintf(stderr, L"Unknown command: %s\n", argv[1]);
        exitCode = 1;
    }

    MFShutdown();
    CoUninitialize();
    return exitCode;
}

