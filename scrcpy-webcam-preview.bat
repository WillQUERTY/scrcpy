@echo off
setlocal
cd /d "%~dp0"

echo ========================================================
echo   Iniciando scrcpy Webcam Virtual (CON ventana previa)
echo ========================================================
echo.

"%~dp0scrcpy.exe" --video-source=camera --vcam %*
if %errorLevel% neq 0 (
    echo.
    echo [AVISO] Si es la primera vez que lo usas, recuerda ejecutar
    echo         install_vcam.bat como Administrador primero.
    echo.
    pause
)

