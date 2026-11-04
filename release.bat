@echo off
set "GAME_EXE=MOHRS"
cd /d "%~dp0"
set "OUT=out\build\release"
rem the Visual C++ runtime beside the executable, so a fresh Windows install needs nothing more
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%VCRUNTIME%\vcruntime140.dll" (echo Visual C++ runtime not found: install Visual Studio with the C++ workload & goto :done)
if not exist "%OUT%\%GAME_EXE%.exe" call build.bat || goto :done
rem the configure step writes where the SDK is
if not exist "%OUT%\sdk_dir.txt" cmake --preset release >nul || goto :done
if not exist "%OUT%\sdk_dir.txt" echo %OUT%\sdk_dir.txt was not written, update the SDK. && goto :done
set /p SDK=<out\build\release\sdk_dir.txt
del /q "%GAME_EXE%.zip" 2>nul
rem the zip holds one MOHRS folder, gathered first: the exes, an empty assets folder where the game (files or .iso)
rem goes, the compiler without its work folder (no game code goes in the zip, the first start builds it) or anything
rem a run left there (logs, dumps, settings), the licenses
rem and the Visual C++ runtime
set "STAGE=%OUT%\release\%GAME_EXE%"
rd /s /q "%OUT%\release" 2>nul
mkdir "%STAGE%\assets"
rem only the runtime DLLs the executables import; xbe_recomp.exe needs its own copy, Windows only looks beside the
rem exe it starts
del /q "%OUT%\compiler\*.dll" 2>nul
for %%f in (msvcp140 vcruntime140) do copy /y "%VCRUNTIME%\%%f.dll" "%OUT%\compiler\" >nul
robocopy "%OUT%\compiler" "%STAGE%\compiler" /E /XD work logs CrashDumps shader_cache /XF *.md *.log *.dmp settings.toml /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (echo Could not copy the compiler folder. & goto :done)
for %%f in ("%OUT%\%GAME_EXE%.exe" "%OUT%\MOH3RDC.exe" "%SDK%\THIRD_PARTY.txt" "%VCRUNTIME%\msvcp140.dll" "%VCRUNTIME%\vcruntime140.dll" "%VCRUNTIME%\vcruntime140_1.dll") do (
    if exist %%f (copy /y %%f "%STAGE%\" >nul) else (echo Missing: %%~f)
)
rem no Markdown in the zip; the licenses of the code taken from MOHRS-XBERecomp go as text, as MIT asks
copy /y THIRD_PARTY_NOTICES.md "%STAGE%\THIRD_PARTY_NOTICES.txt" >nul
rem 7-Zip when it is installed, else the tar that comes with Windows 10 and 11
set "SEVENZIP="
where 7z >nul 2>nul && set "SEVENZIP=7z"
if not defined SEVENZIP if exist "%ProgramFiles%\7-Zip\7z.exe" set "SEVENZIP=%ProgramFiles%\7-Zip\7z.exe"
if defined SEVENZIP (
    rem from inside the folder above it: 7-Zip keeps a relative path whole, out\build\release\release\... included
    pushd "%OUT%\release"
    "%SEVENZIP%" a -tzip "%~dp0%GAME_EXE%.zip" "%GAME_EXE%" >nul
    popd
) else (
    tar -a -c -f "%GAME_EXE%.zip" -C "%OUT%\release" "%GAME_EXE%"
)
if exist "%GAME_EXE%.zip" (echo Wrote %~dp0%GAME_EXE%.zip) else (echo The zip was not written.)
:done
rem a double clicked script keeps its window open to show the result
echo %cmdcmdline% | find /i "%~0" >nul && pause
