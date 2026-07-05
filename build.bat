@echo off
rem ============================================================================
rem  build.bat - configure + build oESeries Release/Win32.
rem  Self-locating (uses %~dp0), so it runs from anywhere; primes the shell via
rem  the in-repo ocpn_env.bat, then does the two-step CMake configure + build.
rem  Output: build\Release\oESeries_pi.dll  (+ tarball / metadata.xml).
rem ============================================================================
cd /d "%~dp0"
call "%~dp0ocpn_env.bat"
echo === CMAKE CONFIGURE ===
cmake -A Win32 -G "Visual Studio 17 2022" -B build
if errorlevel 1 (echo CONFIGURE_FAILED & exit /b 1)
echo === CMAKE BUILD ===
cmake --build build --config Release
if errorlevel 1 (echo BUILD_FAILED & exit /b 1)
echo === BUILD_OK ===
