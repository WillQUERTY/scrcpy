@echo off
setlocal
cd /d "%~dp0"

echo === Estado de Camaras y Memoria Compartida ===
"%~dp0scrcpy-vcam-ctl.exe" status
echo.
echo === Prueba de Captura Directa con Windows FrameServer ===
"%~dp0scrcpy-vcam-ctl.exe" test
echo.
pause

