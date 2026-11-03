#include "mfvcam_source.h"

#include <initguid.h>
#include <propvarutil.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

#if defined(_DEBUG)
static void LogMsg(const wchar_t *fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, sizeof(buf)/sizeof(buf[0]), _TRUNCATE, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
}
#else
static inline void LogMsg(const wchar_t *, ...) {}
#endif

// {9E6B4F82-7C31-4D8A-B5E2-1F4A8C9D2E60}
extern "C" const GUID CLSID_ScrcpyVirtualCameraSource = {
    0x9e6b4f82, 0x7c31, 0x4d8a, {0xb5, 0xe2, 0x1f, 0x4a, 0x8c, 0x9d, 0x2e, 0x60}
};

namespace {

struct VideoProfile {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
};

constexpr VideoProfile kProfiles[] = {
    {1920, 1080, 30},
    {1920, 1080, 60},
    {1280, 720,  60},
    {1280, 720,  30},
};

HRESULT CreateNV12MediaType(uint32_t width, uint32_t height, uint32_t fps, IMFMediaType **ppType) {
    if (!ppType) {
        return E_POINTER;
    }
    *ppType = nullptr;

    ComPtr<IMFMediaType> mt;
    HRESULT hr = MFCreateMediaType(&mt);
    if (FAILED(hr)) return hr;

    hr = mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;
    hr = mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(hr)) return hr;
    hr = mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return hr;
    hr = mt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) return hr;

    uint32_t sampleSize = (width * height * 3u) / 2u;
    hr = mt->SetUINT32(MF_MT_SAMPLE_SIZE, sampleSize);
    if (FAILED(hr)) return hr;
    hr = mt->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    if (FAILED(hr)) return hr;
    hr = mt->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (FAILED(hr)) return hr;

    hr = MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, width, height);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE_RANGE_MIN, fps, 1);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE_RANGE_MAX, fps, 1);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) return hr;

    uint32_t bitrate = width * height * 12u * fps;
    mt->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    mt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    mt->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    mt->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    mt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);

    *ppType = mt.Detach();
    return S_OK;
}

std::wstring GetSharedMemoryFilePath() {
    wchar_t progData[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        wcscpy_s(progData, L"C:\\ProgramData");
    }
    std::wstring path(progData);
    path += SCRCPY_VCAM_SHM_REL;
    return path;
}

