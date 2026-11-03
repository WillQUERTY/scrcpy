#ifndef SCRCPY_VCAM_SHM_H
#define SCRCPY_VCAM_SHM_H

#include <stdint.h>

#define SCRCPY_VCAM_MAGIC      UINT64_C(0x5643414D53435250) /* "SCRPVCAM" */
#define SCRCPY_VCAM_VERSION    1u

#define SCRCPY_VCAM_WIDTH      1920u
#define SCRCPY_VCAM_HEIGHT     1080u
#define SCRCPY_VCAM_Y_SIZE     (SCRCPY_VCAM_WIDTH * SCRCPY_VCAM_HEIGHT)
#define SCRCPY_VCAM_UV_SIZE    (SCRCPY_VCAM_WIDTH * (SCRCPY_VCAM_HEIGHT / 2u))
#define SCRCPY_VCAM_FRAME_SIZE (SCRCPY_VCAM_Y_SIZE + SCRCPY_VCAM_UV_SIZE)

#define SCRCPY_VCAM_DIR_REL    L"\\scrcpy-vcam"
#define SCRCPY_VCAM_SHM_REL    L"\\scrcpy-vcam\\frame_buffer.shm"
#define SCRCPY_VCAM_DLL_REL    L"\\scrcpy-vcam\\scrcpy-mfvcam.dll"
#define SCRCPY_VCAM_SYMLINK_REL L"\\scrcpy-vcam\\vcam_symlink.txt"

#define SCRCPY_VCAM_FRIENDLY_NAME L"scrcpy Virtual Camera"
#define SCRCPY_VCAM_CLSID_STR     L"{9E6B4F82-7C31-4D8A-B5E2-1F4A8C9D2E60}"

/*
 * Lock-free double-buffered NV12 shared memory layout mapped at:
 *   %ProgramData%\scrcpy-vcam\frame_buffer.shm
 *
 * Accessible across Windows Session 1 (scrcpy.exe running as normal user)
 * and Session 0 (Windows Camera FrameServer running as LOCAL SERVICE).
 */
struct scrcpy_vcam_shm {
    uint64_t magic;
    uint32_t version;
    volatile uint32_t active;        /* 1 while scrcpy --vcam is streaming */
    volatile uint32_t write_index;   /* 0 or 1: latest completed slot */
    uint32_t src_width;              /* original source width from Android */
    uint32_t src_height;             /* original source height from Android */
    uint32_t reserved0;
    volatile uint64_t frame_seq;     /* monotonically increasing frame counter */
    volatile int64_t timestamp_qpc;  /* QueryPerformanceCounter() of frame */
    volatile int64_t qpc_freq;       /* QueryPerformanceFrequency() */
    uint8_t reserved[8];
    uint8_t slots[2][SCRCPY_VCAM_FRAME_SIZE];
};

#endif /* SCRCPY_VCAM_SHM_H */

