@echo off
rem ============================================================================
rem  ocpn_env.bat - OpenCPN plugin build environment (Win32 / x86)
rem
rem  Lives in the oESeries repo root. Run it from that folder before configuring:
rem      call .\ocpn_env.bat     (explicit .\ - this machine does not search cwd for bare names)
rem
rem  Sets up, for THIS shell only (nothing global, nothing persistent):
rem    - MSVC v143 x86 toolchain            (VS2022 Community)
rem    - CMake + Ninja bundled with VS2022  (on the session PATH)
rem    - wxWidgets 3.2.10 location          (WXWIN / wxWidgets_ROOT_DIR / _LIB_DIR)
rem
rem  Does NOT change directory - leaves you in the repo root. Then:
rem      cmake -A Win32 -G "Visual Studio 17 2022" -B build
rem      cmake --build build --config Release
rem ============================================================================

rem --- MSVC v143, 32-bit target (vcvarsall does NOT change the current dir) ---
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86

rem --- CMake + Ninja bundled with VS2022 Community (session PATH only) ---
set "VS_CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VS_CMAKE%\CMake\bin;%VS_CMAKE%\Ninja;%PATH%"

rem --- wxWidgets 3.2.10 prebuilt (vc14x, Win32) ---
set "WXWIN=C:\src\OpenCPN\wxWidgets"
set "wxWidgets_ROOT_DIR=C:\src\OpenCPN\wxWidgets"
set "wxWidgets_LIB_DIR=C:\src\OpenCPN\wxWidgets\lib\vc14x_dll"

echo.
echo   OpenCPN plugin build env ready:  x86 / MSVC v143 / wx 3.2.10
echo   cwd = %CD%
echo   next: cmake -A Win32 -G "Visual Studio 17 2022" -B build
echo.