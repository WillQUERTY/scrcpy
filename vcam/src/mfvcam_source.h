#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "scrcpy_vcam_shm.h"

// {9E6B4F82-7C31-4D8A-B5E2-1F4A8C9D2E60}
extern "C" const GUID CLSID_ScrcpyVirtualCameraSource;

extern std::atomic<long> g_dllRefCount;

class ScrcpyMediaSource;

class ScrcpyMediaStream final : public IMFMediaStream2,
                                public IKsControl,
                                public IMFGetService {
public:
    ScrcpyMediaStream(ScrcpyMediaSource *parent, DWORD streamId);
    ~ScrcpyMediaStream();

    HRESULT Initialize();
    HRESULT GetStreamDescriptorReadOnly(IMFStreamDescriptor **ppSD);
    HRESULT SetAllocator(IUnknown *pAllocator);
    HRESULT SetD3DManager(IUnknown *pManager);
    HRESULT Start(IMFMediaType *pMediaType);
    HRESULT Stop();
    void ShutdownStream();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                            HRESULT hrStatus, const PROPVARIANT *pvValue) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource **ppMediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor **ppStreamDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown *pToken) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE *value) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY Property, ULONG PropertyLength,
                            LPVOID PropertyData, ULONG DataLength,
                            ULONG *BytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD Method, ULONG MethodLength,
                          LPVOID MethodData, ULONG DataLength,
                          ULONG *BytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT Event, ULONG EventLength,
                         LPVOID EventData, ULONG DataLength,
                         ULONG *BytesReturned) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID guidService, REFIID riid, LPVOID *ppvObject) override;

private:
    void WorkerLoop();
    bool EnsureSharedMemoryMapped();
    void UnmapSharedMemory();
    void FillFrameNV12(uint8_t *dstNV12, uint32_t width, uint32_t height, uint64_t tickIndex);
    HRESULT ProduceSample(IUnknown *pToken, uint64_t tickIndex);

    std::atomic<ULONG> m_refCount{1};
    std::mutex m_mutex;

    ScrcpyMediaSource *m_parent = nullptr; // weak ref while alive; AddRef'd via GetMediaSource
    DWORD m_streamId = 0;
    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    bool m_shutdown = false;

    Microsoft::WRL::ComPtr<IMFMediaEventQueue> m_eventQueue;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor> m_streamDescriptor;
    Microsoft::WRL::ComPtr<IMFVideoSampleAllocator> m_allocator;
    Microsoft::WRL::ComPtr<IUnknown> m_d3dManager;

    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    uint32_t m_fps = 30;
    LONGLONG m_frameDuration100ns = 333333;

    // Pacing worker & token queue
    std::deque<Microsoft::WRL::ComPtr<IUnknown>> m_tokenQueue;
    HANDLE m_wakeEvent = nullptr;
    std::thread m_workerThread;
    std::atomic<bool> m_workerStop{false};

    // Shared memory view
    HANDLE m_shmFile = INVALID_HANDLE_VALUE;
    HANDLE m_shmMapping = nullptr;
    const scrcpy_vcam_shm *m_shmView = nullptr;

    std::vector<uint8_t> m_scratchNV12;
};

class ScrcpyMediaSource final : public IMFMediaSourceEx,
                                public IMFGetService,
                                public IKsControl,
                                public IMFSampleAllocatorControl {
public:
    ScrcpyMediaSource();
    ~ScrcpyMediaSource();

    HRESULT Initialize();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                            HRESULT hrStatus, const PROPVARIANT *pvValue) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD *pdwCharacteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor **ppPresentationDescriptor) override;
    STDMETHODIMP Start(IMFPresentationDescriptor *pPresentationDescriptor,
                       const GUID *pguidTimeFormat,
                       const PROPVARIANT *pvarStartPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes **ppAttributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes **ppAttributes) override;
    STDMETHODIMP SetD3DManager(IUnknown *pManager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID guidService, REFIID riid, LPVOID *ppvObject) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY Property, ULONG PropertyLength,
                            LPVOID PropertyData, ULONG DataLength,
                            ULONG *BytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD Method, ULONG MethodLength,
                          LPVOID MethodData, ULONG DataLength,
                          ULONG *BytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT Event, ULONG EventLength,
                         LPVOID EventData, ULONG DataLength,
                         ULONG *BytesReturned) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown *pAllocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD dwOutputStreamID, DWORD *pdwInputStreamID,
                                   MFSampleAllocatorUsage *peUsage) override;

