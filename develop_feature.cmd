@echo off
setlocal
set "WIZARD_ROOT=%~dp0"

where py >nul 2>nul
if not errorlevel 1 (
    py -3 "%WIZARD_ROOT%develop_feature" %*
    exit /b %errorlevel%
)

where python >nul 2>nul
if not errorlevel 1 (
    python "%WIZARD_ROOT%develop_feature" %*
    exit /b %errorlevel%
)

echo ERROR: Python 3 was not found. Install Python 3.9 or newer and retry.
exit /b 2
