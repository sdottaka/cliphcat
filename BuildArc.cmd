pushd "%~dp0"

setlocal
set SAFEAPPVER=0.1.0
  )
)
set DISTDIR=.\Build\Releases
set path="%ProgramFiles%\7-zip";"%ProgramFiles(x86)%\7-zip";%path%

if "%1" == "" (
  call :BuildZip x86
  call :BuildZip x64
  call :BuildZip ARM
  call :BuildZip ARM64
) else (
  call :BuildZip %1 
)

popd
goto :eof


:BuildZip

set PLATFORM=%1
if "%1" == "x86" (
  set PLATFORMH=
) else (
  set PLATFORMH=%1-
)

echo.
echo ============================================================
echo BUILD cliphcat-%SAFEAPPVER%-%PLATFORMH%exe.zip...
echo ============================================================

rmdir /q /s "%DISTDIR%\%PLATFORMH%zip-version" > NUL 2> NUL
mkdir "%DISTDIR%" 2> NUL
mkdir "%DISTDIR%\%PLATFORMH%zip-version" 2> NUL
mkdir "%DISTDIR%\%PLATFORMH%zip-version\cliphcat" 2> NUL

rem Excecutables
echo Copy Excecutables...
copy Build\%PLATFORM%\Release\cliphcat.exe "%DISTDIR%\%PLATFORMH%zip-version\cliphcat\" > NUL
copy LICENSE "%DISTDIR%\%PLATFORMH%zip-version\cliphcat\" > NUL

echo.
echo ------------------------------------------------------------
echo Pack archive...
echo ------------------------------------------------------------
7z.exe a -tzip "%DISTDIR%\cliphcat-%SAFEAPPVER%-%PLATFORMH%exe.zip" "%DISTDIR%\%PLATFORMH%zip-version\cliphcat\"

goto :eof