// Minimal 5x7 uppercase bitmap font for clean standby overlay
const uint8_t *GetGlyph5x7(char c) {
    static const uint8_t kSpace[7] = {0,0,0,0,0,0,0};
    static const uint8_t kDash[7]  = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t kA[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t kB[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
    static const uint8_t kC[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
    static const uint8_t kD[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E};
    static const uint8_t kE[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
    static const uint8_t kF[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
    static const uint8_t kG[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F};
    static const uint8_t kI[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t kL[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
    static const uint8_t kM[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
    static const uint8_t kN[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11};
    static const uint8_t kO[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t kP[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
    static const uint8_t kR[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
    static const uint8_t kS[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
    static const uint8_t kT[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t kU[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t kV[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04};
    static const uint8_t kW[7] = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11};
    static const uint8_t kY[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04};

    switch (c) {
        case '-': return kDash;
        case 'A': return kA;
        case 'B': return kB;
        case 'C': return kC;
        case 'D': return kD;
        case 'E': return kE;
        case 'F': return kF;
        case 'G': return kG;
        case 'I': return kI;
        case 'L': return kL;
        case 'M': return kM;
        case 'N': return kN;
        case 'O': return kO;
        case 'P': return kP;
        case 'R': return kR;
        case 'S': return kS;
        case 'T': return kT;
        case 'U': return kU;
        case 'V': return kV;
        case 'W': return kW;
        case 'Y': return kY;
        default:  return kSpace;
    }
}

void DrawTextNV12(uint8_t *yPlane, uint32_t width, uint32_t height,
                  const char *text, uint32_t centerY, uint32_t scale, uint8_t luma) {
    size_t len = strlen(text);
    if (len == 0) return;
    uint32_t charAdvance = 6u * scale;
    uint32_t totalWidth = static_cast<uint32_t>(len) * charAdvance - scale;
    if (totalWidth >= width) return;
    uint32_t startX = (width - totalWidth) / 2u;
    uint32_t glyphH = 7u * scale;
    if (centerY < glyphH / 2u || centerY + glyphH / 2u >= height) return;
    uint32_t startY = centerY - glyphH / 2u;

    for (size_t i = 0; i < len; ++i) {
        const uint8_t *glyph = GetGlyph5x7(text[i]);
        uint32_t gx = startX + static_cast<uint32_t>(i) * charAdvance;
        for (uint32_t row = 0; row < 7u; ++row) {
            uint8_t bits = glyph[row];
            for (uint32_t col = 0; col < 5u; ++col) {
                if (bits & (1u << (4u - col))) {
                    for (uint32_t sy = 0; sy < scale; ++sy) {
                        uint8_t *dstRow = yPlane + (startY + row * scale + sy) * width + gx + col * scale;
                        memset(dstRow, luma, scale);
                    }
                }
            }
        }
    }
}

void RenderStandbyNV12(uint8_t *dstNV12, uint32_t width, uint32_t height, uint64_t tickIndex) {
    uint8_t *yPlane = dstNV12;
    uint8_t *uvPlane = dstNV12 + (width * height);

    // Dark slate background: Y=24, U=132, V=125
    memset(yPlane, 24, width * height);
    uint32_t uvSize = width * (height / 2u);
    for (uint32_t i = 0; i < uvSize; i += 2) {
        uvPlane[i]     = 132;
        uvPlane[i + 1] = 125;
    }

    // Center card
    uint32_t cardW = (width * 60u) / 100u;
    uint32_t cardH = (height * 28u) / 100u;
    uint32_t cardX = ((width - cardW) / 2u) & ~1u;
    uint32_t cardY = ((height - cardH) / 2u) & ~1u;

    for (uint32_t y = cardY; y < cardY + cardH; ++y) {
        memset(yPlane + y * width + cardX, 38, cardW);
    }

    // Top accent bar on card (scrcpy teal/green accent: Y=145, U=112, V=84)
    uint32_t barH = std::max(4u, height / 180u) & ~1u;
    for (uint32_t y = cardY; y < cardY + barH; ++y) {
        memset(yPlane + y * width + cardX, 145, cardW);
    }
    for (uint32_t y = cardY / 2u; y < (cardY + barH) / 2u; ++y) {
        uint8_t *uvRow = uvPlane + y * width + cardX;
        for (uint32_t x = 0; x < cardW; x += 2) {
            uvRow[x]     = 112;
            uvRow[x + 1] = 84;
        }
    }

    uint32_t titleScale = (width >= 1920u) ? 5u : 3u;
    uint32_t subScale   = (width >= 1920u) ? 3u : 2u;

    DrawTextNV12(yPlane, width, height, "SCRCPY VIRTUAL CAMERA",
                 height / 2u - (cardH / 6u), titleScale, 225);
    DrawTextNV12(yPlane, width, height, "STANDBY - RUN SCRCPY --VCAM",
                 height / 2u + (cardH / 6u), subScale, 155);

    // Animated activity bar at bottom of card so apps see live frames
    uint32_t animW = cardW / 5u;
    uint32_t travel = cardW - animW;
    uint32_t phase = static_cast<uint32_t>((tickIndex * 8u) % (travel * 2u));
    uint32_t pos = (phase <= travel) ? phase : (travel * 2u - phase);
    uint32_t barY = (cardY + cardH - barH * 2u) & ~1u;
    uint32_t barX = (cardX + pos) & ~1u;
    for (uint32_t y = barY; y < barY + barH; ++y) {
        memset(yPlane + y * width + barX, 160, animW);
    }
}

void DownscaleNV12_1080_to_720(const uint8_t *src, uint8_t *dst) {
    // Exact 3:2 ratio: 1920x1080 -> 1280x720
    const uint8_t *srcY  = src;
    const uint8_t *srcUV = src + SCRCPY_VCAM_Y_SIZE;
    uint8_t *dstY  = dst;
    uint8_t *dstUV = dst + (1280u * 720u);

    for (uint32_t dy = 0; dy < 720u; ++dy) {
        uint32_t sy = (dy * 1080u) / 720u;
        const uint8_t *sRow = srcY + sy * 1920u;
        uint8_t *dRow = dstY + dy * 1280u;
        uint32_t sx = 0;
        for (uint32_t dx = 0; dx < 1280u; dx += 2, sx += 3) {
            dRow[dx]     = sRow[sx];
            dRow[dx + 1] = static_cast<uint8_t>((uint32_t(sRow[sx + 1]) + uint32_t(sRow[sx + 2])) >> 1);
        }
    }

    // UV plane: 960x540 UV pairs -> 640x360 UV pairs
    for (uint32_t dy = 0; dy < 360u; ++dy) {
        uint32_t sy = (dy * 540u) / 360u;
        const uint8_t *sRow = srcUV + sy * 1920u;
        uint8_t *dRow = dstUV + dy * 1280u;
        uint32_t sp = 0;
        for (uint32_t dp = 0; dp < 640u; dp += 2, sp += 3) {
            dRow[dp * 2]     = sRow[sp * 2];
            dRow[dp * 2 + 1] = sRow[sp * 2 + 1];
            dRow[dp * 2 + 2] = static_cast<uint8_t>((uint32_t(sRow[sp * 2 + 2]) + uint32_t(sRow[sp * 2 + 4])) >> 1);
            dRow[dp * 2 + 3] = static_cast<uint8_t>((uint32_t(sRow[sp * 2 + 3]) + uint32_t(sRow[sp * 2 + 5])) >> 1);
        }
    }
}

} // namespace

// ============================================================================
// ScrcpyMediaStream
// ============================================================================

ScrcpyMediaStream::ScrcpyMediaStream(ScrcpyMediaSource *parent, DWORD streamId)
    : m_parent(parent), m_streamId(streamId) {
    ++g_dllRefCount;
}

ScrcpyMediaStream::~ScrcpyMediaStream() {
    ShutdownStream();
    --g_dllRefCount;
}

HRESULT ScrcpyMediaStream::Initialize() {
    HRESULT hr = MFCreateEventQueue(&m_eventQueue);
    if (FAILED(hr)) return hr;

    constexpr DWORD kNumTypes = static_cast<DWORD>(std::size(kProfiles));
    IMFMediaType *types[kNumTypes] = {};
    for (DWORD i = 0; i < kNumTypes; ++i) {
        hr = CreateNV12MediaType(kProfiles[i].width, kProfiles[i].height,
                                 kProfiles[i].fps, &types[i]);
        if (FAILED(hr)) {
            for (DWORD j = 0; j < i; ++j) types[j]->Release();
            return hr;
        }
    }

    hr = MFCreateStreamDescriptor(m_streamId, kNumTypes, types, &m_streamDescriptor);
    if (SUCCEEDED(hr)) {
        ComPtr<IMFMediaTypeHandler> handler;
        hr = m_streamDescriptor->GetMediaTypeHandler(&handler);
        if (SUCCEEDED(hr)) {
            hr = handler->SetCurrentMediaType(types[0]);
        }
    }
    for (DWORD i = 0; i < kNumTypes; ++i) {
        types[i]->Release();
    }
    if (FAILED(hr)) return hr;

    // Required FrameServer stream attributes
    m_streamDescriptor->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    m_streamDescriptor->SetUINT32(MF_DEVICESTREAM_STREAM_ID, m_streamId);
    m_streamDescriptor->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    m_streamDescriptor->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, 0x0001 /* Color */);
    m_streamDescriptor->SetString(MF_SD_STREAM_NAME, L"Video Capture");

    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_wakeEvent) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    m_workerStop = false;
    m_workerThread = std::thread(&ScrcpyMediaStream::WorkerLoop, this);
    return S_OK;
}

HRESULT ScrcpyMediaStream::GetStreamDescriptorReadOnly(IMFStreamDescriptor **ppSD) {
    if (!ppSD) return E_POINTER;
    *ppSD = m_streamDescriptor.Get();
    if (*ppSD) (*ppSD)->AddRef();
    return *ppSD ? S_OK : MF_E_NOT_INITIALIZED;
}

HRESULT ScrcpyMediaStream::SetAllocator(IUnknown *pAllocator) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) return MF_E_SHUTDOWN;
    m_allocator.Reset();
    if (!pAllocator) return E_POINTER;
    return pAllocator->QueryInterface(IID_PPV_ARGS(&m_allocator));
}

HRESULT ScrcpyMediaStream::SetD3DManager(IUnknown *pManager) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) return MF_E_SHUTDOWN;
    // Keep CPU cross-process allocator active (do not bind D3D texture manager
    // on m_allocator so AllocateSample produces CPU-lockable IMF2DBuffers).
    m_d3dManager = pManager;
    return S_OK;
}

HRESULT ScrcpyMediaStream::Start(IMFMediaType *pMediaType) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;

        ComPtr<IMFMediaType> activeType = pMediaType;
        if (!activeType) {
            ComPtr<IMFMediaTypeHandler> handler;
            HRESULT hr = m_streamDescriptor->GetMediaTypeHandler(&handler);
            if (FAILED(hr)) return hr;
            hr = handler->GetCurrentMediaType(&activeType);
            if (FAILED(hr)) return hr;
        } else {
            ComPtr<IMFMediaTypeHandler> handler;
            if (SUCCEEDED(m_streamDescriptor->GetMediaTypeHandler(&handler))) {
                handler->SetCurrentMediaType(activeType.Get());
            }
        }

        UINT32 w = 1920, h = 1080, fpsNum = 30, fpsDen = 1;
        MFGetAttributeSize(activeType.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(activeType.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        if (fpsDen == 0) fpsDen = 1;
        uint32_t fps = std::max(1u, fpsNum / fpsDen);

        m_width = w;
        m_height = h;
        m_fps = fps;
        m_frameDuration100ns = 10000000LL / static_cast<LONGLONG>(m_fps);
        m_scratchNV12.resize((m_width * m_height * 3u) / 2u);

        if (m_allocator) {
            m_allocator->UninitializeSampleAllocator();
            HRESULT hrAlloc = m_allocator->InitializeSampleAllocator(8, activeType.Get());
            if (FAILED(hrAlloc)) {
                // Fall back to MFCreate2DMediaBuffer in ProduceSample
                m_allocator.Reset();
            }
        }

        m_state = MF_STREAM_STATE_RUNNING;
        queue = m_eventQueue;
    }

    if (m_wakeEvent) SetEvent(m_wakeEvent);

    PROPVARIANT pv;
    PropVariantInit(&pv);
    HRESULT hr = queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &pv);
    PropVariantClear(&pv);
    return hr;
}

HRESULT ScrcpyMediaStream::Stop() {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        m_state = MF_STREAM_STATE_STOPPED;
        m_tokenQueue.clear();
        if (m_allocator) {
            m_allocator->UninitializeSampleAllocator();
        }
        queue = m_eventQueue;
    }
    return queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void ScrcpyMediaStream::ShutdownStream() {
    m_workerStop = true;
    if (m_wakeEvent) {
        SetEvent(m_wakeEvent);
    }
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) return;
    m_shutdown = true;
    m_state = MF_STREAM_STATE_STOPPED;
    m_tokenQueue.clear();

    if (m_allocator) {
        m_allocator->UninitializeSampleAllocator();
        m_allocator.Reset();
    }
    if (m_eventQueue) {
        m_eventQueue->Shutdown();
    }
    if (m_wakeEvent) {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }
    UnmapSharedMemory();
    m_parent = nullptr;
}

bool ScrcpyMediaStream::EnsureSharedMemoryMapped() {
    if (m_shmView) {
        return true;
    }

    std::wstring path = GetSharedMemoryFilePath();
    m_shmFile = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_shmFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(m_shmFile, &sz) ||
        static_cast<size_t>(sz.QuadPart) < sizeof(scrcpy_vcam_shm)) {
        CloseHandle(m_shmFile);
        m_shmFile = INVALID_HANDLE_VALUE;
        return false;
    }

    m_shmMapping = CreateFileMappingW(m_shmFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m_shmMapping) {
        CloseHandle(m_shmFile);
        m_shmFile = INVALID_HANDLE_VALUE;
        return false;
    }

    m_shmView = static_cast<const scrcpy_vcam_shm *>(
        MapViewOfFile(m_shmMapping, FILE_MAP_READ, 0, 0, sizeof(scrcpy_vcam_shm)));
    if (!m_shmView) {
        CloseHandle(m_shmMapping);
        m_shmMapping = nullptr;
        CloseHandle(m_shmFile);
        m_shmFile = INVALID_HANDLE_VALUE;
        return false;
    }

    return true;
}

void ScrcpyMediaStream::UnmapSharedMemory() {
    if (m_shmView) {
        UnmapViewOfFile(m_shmView);
        m_shmView = nullptr;
    }
    if (m_shmMapping) {
        CloseHandle(m_shmMapping);
        m_shmMapping = nullptr;
    }
    if (m_shmFile != INVALID_HANDLE_VALUE) {
        CloseHandle(m_shmFile);
        m_shmFile = INVALID_HANDLE_VALUE;
    }
}

void ScrcpyMediaStream::FillFrameNV12(uint8_t *dstNV12, uint32_t width, uint32_t height, uint64_t tickIndex) {
    bool live = false;
    if (EnsureSharedMemoryMapped() &&
        m_shmView->magic == SCRCPY_VCAM_MAGIC &&
        m_shmView->version == SCRCPY_VCAM_VERSION &&
        m_shmView->active != 0) {

        LARGE_INTEGER nowQpc = {};
        QueryPerformanceCounter(&nowQpc);
        int64_t freq = m_shmView->qpc_freq;
        int64_t lastQpc = m_shmView->timestamp_qpc;
        MemoryBarrier();

        // Treat producer as live if last frame arrived within 2.5 seconds
        if (freq > 0 && (nowQpc.QuadPart - lastQpc) < (freq * 5) / 2) {
            uint32_t idx = m_shmView->write_index & 1u;
            MemoryBarrier();
            const uint8_t *srcSlot = m_shmView->slots[idx];

            if (width == 1920u && height == 1080u) {
                memcpy(dstNV12, srcSlot, SCRCPY_VCAM_FRAME_SIZE);
                live = true;
            } else if (width == 1280u && height == 720u) {
                DownscaleNV12_1080_to_720(srcSlot, dstNV12);
                live = true;
            }
        }
    }

    if (!live) {
        RenderStandbyNV12(dstNV12, width, height, tickIndex);
    }
}

HRESULT ScrcpyMediaStream::ProduceSample(IUnknown *pToken, uint64_t tickIndex) {
    ComPtr<IMFVideoSampleAllocator> allocator;
    ComPtr<IMFMediaEventQueue> queue;
    uint32_t width = 0, height = 0;
    LONGLONG duration = 333333;
    uint8_t *scratch = nullptr;
    DWORD frameBytes = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown || m_state != MF_STREAM_STATE_RUNNING) {
            return S_FALSE;
        }
        allocator = m_allocator;
        queue = m_eventQueue;
        width = m_width;
        height = m_height;
        duration = m_frameDuration100ns;
        frameBytes = (width * height * 3u) / 2u;
        if (m_scratchNV12.size() < frameBytes) {
            m_scratchNV12.resize(frameBytes);
        }
        scratch = m_scratchNV12.data();
    }

    FillFrameNV12(scratch, width, height, tickIndex);

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> mediaBuffer;
    HRESULT hr = E_FAIL;

    if (allocator) {
        hr = allocator->AllocateSample(&sample);
        if (SUCCEEDED(hr) && sample) {
            hr = sample->GetBufferByIndex(0, &mediaBuffer);
        }
    }

    if (FAILED(hr) || !mediaBuffer) {
        sample.Reset();
        mediaBuffer.Reset();
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return hr;
        hr = MFCreate2DMediaBuffer(width, height, MFVideoFormat_NV12.Data1, FALSE, &mediaBuffer);
        if (FAILED(hr)) return hr;
        hr = sample->AddBuffer(mediaBuffer.Get());
        if (FAILED(hr)) return hr;
    }

    ComPtr<IMF2DBuffer> buf2D;
    if (SUCCEEDED(mediaBuffer.As(&buf2D))) {
        hr = buf2D->ContiguousCopyFrom(scratch, frameBytes);
        if (FAILED(hr)) return hr;
    } else {
        BYTE *dstPtr = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = mediaBuffer->Lock(&dstPtr, &maxLen, &curLen);
        if (FAILED(hr)) return hr;
        DWORD toCopy = std::min(maxLen, frameBytes);
        memcpy(dstPtr, scratch, toCopy);
        mediaBuffer->Unlock();
        mediaBuffer->SetCurrentLength(toCopy);
    }

    LONGLONG now100ns = MFGetSystemTime();
    sample->SetSampleTime(now100ns);
    sample->SetSampleDuration(duration);
    sample->SetUINT64(MFSampleExtension_DeviceTimestamp, static_cast<UINT64>(now100ns));
    sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (pToken) {
        sample->SetUnknown(MFSampleExtension_Token, pToken);
    }

    return queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

void ScrcpyMediaStream::WorkerLoop() {
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    uint64_t tickIndex = 0;
    LONGLONG nextDeadline100ns = 0;

    while (!m_workerStop) {
        ComPtr<IUnknown> token;
        bool hasRequest = false;
        LONGLONG interval100ns = 333333;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            interval100ns = m_frameDuration100ns;
            if (!m_shutdown && m_state == MF_STREAM_STATE_RUNNING && !m_tokenQueue.empty()) {
                token = std::move(m_tokenQueue.front());
                m_tokenQueue.pop_front();
                hasRequest = true;
            }
        }

        if (!hasRequest) {
            nextDeadline100ns = 0;
            WaitForSingleObject(m_wakeEvent, 50);
            continue;
        }

        LONGLONG now100ns = MFGetSystemTime();
        if (nextDeadline100ns == 0) {
            nextDeadline100ns = now100ns;
        } else if (now100ns < nextDeadline100ns && timer) {
            LARGE_INTEGER due = {};
            due.QuadPart = -(nextDeadline100ns - now100ns); // relative 100ns
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                HANDLE waits[2] = {timer, m_wakeEvent};
                WaitForMultipleObjects(2, waits, FALSE, 100);
            }
        }

        if (m_workerStop) break;

        ProduceSample(token.Get(), tickIndex++);

        now100ns = MFGetSystemTime();
        nextDeadline100ns += interval100ns;
        if (nextDeadline100ns + interval100ns < now100ns) {
            nextDeadline100ns = now100ns;
        }
    }

    if (timer) {
        CloseHandle(timer);
    }
}

