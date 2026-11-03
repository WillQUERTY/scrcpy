@echo off
setlocal
cd /d "%~dp0"

if exist "%~dp0dist\scrcpy-vcam-ctl.exe" (
    cd /d "%~dp0dist"
)

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [INFO] Solicitando permisos de Administrador para registrar la camara virtual en Windows...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process cmd -ArgumentList '/k cd /d \"%CD%\" ^&^& scrcpy-vcam-ctl.exe install' -Verb RunAs"
    exit /b
)

net stop FrameServer /y >nul 2>&1
"%CD%\scrcpy-vcam-ctl.exe" install
echo.
pause
