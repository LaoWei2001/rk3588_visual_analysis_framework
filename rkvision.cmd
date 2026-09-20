@echo off
pushd "%~dp0"
where py >nul 2>nul
if %errorlevel% equ 0 (
    py -3 -m tools.rkvision %*
) else (
    python -m tools.rkvision %*
)
set RK_EXIT=%errorlevel%
popd
exit /b %RK_EXIT%