STDMETHODIMP ScrcpyMediaStream::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == IID_IMFMediaEventGenerator ||
        riid == IID_IMFMediaStream ||
        riid == IID_IMFMediaStream2) {
        *ppv = static_cast<IMFMediaStream2 *>(this);
    } else if (riid == __uuidof(IKsControl)) {
        *ppv = static_cast<IKsControl *>(this);
    } else if (riid == IID_IMFGetService) {
        *ppv = static_cast<IMFGetService *>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) ScrcpyMediaStream::AddRef() {
    return ++m_refCount;
}

STDMETHODIMP_(ULONG) ScrcpyMediaStream::Release() {
    ULONG count = --m_refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

STDMETHODIMP ScrcpyMediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        queue = m_eventQueue;
    }
    return queue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP ScrcpyMediaStream::BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        queue = m_eventQueue;
    }
    return queue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP ScrcpyMediaStream::EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        queue = m_eventQueue;
    }
    return queue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP ScrcpyMediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                           HRESULT hrStatus, const PROPVARIANT *pvValue) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        queue = m_eventQueue;
    }
    return queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

STDMETHODIMP ScrcpyMediaStream::GetMediaSource(IMFMediaSource **ppMediaSource) {
    if (!ppMediaSource) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown || !m_parent) return MF_E_SHUTDOWN;
    *ppMediaSource = m_parent;
    (*ppMediaSource)->AddRef();
    return S_OK;
}

