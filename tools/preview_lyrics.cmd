@echo off
setlocal
where py >nul 2>&1 && (
  py -3 "%~dp0preview_lyrics.py" %*
  goto :done
)
where python >nul 2>&1 && (
  python "%~dp0preview_lyrics.py" %*
  goto :done
)
echo Python 3 is required to preview lyrics. Install it from https://www.python.org/downloads/ and run this again.
pause
exit /b 1
:done
if errorlevel 1 pause
