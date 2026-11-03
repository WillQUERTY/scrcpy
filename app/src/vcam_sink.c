#include "vcam_sink.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../../vcam/include/scrcpy_vcam_shm.h"
#include "util/log.h"

/** Downcast frame_sink to sc_vcam_sink */
#define DOWNCAST(SINK) container_of(SINK, struct sc_vcam_sink, frame_sink)

static void
clear_nv12_slot(uint8_t *slot) {
    memset(slot, 16, SCRCPY_VCAM_Y_SIZE);
    memset(slot + SCRCPY_VCAM_Y_SIZE, 128, SCRCPY_VCAM_UV_SIZE);
}

static bool
write_frame_to_shm(struct sc_vcam_sink *vc, const AVFrame *frame) {
    if (frame->width <= 0 || frame->height <= 0) {
        return true;
    }

    uint32_t src_w = (uint32_t) frame->width;
    uint32_t src_h = (uint32_t) frame->height;

    // Fit inside 1920x1080 preserving aspect ratio (even-aligned for NV12)
    uint32_t dst_w = SCRCPY_VCAM_WIDTH;
    uint32_t dst_h = SCRCPY_VCAM_HEIGHT;
    if ((uint64_t) src_w * SCRCPY_VCAM_HEIGHT >
            (uint64_t) src_h * SCRCPY_VCAM_WIDTH) {
        dst_w = SCRCPY_VCAM_WIDTH;
        dst_h = (uint32_t) (((uint64_t) src_h * SCRCPY_VCAM_WIDTH) / src_w);
    } else {
        dst_h = SCRCPY_VCAM_HEIGHT;
        dst_w = (uint32_t) (((uint64_t) src_w * SCRCPY_VCAM_HEIGHT) / src_h);
    }
    dst_w = (dst_w < 2) ? 2 : (dst_w & ~1u);
    dst_h = (dst_h < 2) ? 2 : (dst_h & ~1u);

    uint32_t dst_x = ((SCRCPY_VCAM_WIDTH - dst_w) / 2u) & ~1u;
    uint32_t dst_y = ((SCRCPY_VCAM_HEIGHT - dst_h) / 2u) & ~1u;

    vc->sws_ctx = sws_getCachedContext(vc->sws_ctx,
                                       (int) src_w, (int) src_h, frame->format,
                                       (int) dst_w, (int) dst_h, AV_PIX_FMT_NV12,
                                       SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!vc->sws_ctx) {
        LOGE("Could not initialize swscale context for virtual camera");
        return false;
    }

    if (src_w != vc->last_src_w || src_h != vc->last_src_h) {
        clear_nv12_slot(vc->shm->slots[0]);
        clear_nv12_slot(vc->shm->slots[1]);
        vc->last_src_w = src_w;
        vc->last_src_h = src_h;
        LOGI("Virtual camera stream geometry: %" PRIu32 "x%" PRIu32
             " -> %" PRIu32 "x%" PRIu32 " (in 1920x1080 NV12)",
             src_w, src_h, dst_w, dst_h);
    }

    uint32_t next_slot = (vc->shm->write_index ^ 1u) & 1u;
    uint8_t *slot = vc->shm->slots[next_slot];

    uint8_t *dst_data[4] = {
        slot + dst_y * SCRCPY_VCAM_WIDTH + dst_x,
        slot + SCRCPY_VCAM_Y_SIZE + (dst_y / 2u) * SCRCPY_VCAM_WIDTH + dst_x,
        NULL,
        NULL,
    };
    int dst_linesize[4] = {
        (int) SCRCPY_VCAM_WIDTH,
        (int) SCRCPY_VCAM_WIDTH,
        0,
        0,
    };

    sws_scale(vc->sws_ctx,
              (const uint8_t * const *) frame->data, frame->linesize,
              0, frame->height, dst_data, dst_linesize);

    LARGE_INTEGER now_qpc;
    QueryPerformanceCounter(&now_qpc);

    MemoryBarrier();
    vc->shm->src_width = src_w;
    vc->shm->src_height = src_h;
    vc->shm->write_index = next_slot;
    vc->shm->timestamp_qpc = now_qpc.QuadPart;
    vc->shm->active = 1;
    vc->shm->frame_seq++;
    MemoryBarrier();

    return true;
}