STDMETHODIMP ScrcpyMediaStream::GetStreamDescriptor(IMFStreamDescriptor **ppStreamDescriptor) {
    if (!ppStreamDescriptor) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (!m_streamDescriptor) return MF_E_NOT_INITIALIZED;
    *ppStreamDescriptor = m_streamDescriptor.Get();
    (*ppStreamDescriptor)->AddRef();
    return S_OK;
}

STDMETHODIMP ScrcpyMediaStream::RequestSample(IUnknown *pToken) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        if (m_state == MF_STREAM_STATE_STOPPED) return MF_E_INVALIDREQUEST;
        m_tokenQueue.emplace_back(pToken);
    }
    if (m_wakeEvent) {
        SetEvent(m_wakeEvent);
    }
    return S_OK;
}

STDMETHODIMP ScrcpyMediaStream::SetStreamState(MF_STREAM_STATE value) {
    ComPtr<IMFMediaEventQueue> queue;
    MF_STREAM_STATE prevState;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        prevState = m_state;
        if (prevState == value) {
            return S_OK;
        }
        m_state = value;
        queue = m_eventQueue;

        if (value == MF_STREAM_STATE_STOPPED) {
            m_tokenQueue.clear();
            if (m_allocator) {
                m_allocator->UninitializeSampleAllocator();
            }
        } else if (value == MF_STREAM_STATE_RUNNING) {
            if (m_allocator && prevState == MF_STREAM_STATE_STOPPED) {
                ComPtr<IMFMediaTypeHandler> handler;
                ComPtr<IMFMediaType> mt;
                if (SUCCEEDED(m_streamDescriptor->GetMediaTypeHandler(&handler)) &&
                    SUCCEEDED(handler->GetCurrentMediaType(&mt))) {
                    m_allocator->InitializeSampleAllocator(8, mt.Get());
                }
            }
        }
    }

    if (m_wakeEvent) {
        SetEvent(m_wakeEvent);
    }

    if (value == MF_STREAM_STATE_RUNNING) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        HRESULT hr = queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &pv);
        PropVariantClear(&pv);
        return hr;
    } else if (value == MF_STREAM_STATE_PAUSED) {
        return queue->QueueEventParamVar(MEStreamPaused, GUID_NULL, S_OK, nullptr);
    } else {
        return queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }
}