private:
    std::atomic<ULONG> m_refCount{1};
    std::mutex m_mutex;
    bool m_shutdown = false;
    bool m_started = false;

    Microsoft::WRL::ComPtr<IMFMediaEventQueue> m_eventQueue;
    Microsoft::WRL::ComPtr<IMFAttributes> m_attributes;
    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> m_presentationDescriptor;
    Microsoft::WRL::ComPtr<ScrcpyMediaStream> m_stream;
};

class ScrcpyActivate final : public IMFActivate {
public:
    ScrcpyActivate();
    ~ScrcpyActivate();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void **ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

    // IMFAttributes
    STDMETHODIMP GetItem(REFGUID guidKey, PROPVARIANT *pValue) override;
    STDMETHODIMP GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE *pType) override;
    STDMETHODIMP CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL *pbResult) override;
    STDMETHODIMP Compare(IMFAttributes *pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL *pbResult) override;
    STDMETHODIMP GetUINT32(REFGUID guidKey, UINT32 *punValue) override;
    STDMETHODIMP GetUINT64(REFGUID guidKey, UINT64 *punValue) override;
    STDMETHODIMP GetDouble(REFGUID guidKey, double *pfValue) override;
    STDMETHODIMP GetGUID(REFGUID guidKey, GUID *pguidValue) override;
    STDMETHODIMP GetStringLength(REFGUID guidKey, UINT32 *pcchLength) override;
    STDMETHODIMP GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32 *pcchLength) override;
    STDMETHODIMP GetAllocatedString(REFGUID guidKey, LPWSTR *ppwszValue, UINT32 *pcchLength) override;
    STDMETHODIMP GetBlobSize(REFGUID guidKey, UINT32 *pcbBlobSize) override;
    STDMETHODIMP GetBlob(REFGUID guidKey, UINT8 *pBuf, UINT32 cbBufSize, UINT32 *pcbBlobSize) override;
    STDMETHODIMP GetAllocatedBlob(REFGUID guidKey, UINT8 **ppBuf, UINT32 *pcbSize) override;
    STDMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID *ppv) override;
    STDMETHODIMP SetItem(REFGUID guidKey, REFPROPVARIANT Value) override;
    STDMETHODIMP DeleteItem(REFGUID guidKey) override;
    STDMETHODIMP DeleteAllItems() override;
    STDMETHODIMP SetUINT32(REFGUID guidKey, UINT32 unValue) override;
    STDMETHODIMP SetUINT64(REFGUID guidKey, UINT64 unValue) override;
    STDMETHODIMP SetDouble(REFGUID guidKey, double fValue) override;
    STDMETHODIMP SetGUID(REFGUID guidKey, REFGUID guidValue) override;
    STDMETHODIMP SetString(REFGUID guidKey, LPCWSTR wszValue) override;
    STDMETHODIMP SetBlob(REFGUID guidKey, const UINT8 *pBuf, UINT32 cbBufSize) override;
    STDMETHODIMP SetUnknown(REFGUID guidKey, IUnknown *pUnknown) override;
    STDMETHODIMP LockStore() override;
    STDMETHODIMP UnlockStore() override;
    STDMETHODIMP GetCount(UINT32 *pcItems) override;
    STDMETHODIMP GetItemByIndex(UINT32 unIndex, GUID *pguidKey, PROPVARIANT *pValue) override;
    STDMETHODIMP CopyAllItems(IMFAttributes *pDest) override;

private:
    std::atomic<ULONG> m_refCount{1};
    std::mutex m_mutex;
    Microsoft::WRL::ComPtr<IMFAttributes> m_attributes;
    Microsoft::WRL::ComPtr<ScrcpyMediaSource> m_source;
};


