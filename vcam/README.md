# scrcpy Windows Virtual Camera (Media Foundation)

High-performance, low-latency native Virtual Camera for Windows 11 powered by the official Windows Media Foundation Virtual Camera API (`IMFVirtualCamera`).

This component allows `scrcpy` to output the video feed of an Android device directly as a system-wide webcam, visible in all Windows applications (Google Meet, Zoom, Microsoft Teams, OBS Studio, Discord, Windows Camera App, WebRTC browsers) **without requiring third-party virtual webcam drivers, OBS plugins, or mobile helper apps**.

---

## Architecture Overview

```
 +---------------------------------------------------------+
 |                      Android Device                     |
 |  Camera2 API via app_process (scrcpy-server)            |
 +---------------------------+-----------------------------+
                             |  H.264 / H.265 stream (USB / TCP)
                             v
 +---------------------------------------------------------+
 |                       scrcpy.exe                        |
 |  1. Hardware / FFmpeg video decoder                     |
 |  2. vcam_sink: scale/letterbox to NV12 via swscale      |
 |  3. Lock-free double-buffered Shared Memory (SHM)       |
 +---------------------------+-----------------------------+
                             |  Shared Memory (%ProgramData%\scrcpy-vcam\frame_buffer.shm)
                             v
 +---------------------------------------------------------+
 |               Windows Media Foundation                  |
 |  scrcpy-mfvcam.dll (InprocServer32 COM registered)      |
 |  - IMFVirtualCamera / IMFActivate / IMFMediaSourceEx    |
 |  - Windows Camera Frame Server (FrameServer service)    |
 |  - Standby Fallback: animated NV12 placeholder overlay  |
 +---------------------------+-----------------------------+
                             |  DirectShow / MediaFoundation / WebRTC
                             v
 +---------------------------------------------------------+
 |           Applications (Zoom, Meet, OBS, Teams)         |
 |   Selected Camera: "scrcpy Virtual Camera"              |
 +---------------------------------------------------------+
```

### Key Technical Characteristics
- **Windows 11 Native**: Uses `IMFVirtualCamera` (`MFVirtualCameraType_SoftwareCameraSource`) with `MFVirtualCameraLifetime_System` and `MFVirtualCameraAccess_AllUsers`.
- **Zero Kernel Overhead**: Pure user-mode COM component executed under Windows Camera Frame Server isolation.
- **Lock-Free Double Buffering**: The shared memory buffer uses atomic sequence counters (`frame_seq`) to ensure tear-free frame reads without blocking or locking mutexes between processes.
- **Standby Watchdog**: When `scrcpy` is disconnected or not streaming, the virtual camera automatically serves an animated 1080p standby placeholder with status text, preventing client applications (Zoom/Teams) from freezing.
- **Multi-Resolution Negotiation**: Supports 1080p@30/60fps and 720p@60fps formats.

---

## Project Structure

```
vcam/
├── CMakeLists.txt            # CMake build definition for MSVC
├── include/
│   └── scrcpy_vcam_shm.h     # Shared memory layout and synchronization protocol
├── src/
│   ├── dllmain.cpp           # In-process COM Server DLL entry point and ClassFactory
│   ├── mfvcam_source.h       # Media Foundation MediaSource, MediaStream, IMFActivate
│   ├── mfvcam_source.cpp     # IMFMediaSourceEx implementation and NV12 bitmap font renderer
│   └── vcam_ctl.cpp          # Management CLI (install, uninstall, status, test)
```

---

## Building

### Requirements
- Windows 11 Build 22000 or newer
- Visual Studio 2022 / 2025 Community with C++ Desktop Development (MSVC v143+)
- Windows 10/11 SDK 10.0.22000.0 or newer
- CMake 3.20+

### Build Steps
```cmd
cd vcam
cmake -B build -G "Visual Studio 18 2025" -A x64
cmake --build build --config Release
```

Output binaries will be generated in `vcam/build/Release/`:
- `scrcpy-mfvcam.dll`: The Virtual Camera COM server.
- `scrcpy-vcam-ctl.exe`: The installation and diagnostics utility.

---

## Usage

### 1. Install Virtual Camera (Administrator required)
```cmd
scrcpy-vcam-ctl.exe install
```
This registers `{9E6B4F82-7C31-4D8A-B5E2-1F4A8C9D2E60}` in the Windows Registry, initializes the shared memory file, and activates the virtual camera in Windows Frame Server.

### 2. Stream Android Camera
```cmd
scrcpy.exe --webcam
```
Shortcut for: `--video-source=camera --no-window --no-audio --no-control --vcam`.

To display a local preview window simultaneously:
```cmd
scrcpy.exe --video-source=camera --vcam
```

### 3. Check Status
```cmd
scrcpy-vcam-ctl.exe status
```

### 4. Uninstall
```cmd
scrcpy-vcam-ctl.exe uninstall
```