STDMETHODIMP ScrcpyMediaStream::GetStreamState(MF_STREAM_STATE *value) {
    if (!value) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) return MF_E_SHUTDOWN;
    *value = m_state;
    return S_OK;
}

STDMETHODIMP ScrcpyMediaStream::KsProperty(PKSPROPERTY Property, ULONG PropertyLength,
                                           LPVOID PropertyData, ULONG DataLength,
                                           ULONG *BytesReturned) {
    if (!Property) return E_POINTER;
    static const GUID kGuidCameraControl = {
        0xc6e13370, 0x30ac, 0x11d0, { 0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56 }
    };
    if (IsEqualGUID(Property->Set, kGuidCameraControl) && Property->Id == 8 /* KSPROPERTY_CAMERACONTROL_PRIVACY */) {
        if (Property->Flags & KSPROPERTY_TYPE_GET) {
            if (DataLength >= sizeof(KSPROPERTY_CAMERACONTROL_S)) {
                auto *p = reinterpret_cast<PKSPROPERTY_CAMERACONTROL_S>(PropertyData);
                p->Value = 0; // Not private
                p->Flags = KSPROPERTY_CAMERACONTROL_FLAGS_MANUAL;
                p->Capabilities = KSPROPERTY_CAMERACONTROL_FLAGS_MANUAL;
                if (BytesReturned) *BytesReturned = sizeof(KSPROPERTY_CAMERACONTROL_S);
                return S_OK;
            } else if (DataLength >= sizeof(LONG)) {
                *reinterpret_cast<LONG *>(PropertyData) = 0;
                if (BytesReturned) *BytesReturned = sizeof(LONG);
                return S_OK;
            }
        }
    }
    (void)PropertyLength; (void)PropertyData; (void)DataLength;
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP ScrcpyMediaStream::KsMethod(PKSMETHOD Method, ULONG MethodLength,
                                         LPVOID MethodData, ULONG DataLength,
                                         ULONG *BytesReturned) {
    (void)Method; (void)MethodLength; (void)MethodData; (void)DataLength;
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP ScrcpyMediaStream::KsEvent(PKSEVENT Event, ULONG EventLength,
                                        LPVOID EventData, ULONG DataLength,
                                        ULONG *BytesReturned) {
    (void)Event; (void)EventLength; (void)EventData; (void)DataLength;
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP ScrcpyMediaStream::GetService(REFGUID guidService, REFIID riid, LPVOID *ppvObject) {
    (void)guidService; (void)riid;
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// ============================================================================
// ScrcpyMediaSource
// ============================================================================

ScrcpyMediaSource::ScrcpyMediaSource() {
    LogMsg(L"[ScrcpyMediaSource::ctor] this=%p\n", this);
    ++g_dllRefCount;
}

ScrcpyMediaSource::~ScrcpyMediaSource() {
    LogMsg(L"[ScrcpyMediaSource::dtor] this=%p\n", this);
    Shutdown();
    --g_dllRefCount;
}

HRESULT ScrcpyMediaSource::Initialize() {
    LogMsg(L"[ScrcpyMediaSource::Initialize] this=%p\n", this);
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return hr;

    hr = MFCreateEventQueue(&m_eventQueue);
    if (FAILED(hr)) return hr;

    hr = MFCreateAttributes(&m_attributes, 4);
    if (FAILED(hr)) return hr;

    // Sensor profiles required by Windows 11 Camera FrameServer
    ComPtr<IMFSensorProfileCollection> profileCollection;
    hr = MFCreateSensorProfileCollection(&profileCollection);
    if (SUCCEEDED(hr)) {
        ComPtr<IMFSensorProfile> legacyProfile;
        if (SUCCEEDED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &legacyProfile))) {
            legacyProfile->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))");
            profileCollection->AddProfile(legacyProfile.Get());
        }
        ComPtr<IMFSensorProfile> hfrProfile;
        if (SUCCEEDED(MFCreateSensorProfile(KSCAMERAPROFILE_HighFrameRate, 0, nullptr, &hfrProfile))) {
            hfrProfile->AddProfileFilter(0, L"((RES==;FRT>=60,1;SUT==))");
            profileCollection->AddProfile(hfrProfile.Get());
        }
        m_attributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profileCollection.Get());
    }

    m_stream.Attach(new (std::nothrow) ScrcpyMediaStream(this, 0));
    if (!m_stream) return E_OUTOFMEMORY;

    hr = m_stream->Initialize();
    if (FAILED(hr)) return hr;

    ComPtr<IMFStreamDescriptor> sd;
    hr = m_stream->GetStreamDescriptorReadOnly(&sd);
    if (FAILED(hr)) return hr;

    IMFStreamDescriptor *sds[1] = {sd.Get()};
    hr = MFCreatePresentationDescriptor(1, sds, &m_presentationDescriptor);
    if (FAILED(hr)) return hr;

    hr = m_presentationDescriptor->SelectStream(0);
    return hr;
}

