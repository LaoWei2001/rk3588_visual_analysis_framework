@echo off
where py >nul 2>nul
if %errorlevel% equ 0 (
    py -3 "%~dp0tools\project\vision.py" %*
    exit /b
)
python "%~dp0tools\project\vision.py" %*
