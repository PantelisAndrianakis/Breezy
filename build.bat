@echo off
REM Breezy - toolchain bootstrap + build (Windows).
REM Installs the build dependencies if missing (via scoop, no admin needed),
REM then builds the compiler:
REM   - gcc  (MinGW-w64 C compiler)
REM   - nasm (assembler)
REM   - make (GNU Make)
REM
REM Usage:  build.bat   (run from a normal, NON-administrator command prompt)

setlocal enableextensions
cd /d "%~dp0"
echo ==^> Breezy build (Windows)

set "MISSING="
where gcc  >nul 2>&1 || set "MISSING=%MISSING% gcc"
where nasm >nul 2>&1 || set "MISSING=%MISSING% nasm"
where make >nul 2>&1 && goto have_make
where mingw32-make >nul 2>&1 && goto have_make
set "MISSING=%MISSING% make"
:have_make

if not "%MISSING%"=="" (
  echo ==^> Missing tools:%MISSING%  -- installing via scoop ^(no admin^)
  call :ensure_scoop || goto error
  call scoop install gcc nasm make
)

REM Make scoop's shims visible in this session.
if exist "%USERPROFILE%\scoop\shims" set "PATH=%USERPROFILE%\scoop\shims;%PATH%"

REM Verify the toolchain.
set "OK=1"
where gcc  >nul 2>&1 || (echo ERROR: gcc still missing  & set "OK=0")
where nasm >nul 2>&1 || (echo ERROR: nasm still missing & set "OK=0")
where make >nul 2>&1 || where mingw32-make >nul 2>&1 || (echo ERROR: make still missing & set "OK=0")
if not "%OK%"=="1" goto error

REM The compiler sources live under src\.
if not exist "src\main.c" (
  echo ==^> Toolchain ready.
  echo     Compiler sources are not present yet -- add them under src\,
  echo     then run build.bat again.
  goto done
)

echo ==^> Building the Breezy compiler...
where make >nul 2>&1
if not errorlevel 1 ( make ) else ( mingw32-make )
if errorlevel 1 goto error
echo ==^> Done -- 'breezy.exe' is built.
goto done

:ensure_scoop
where scoop >nul 2>&1 && exit /b 0
echo ==^> Installing scoop ^(user-scope, no admin^)...
REM The -ExecutionPolicy Bypass on this process already allows the installer to run,
REM so we do NOT call Set-ExecutionPolicy (it errors noisily under group policy).
powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-RestMethod -UseBasicParsing get.scoop.sh | Invoke-Expression"
if exist "%USERPROFILE%\scoop\shims" set "PATH=%USERPROFILE%\scoop\shims;%PATH%"
where scoop >nul 2>&1 && exit /b 0
echo ERROR: scoop install failed. Install scoop from https://scoop.sh then re-run, or install gcc/nasm/make manually.
exit /b 1

:error
echo.
echo Build failed. See messages above.
endlocal
exit /b 1

:done
endlocal
exit /b 0