STDMETHODIMP ScrcpyMediaSource::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;

    if (riid == IID_IUnknown ||
        riid == IID_IMFMediaEventGenerator ||
        riid == IID_IMFMediaSource ||
        riid == IID_IMFMediaSourceEx) {
        *ppv = static_cast<IMFMediaSourceEx *>(this);
    } else if (riid == IID_IMFGetService) {
        *ppv = static_cast<IMFGetService *>(this);
    } else if (riid == __uuidof(IKsControl)) {
        *ppv = static_cast<IKsControl *>(this);
    } else if (riid == IID_IMFSampleAllocatorControl) {
        *ppv = static_cast<IMFSampleAllocatorControl *>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) ScrcpyMediaSource::AddRef() {
    return ++m_refCount;
}

STDMETHODIMP_(ULONG) ScrcpyMediaSource::Release() {
    ULONG count = --m_refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

STDMETHODIMP ScrcpyMediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        queue = m_eventQueue;
    }
    return queue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP ScrcpyMediaSource::BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        queue = m_eventQueue;
    }
    return queue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP ScrcpyMediaSource::EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        queue = m_eventQueue;
    }
    return queue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP ScrcpyMediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                           HRESULT hrStatus, const PROPVARIANT *pvValue) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        queue = m_eventQueue;
    }
    return queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

STDMETHODIMP ScrcpyMediaSource::GetCharacteristics(DWORD *pdwCharacteristics) {
    LogMsg(L"[ScrcpyMediaSource::GetCharacteristics] this=%p\n", this);
    if (!pdwCharacteristics) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) {
        LogMsg(L"   -> MF_E_SHUTDOWN!\n");
        return MF_E_SHUTDOWN;
    }
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP ScrcpyMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor **ppPD) {
    LogMsg(L"[ScrcpyMediaSource::CreatePresentationDescriptor] this=%p\n", this);
    if (!ppPD) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) {
        LogMsg(L"   -> MF_E_SHUTDOWN!\n");
        return MF_E_SHUTDOWN;
    }
    if (!m_presentationDescriptor) return MF_E_NOT_INITIALIZED;
    return m_presentationDescriptor->Clone(ppPD);
}

STDMETHODIMP ScrcpyMediaSource::Start(IMFPresentationDescriptor *pPD,
                                      const GUID *pguidTimeFormat,
                                      const PROPVARIANT *pvarStartPosition) {
    if (!pPD || !pvarStartPosition) return E_INVALIDARG;
    if (pguidTimeFormat && *pguidTimeFormat != GUID_NULL) {
        return MF_E_UNSUPPORTED_TIME_FORMAT;
    }

    ComPtr<IMFMediaEventQueue> queue;
    ComPtr<ScrcpyMediaStream> stream;
    ComPtr<IMFMediaType> selectedType;
    bool wasStarted = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;

        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        HRESULT hr = pPD->GetStreamDescriptorByIndex(0, &selected, &sd);
        if (FAILED(hr)) return hr;

        if (sd) {
            ComPtr<IMFMediaTypeHandler> handler;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&handler))) {
                handler->GetCurrentMediaType(&selectedType);
            }
        }

        wasStarted = m_started;
        m_started = true;
        queue = m_eventQueue;
        stream = m_stream;
    }

    MediaEventType streamEvt = wasStarted ? MEUpdatedStream : MENewStream;
    HRESULT hr = queue->QueueEventParamUnk(
        streamEvt, GUID_NULL, S_OK, static_cast<IMFMediaStream2 *>(stream.Get()));
    if (FAILED(hr)) return hr;

    PROPVARIANT pvStart;
    PropVariantInit(&pvStart);
    hr = queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &pvStart);
    PropVariantClear(&pvStart);
    if (FAILED(hr)) return hr;

    return stream->Start(selectedType.Get());
}

STDMETHODIMP ScrcpyMediaSource::Stop() {
    ComPtr<IMFMediaEventQueue> queue;
    ComPtr<ScrcpyMediaStream> stream;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        m_started = false;
        queue = m_eventQueue;
        stream = m_stream;
    }

    if (stream) {
        stream->Stop();
    }
    return queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP ScrcpyMediaSource::Pause() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) return MF_E_SHUTDOWN;
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP ScrcpyMediaSource::Shutdown() {
    LogMsg(L"[ScrcpyMediaSource::Shutdown] this=%p\n", this);
    ComPtr<ScrcpyMediaStream> stream;
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return S_OK;
        m_shutdown = true;
        m_started = false;
        stream = m_stream;
        queue = m_eventQueue;
        m_presentationDescriptor.Reset();
        m_attributes.Reset();
    }

    if (stream) {
        stream->ShutdownStream();
    }
    if (queue) {
        queue->Shutdown();
    }
    MFShutdown();
    return S_OK;
}

STDMETHODIMP ScrcpyMediaSource::GetSourceAttributes(IMFAttributes **ppAttributes) {
    LogMsg(L"[ScrcpyMediaSource::GetSourceAttributes] this=%p\n", this);
    if (!ppAttributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) {
        LogMsg(L"   -> GetSourceAttributes: MF_E_SHUTDOWN!\n");
        return MF_E_SHUTDOWN;
    }
    if (!m_attributes) return MF_E_NOT_INITIALIZED;
    *ppAttributes = m_attributes.Get();
    (*ppAttributes)->AddRef();
    return S_OK;
}

STDMETHODIMP ScrcpyMediaSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes **ppAttributes) {
    LogMsg(L"[ScrcpyMediaSource::GetStreamAttributes] this=%p id=%lu\n", this, dwStreamIdentifier);
    if (!ppAttributes) return E_POINTER;
    *ppAttributes = nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) {
        LogMsg(L"   -> GetStreamAttributes: MF_E_SHUTDOWN!\n");
        return MF_E_SHUTDOWN;
    }
    if (dwStreamIdentifier != 0 || !m_stream) return MF_E_INVALIDSTREAMNUMBER;

    ComPtr<IMFStreamDescriptor> sd;
    HRESULT hr = m_stream->GetStreamDescriptorReadOnly(&sd);
    if (FAILED(hr)) return hr;
    return sd.CopyTo(ppAttributes);
}

STDMETHODIMP ScrcpyMediaSource::SetD3DManager(IUnknown *pManager) {
    LogMsg(L"[ScrcpyMediaSource::SetD3DManager] this=%p\n", this);
    ComPtr<ScrcpyMediaStream> stream;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        stream = m_stream;
    }
    return stream ? stream->SetD3DManager(pManager) : S_OK;
}