static int
run_vcam_sink(void *data) {
    struct sc_vcam_sink *vc = data;

    for (;;) {
        sc_mutex_lock(&vc->mutex);

        while (!vc->stopped && !vc->has_frame) {
            sc_cond_wait(&vc->cond, &vc->mutex);
        }

        if (vc->stopped) {
            sc_mutex_unlock(&vc->mutex);
            break;
        }

        vc->has_frame = false;
        sc_frame_buffer_consume(&vc->fb, vc->frame);
        sc_mutex_unlock(&vc->mutex);

        bool ok = write_frame_to_shm(vc, vc->frame);
        av_frame_unref(vc->frame);
        if (!ok) {
            LOGE("Could not write frame to Windows virtual camera");
            break;
        }
    }

    LOGD("Windows virtual camera sink thread ended");
    return 0;
}

static bool
open_vcam_shared_memory(struct sc_vcam_sink *vc) {
    wchar_t prog_data[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", prog_data, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        wcscpy(prog_data, L"C:\\ProgramData");
    }

    wchar_t shm_path[MAX_PATH];
    int written = swprintf(shm_path, MAX_PATH, L"%ls%ls",
                           prog_data, SCRCPY_VCAM_SHM_REL);
    if (written <= 0 || written >= MAX_PATH) {
        LOGE("Virtual camera shared memory path too long");
        return false;
    }

    vc->shm_file = CreateFileW(shm_path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (vc->shm_file == INVALID_HANDLE_VALUE) {
        LOGE("Virtual camera driver is not installed yet (missing %ls, err=%lu).\n"
             "Run 'install_vcam.bat' (or 'scrcpy-vcam-ctl.exe install') once as Administrator.",
             shm_path, GetLastError());
        return false;
    }

    vc->shm_mapping = CreateFileMappingW(vc->shm_file, NULL, PAGE_READWRITE,
                                         0, (DWORD) sizeof(struct scrcpy_vcam_shm),
                                         NULL);
    if (!vc->shm_mapping) {
        LOGE("CreateFileMappingW failed for virtual camera (err=%lu)",
             GetLastError());
        CloseHandle(vc->shm_file);
        vc->shm_file = INVALID_HANDLE_VALUE;
        return false;
    }

    vc->shm = MapViewOfFile(vc->shm_mapping, FILE_MAP_ALL_ACCESS,
                            0, 0, sizeof(struct scrcpy_vcam_shm));
    if (!vc->shm) {
        LOGE("MapViewOfFile failed for virtual camera (err=%lu)",
             GetLastError());
        CloseHandle(vc->shm_mapping);
        vc->shm_mapping = NULL;
        CloseHandle(vc->shm_file);
        vc->shm_file = INVALID_HANDLE_VALUE;
        return false;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    vc->shm->magic = SCRCPY_VCAM_MAGIC;
    vc->shm->version = SCRCPY_VCAM_VERSION;
    vc->shm->qpc_freq = freq.QuadPart;
    vc->shm->active = 1;
    MemoryBarrier();

    return true;
}

static void
close_vcam_shared_memory(struct sc_vcam_sink *vc) {
    if (vc->shm) {
        vc->shm->active = 0;
        MemoryBarrier();
        UnmapViewOfFile(vc->shm);
        vc->shm = NULL;
    }
    if (vc->shm_mapping) {
        CloseHandle(vc->shm_mapping);
        vc->shm_mapping = NULL;
    }
    if (vc->shm_file != INVALID_HANDLE_VALUE) {
        CloseHandle(vc->shm_file);
        vc->shm_file = INVALID_HANDLE_VALUE;
    }
}

static bool
sc_vcam_sink_open(struct sc_vcam_sink *vc, const AVCodecContext *ctx,
                  const struct sc_stream_session *session) {
    (void) ctx;
    (void) session;

    if (!open_vcam_shared_memory(vc)) {
        return false;
    }

    if (!sc_frame_buffer_init(&vc->fb)) {
        goto error_close_shm;
    }

    if (!sc_mutex_init(&vc->mutex)) {
        goto error_fb_destroy;
    }

    if (!sc_cond_init(&vc->cond)) {
        goto error_mutex_destroy;
    }

    vc->frame = av_frame_alloc();
    if (!vc->frame) {
        LOG_OOM();
        goto error_cond_destroy;
    }

    vc->sws_ctx = NULL;
    vc->last_src_w = 0;
    vc->last_src_h = 0;
    vc->has_frame = false;
    vc->stopped = false;

    if (!sc_thread_create(&vc->thread, run_vcam_sink, "scrcpy-vcam", vc)) {
        LOGE("Could not start Windows virtual camera thread");
        goto error_frame_free;
    }

    LOGI("Streaming to Windows camera: \"scrcpy Virtual Camera\"");
    return true;

error_frame_free:
    av_frame_free(&vc->frame);
error_cond_destroy:
    sc_cond_destroy(&vc->cond);
error_mutex_destroy:
    sc_mutex_destroy(&vc->mutex);
error_fb_destroy:
    sc_frame_buffer_destroy(&vc->fb);
error_close_shm:
    close_vcam_shared_memory(vc);
    return false;
}

static void
sc_vcam_sink_close(struct sc_vcam_sink *vc) {
    sc_mutex_lock(&vc->mutex);
    vc->stopped = true;
    sc_cond_signal(&vc->cond);
    sc_mutex_unlock(&vc->mutex);

    sc_thread_join(&vc->thread, NULL);

    if (vc->sws_ctx) {
        sws_freeContext(vc->sws_ctx);
        vc->sws_ctx = NULL;
    }
    av_frame_free(&vc->frame);
    sc_cond_destroy(&vc->cond);
    sc_mutex_destroy(&vc->mutex);
    sc_frame_buffer_destroy(&vc->fb);
    close_vcam_shared_memory(vc);
}

static enum sc_sink_result
sc_vcam_sink_push(struct sc_vcam_sink *vc, const AVFrame *frame) {
    sc_mutex_lock(&vc->mutex);

    if (vc->stopped) {
        sc_mutex_unlock(&vc->mutex);
        return SC_SINK_STOPPED;
    }

    bool previous_skipped = sc_frame_buffer_has_frame(&vc->fb);
    bool ok = sc_frame_buffer_push(&vc->fb, frame);
    if (!ok) {
        sc_mutex_unlock(&vc->mutex);
        return SC_SINK_KO;
    }

    if (!previous_skipped) {
        vc->has_frame = true;
        sc_cond_signal(&vc->cond);
    }

    sc_mutex_unlock(&vc->mutex);
    return SC_SINK_OK;
}

static bool
sc_vcam_frame_sink_open(struct sc_frame_sink *sink, const AVCodecContext *ctx,
                        const struct sc_stream_session *session) {
    struct sc_vcam_sink *vc = DOWNCAST(sink);
    return sc_vcam_sink_open(vc, ctx, session);
}

static void
sc_vcam_frame_sink_close(struct sc_frame_sink *sink) {
    struct sc_vcam_sink *vc = DOWNCAST(sink);
    sc_vcam_sink_close(vc);
}

static enum sc_sink_result
sc_vcam_frame_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_vcam_sink *vc = DOWNCAST(sink);
    return sc_vcam_sink_push(vc, frame);
}

bool
sc_vcam_sink_init(struct sc_vcam_sink *vc) {
    vc->shm_file = INVALID_HANDLE_VALUE;
    vc->shm_mapping = NULL;
    vc->shm = NULL;
    vc->sws_ctx = NULL;

    static const struct sc_frame_sink_ops ops = {
        .open = sc_vcam_frame_sink_open,
        .close = sc_vcam_frame_sink_close,
        .push = sc_vcam_frame_sink_push,
    };

    vc->frame_sink.ops = &ops;
    return true;
}

void
sc_vcam_sink_destroy(struct sc_vcam_sink *vc) {
    (void) vc;
}

