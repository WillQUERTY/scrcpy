@echo off
setlocal
cd /d "%~dp0"

echo ========================================================
echo   Iniciando scrcpy en modo Webcam Virtual (sin ventana)
echo ========================================================
echo.
echo Presiona Ctrl+C en esta consola para detener la transmision.
echo.

"%~dp0scrcpy.exe" --webcam %*
if %errorLevel% neq 0 (
    echo.
    echo [AVISO] Si es la primera vez que lo usas, recuerda ejecutar
    echo         install_vcam.bat como Administrador primero.
    echo.
    pause
)