STDMETHODIMP ScrcpyMediaSource::GetService(REFGUID guidService, REFIID riid, LPVOID *ppvObject) {
    wchar_t svcStr[64] = {}, riidStr[64] = {};
    StringFromGUID2(guidService, svcStr, 64);
    StringFromGUID2(riid, riidStr, 64);
    LogMsg(L"[ScrcpyMediaSource::GetService] this=%p svc=%s riid=%s\n", this, svcStr, riidStr);
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

STDMETHODIMP ScrcpyMediaSource::KsProperty(PKSPROPERTY Property, ULONG PropertyLength,
                                           LPVOID PropertyData, ULONG DataLength,
                                           ULONG *BytesReturned) {
    if (!Property) return E_POINTER;
    wchar_t setStr[64] = {};
    StringFromGUID2(Property->Set, setStr, 64);
    LogMsg(L"[ScrcpyMediaSource::KsProperty] this=%p Set=%s Id=%lu Flags=%lu\n",
           this, setStr, Property->Id, Property->Flags);

    static const GUID kGuidCameraControl = {
        0xc6e13370, 0x30ac, 0x11d0, { 0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56 }
    };

    if (IsEqualGUID(Property->Set, kGuidCameraControl) && Property->Id == 8 /* KSPROPERTY_CAMERACONTROL_PRIVACY */) {
        if (Property->Flags & KSPROPERTY_TYPE_GET) {
            if (DataLength >= sizeof(KSPROPERTY_CAMERACONTROL_S)) {
                auto *p = reinterpret_cast<PKSPROPERTY_CAMERACONTROL_S>(PropertyData);
                p->Value = 0; // 0 = Not private (streaming enabled)
                p->Flags = KSPROPERTY_CAMERACONTROL_FLAGS_MANUAL;
                p->Capabilities = KSPROPERTY_CAMERACONTROL_FLAGS_MANUAL;
                if (BytesReturned) *BytesReturned = sizeof(KSPROPERTY_CAMERACONTROL_S);
                LogMsg(L"   -> KsProperty Privacy GET: returned Value=0 (NOT private)\n");
                return S_OK;
            } else if (DataLength >= sizeof(LONG)) {
                *reinterpret_cast<LONG *>(PropertyData) = 0;
                if (BytesReturned) *BytesReturned = sizeof(LONG);
                LogMsg(L"   -> KsProperty Privacy GET (short): returned 0\n");
                return S_OK;
            }
        }
    }

    (void)PropertyLength; (void)PropertyData; (void)DataLength;
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP ScrcpyMediaSource::KsMethod(PKSMETHOD Method, ULONG MethodLength,
                                         LPVOID MethodData, ULONG DataLength,
                                         ULONG *BytesReturned) {
    (void)Method; (void)MethodLength; (void)MethodData; (void)DataLength;
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP ScrcpyMediaSource::KsEvent(PKSEVENT Event, ULONG EventLength,
                                        LPVOID EventData, ULONG DataLength,
                                        ULONG *BytesReturned) {
    (void)Event; (void)EventLength; (void)EventData; (void)DataLength;
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP ScrcpyMediaSource::SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown *pAllocator) {
    ComPtr<ScrcpyMediaStream> stream;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        if (dwOutputStreamID != 0 || !m_stream) return MF_E_INVALIDSTREAMNUMBER;
        stream = m_stream;
    }
    return stream->SetAllocator(pAllocator);
}

STDMETHODIMP ScrcpyMediaSource::GetAllocatorUsage(DWORD dwOutputStreamID,
                                                  DWORD *pdwInputStreamID,
                                                  MFSampleAllocatorUsage *peUsage) {
    if (!pdwInputStreamID || !peUsage) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (dwOutputStreamID != 0) return MF_E_INVALIDSTREAMNUMBER;

    *pdwInputStreamID = dwOutputStreamID;
    *peUsage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

// ---------------------------------------------------------------------------
// ScrcpyActivate (IMFActivate implementation for deferred activation)
// ---------------------------------------------------------------------------

ScrcpyActivate::ScrcpyActivate() {
    ++g_dllRefCount;
    HRESULT hr = MFCreateAttributes(&m_attributes, 32);
    if (SUCCEEDED(hr) && m_attributes) {
        static const GUID kGuidVideoCamera = {
            0xe5323777, 0xf976, 0x4f5b, { 0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44 }
        };
        m_attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                             MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        m_attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_CATEGORY,
                             kGuidVideoCamera);
        m_attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                               SCRCPY_VCAM_FRIENDLY_NAME);
        m_attributes->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 0);
    }
}

ScrcpyActivate::~ScrcpyActivate() {
    m_source.Reset();
    m_attributes.Reset();
    --g_dllRefCount;
}

STDMETHODIMP ScrcpyActivate::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown) {
        *ppv = static_cast<IUnknown *>(static_cast<IMFActivate *>(this));
    } else if (riid == IID_IMFActivate) {
        *ppv = static_cast<IMFActivate *>(this);
    } else if (riid == IID_IMFAttributes) {
        *ppv = static_cast<IMFAttributes *>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) ScrcpyActivate::AddRef() {
    return ++m_refCount;
}

STDMETHODIMP_(ULONG) ScrcpyActivate::Release() {
    ULONG count = --m_refCount;
    if (count == 0) {
        delete this;
    }
    return count;
}

STDMETHODIMP ScrcpyActivate::ActivateObject(REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_source) {
        auto *src = new (std::nothrow) ScrcpyMediaSource();
        if (!src) return E_OUTOFMEMORY;
        HRESULT hr = src->Initialize();
        if (FAILED(hr)) {
            src->Release();
            return hr;
        }
        m_source.Attach(src);
    }
    return m_source->QueryInterface(riid, ppv);
}

STDMETHODIMP ScrcpyActivate::ShutdownObject() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_source) {
        m_source->Shutdown();
        m_source.Reset();
    }
    return S_OK;
}

STDMETHODIMP ScrcpyActivate::DetachObject() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_source.Detach();
    return S_OK;
}

