#ifndef SC_VCAM_SINK_H
#define SC_VCAM_SINK_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#ifdef _WIN32
# include <winsock2.h>
# include <windows.h>
#endif

#include "frame_buffer.h"
#include "trait/frame_sink.h"
#include "util/thread.h"

struct scrcpy_vcam_shm;

struct sc_vcam_sink {
    struct sc_frame_sink frame_sink; // frame sink trait

    struct sc_frame_buffer fb;
    struct SwsContext *sws_ctx;

    sc_thread thread;
    sc_mutex mutex;
    sc_cond cond;
    bool has_frame;
    bool stopped;

    AVFrame *frame;

    uint32_t last_src_w;
    uint32_t last_src_h;

#ifdef _WIN32
    HANDLE shm_file;
    HANDLE shm_mapping;
    struct scrcpy_vcam_shm *shm;
#endif
};

bool
sc_vcam_sink_init(struct sc_vcam_sink *vc);

void
sc_vcam_sink_destroy(struct sc_vcam_sink *vc);

#endif

