@echo off
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [INFO] Solicitando permisos de Administrador para desinstalar la camara virtual...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process cmd -ArgumentList '/c \"\"%~dp0scrcpy-vcam-ctl.exe\" uninstall & echo. & pause\"' -Verb RunAs"
    exit /b
)

"%~dp0scrcpy-vcam-ctl.exe" uninstall
echo.
pause