// IMFAttributes delegation
STDMETHODIMP ScrcpyActivate::GetItem(REFGUID guidKey, PROPVARIANT *pValue) {
    wchar_t kStr[64] = {};
    StringFromGUID2(guidKey, kStr, 64);
    HRESULT hr = m_attributes ? m_attributes->GetItem(guidKey, pValue) : E_UNEXPECTED;
    LogMsg(L"[ScrcpyActivate::GetItem] key=%s hr=0x%08lX\n", kStr, hr);
    return hr;
}
STDMETHODIMP ScrcpyActivate::GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE *pType) {
    return m_attributes ? m_attributes->GetItemType(guidKey, pType) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL *pbResult) {
    return m_attributes ? m_attributes->CompareItem(guidKey, Value, pbResult) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::Compare(IMFAttributes *pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL *pbResult) {
    return m_attributes ? m_attributes->Compare(pTheirs, MatchType, pbResult) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::GetUINT32(REFGUID guidKey, UINT32 *punValue) {
    wchar_t kStr[64] = {};
    StringFromGUID2(guidKey, kStr, 64);
    HRESULT hr = m_attributes ? m_attributes->GetUINT32(guidKey, punValue) : E_UNEXPECTED;
    LogMsg(L"[ScrcpyActivate::GetUINT32] key=%s val=%lu hr=0x%08lX\n", kStr, punValue ? *punValue : 0, hr);
    return hr;
}
STDMETHODIMP ScrcpyActivate::GetUINT64(REFGUID guidKey, UINT64 *punValue) {
    return m_attributes ? m_attributes->GetUINT64(guidKey, punValue) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::GetDouble(REFGUID guidKey, double *pfValue) {
    return m_attributes ? m_attributes->GetDouble(guidKey, pfValue) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::GetGUID(REFGUID guidKey, GUID *pguidValue) {
    wchar_t kStr[64] = {};
    StringFromGUID2(guidKey, kStr, 64);
    HRESULT hr = m_attributes ? m_attributes->GetGUID(guidKey, pguidValue) : E_UNEXPECTED;
    LogMsg(L"[ScrcpyActivate::GetGUID] key=%s hr=0x%08lX\n", kStr, hr);
    return hr;
}
STDMETHODIMP ScrcpyActivate::GetStringLength(REFGUID guidKey, UINT32 *pcchLength) {
    return m_attributes ? m_attributes->GetStringLength(guidKey, pcchLength) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32 *pcchLength) {
    wchar_t kStr[64] = {};
    StringFromGUID2(guidKey, kStr, 64);
    HRESULT hr = m_attributes ? m_attributes->GetString(guidKey, pwszValue, cchBufSize, pcchLength) : E_UNEXPECTED;
    LogMsg(L"[ScrcpyActivate::GetString] key=%s val=%s hr=0x%08lX\n", kStr, pwszValue ? pwszValue : L"", hr);
    return hr;
}
STDMETHODIMP ScrcpyActivate::GetAllocatedString(REFGUID guidKey, LPWSTR *ppwszValue, UINT32 *pcchLength) {
    wchar_t kStr[64] = {};
    StringFromGUID2(guidKey, kStr, 64);
    HRESULT hr = m_attributes ? m_attributes->GetAllocatedString(guidKey, ppwszValue, pcchLength) : E_UNEXPECTED;
    LogMsg(L"[ScrcpyActivate::GetAllocatedString] key=%s val=%s hr=0x%08lX\n", kStr, (ppwszValue && *ppwszValue) ? *ppwszValue : L"", hr);
    return hr;
}
STDMETHODIMP ScrcpyActivate::GetBlobSize(REFGUID guidKey, UINT32 *pcbBlobSize) {
    return m_attributes ? m_attributes->GetBlobSize(guidKey, pcbBlobSize) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::GetBlob(REFGUID guidKey, UINT8 *pBuf, UINT32 cbBufSize, UINT32 *pcbBlobSize) {
    return m_attributes ? m_attributes->GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::GetAllocatedBlob(REFGUID guidKey, UINT8 **ppBuf, UINT32 *pcbSize) {
    return m_attributes ? m_attributes->GetAllocatedBlob(guidKey, ppBuf, pcbSize) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::GetUnknown(REFGUID guidKey, REFIID riid, LPVOID *ppv) {
    wchar_t kStr[64] = {};
    StringFromGUID2(guidKey, kStr, 64);
    HRESULT hr = m_attributes ? m_attributes->GetUnknown(guidKey, riid, ppv) : E_UNEXPECTED;
    LogMsg(L"[ScrcpyActivate::GetUnknown] key=%s hr=0x%08lX\n", kStr, hr);
    return hr;
}
STDMETHODIMP ScrcpyActivate::SetItem(REFGUID guidKey, REFPROPVARIANT Value) {
    return m_attributes ? m_attributes->SetItem(guidKey, Value) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::DeleteItem(REFGUID guidKey) {
    return m_attributes ? m_attributes->DeleteItem(guidKey) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::DeleteAllItems() {
    return m_attributes ? m_attributes->DeleteAllItems() : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::SetUINT32(REFGUID guidKey, UINT32 unValue) {
    return m_attributes ? m_attributes->SetUINT32(guidKey, unValue) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::SetUINT64(REFGUID guidKey, UINT64 unValue) {
    return m_attributes ? m_attributes->SetUINT64(guidKey, unValue) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::SetDouble(REFGUID guidKey, double fValue) {
    return m_attributes ? m_attributes->SetDouble(guidKey, fValue) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::SetGUID(REFGUID guidKey, REFGUID guidValue) {
    return m_attributes ? m_attributes->SetGUID(guidKey, guidValue) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::SetString(REFGUID guidKey, LPCWSTR wszValue) {
    return m_attributes ? m_attributes->SetString(guidKey, wszValue) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::SetBlob(REFGUID guidKey, const UINT8 *pBuf, UINT32 cbBufSize) {
    return m_attributes ? m_attributes->SetBlob(guidKey, pBuf, cbBufSize) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::SetUnknown(REFGUID guidKey, IUnknown *pUnknown) {
    return m_attributes ? m_attributes->SetUnknown(guidKey, pUnknown) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::LockStore() {
    return m_attributes ? m_attributes->LockStore() : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::UnlockStore() {
    return m_attributes ? m_attributes->UnlockStore() : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::GetCount(UINT32 *pcItems) {
    return m_attributes ? m_attributes->GetCount(pcItems) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::GetItemByIndex(UINT32 unIndex, GUID *pguidKey, PROPVARIANT *pValue) {
    return m_attributes ? m_attributes->GetItemByIndex(unIndex, pguidKey, pValue) : E_UNEXPECTED;
}
STDMETHODIMP ScrcpyActivate::CopyAllItems(IMFAttributes *pDest) {
    return m_attributes ? m_attributes->CopyAllItems(pDest) : E_UNEXPECTED;
}

