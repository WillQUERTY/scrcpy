# Windows Virtual Camera (Media Foundation)

On Windows 11 (21H2+ / Build 22000+), `scrcpy` can stream the video of your
Android device (camera or screen) directly into a **native Windows Virtual
Camera** using the official Windows Media Foundation Virtual Camera API
(`IMFVirtualCamera` / Frame Server).

This allows any Windows application (Discord, Google Meet, Zoom, Microsoft
Teams, OBS Studio, Chrome, Edge, and the native Windows Camera app) to select
**`scrcpy Virtual Camera`** as a standard webcam.

---

## 1. Initial One-Time Setup

To register the virtual camera driver in Windows, execute `install_vcam.bat`
(or run `scrcpy-vcam-ctl.exe install` from an Administrator command prompt):

```bat
install_vcam.bat
```

This performs the following one-time steps:
1. Copies `scrcpy-mfvcam.dll` to `%ProgramData%\scrcpy-vcam\`.
2. Sets explicit cross-session security permissions so `NT AUTHORITY\LOCAL SERVICE`
   (Windows Camera Frame Server) and your regular non-admin user account can
   access the shared memory pipeline.
3. Registers the COM media source (`CLSID {9E6B4F82-7C31-4D8A-B5E2-1F4A8C9D2E60}`).
4. Registers the system-wide camera with Windows: **`scrcpy Virtual Camera`**.

Once installed, **you never need administrator privileges again** to use it.

---

## 2. Using scrcpy as a Webcam (No Window)

Connect your Android phone via USB (with USB Debugging enabled) and run:

```bat
scrcpy --webcam
```

This single command automatically activates:
- Video source: Camera (`--video-source=camera`)
- Headless mode: No desktop window (`--no-window`)
- Audio forwarding disabled (`--no-audio`)
- Control injection disabled (`--no-control`)
- Windows virtual camera sink enabled (`--vcam`)

### Common Camera Customizations

Select the rear or front camera:
```bat
scrcpy --webcam --camera-facing=back
scrcpy --webcam --camera-facing=front
```

Request a specific camera resolution or framerate:
```bat
scrcpy --webcam --camera-size=1920x1080 --camera-fps=60
```

---

## 3. Using scrcpy Virtual Camera with a Desktop Preview Window

If you want to view the camera feed on your computer while simultaneously
sending it to Zoom/Meet/Discord:

```bat
scrcpy --video-source=camera --vcam
```

You can also double-click `scrcpy-webcam-preview.bat`.

---

## 4. Mirroring the Screen as a Webcam

You can also pipe the device's screen mirroring feed into the webcam:

```bat
scrcpy --vcam --no-window
```

---

## 5. Standby Screen & Diagnostics

When `scrcpy` is not running, the camera stream remains active and emits a
clean dark standby screen (`"SCRCPY VIRTUAL CAMERA - STANDBY"`). Applications
like Zoom or Teams will never crash or freeze waiting for frames.

To inspect the status of the camera and test capture directly:

```bat
scrcpy-vcam-ctl.exe status
scrcpy-vcam-ctl.exe test
```

---

## 6. Uninstallation

To cleanly remove the virtual camera from Windows:

```bat
uninstall_vcam.bat
```

