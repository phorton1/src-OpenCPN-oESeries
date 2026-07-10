# oESeries - Build

**[Home](readme.md)** --
**[Getting Started](getting_started.md)** --
**[Design](design.md)** --
**[Protocol](protocol.md)** --
**[Implementation](implementation.md)** --
**Build** --
**[Releases](releases.md)**

How to build the oESeries plugin and install it for development testing. The build uses the
standard OpenCPN plugin CMake framework (in `cmake/`), so the same `cmake --build` also
produces a distributable tarball + `metadata.xml` and supports the Linux/flatpak targets.

## Target

- Plugin API/ABI: **api-20** (bench host OpenCPN **5.12.4**).
- Architecture: **Win32 / x86** - OpenCPN on Windows is 32-bit, and the plugin must match.

## Toolchain

- **Visual Studio 2022** Community - provides the compiler (`cl` 19.4x, target x86),
  toolset **v143** (the "vc14x" family), the Windows SDK, and a bundled **CMake** (3.29.x)
  and **Ninja**. No separate CMake install; it is put on `PATH` per-shell by the build
  batch. The v14x C++ ABI is stable across VS2015-2022, so v143 is frozen - no VS update is
  required.

## Dependencies (sibling folders)

The plugin expects two sibling folders next to it:

- **`../wxWidgets`** - wxWidgets **3.2.10**, vc14x, Win32/x86 (headers + Dev import libs).
  This is `WXWIN`. Any 3.2.x is ABI-compatible with the host; the plugin links the import
  libs and uses OpenCPN's own wx DLLs at runtime, so the Release wx DLLs are not needed.
- **`../opencpn_libs`** - the OpenCPN plugin API + support libraries (a fork/clone of
  [opencpn-libs](https://github.com/OpenCPN/opencpn-libs)). oESeries points its CMake at
  `../opencpn_libs/api-20` (which defines the `ocpn::api` target and, on Windows, links
  `api-20/msvc-wx32/opencpn.lib`). oESeries carries **no** `opencpn-libs` submodule of its
  own - it is authored clean and wired to this sibling via `OCPN_LIBS_DIR` in
  `CMakeLists.txt`.

## Building

A build shell is primed by **`ocpn_env.bat`** (it runs `vcvarsall.bat x86`, puts the bundled
CMake/Ninja on `PATH`, and sets `WXWIN` / `wxWidgets_ROOT_DIR` / `wxWidgets_LIB_DIR`).
Nothing it sets is global or persistent - it primes the current shell only. It lives in the
repo root; run it from there, then configure and build:

```
call .\ocpn_env.bat
cmake -A Win32 -G "Visual Studio 17 2022" -B build
cmake --build build --config Release
```

The two-command sequence is required: the first configure defines the targets and returns
early; building it drives the framework's `tarball` target, which does the full configure,
compiles the plugin, and emits `build\Release\oESeries_pi.dll` along with a distributable
`.tar.gz` and `metadata.xml`. Only benign warnings are expected (a missing-WindowsHeaders
OpenGL note; jsoncpp's "unknown pragma"; "missing components: lexilla").

A convenience `build.bat` in the repo root runs exactly this. It is self-locating (`%~dp0`), so
it primes the shell through `ocpn_env.bat` and runs both CMake steps regardless of where it is
invoked from.

## Dev install and host-load

OpenCPN loads custom (unmanaged) plugins from the **user** plugin directory, on Windows
`%LOCALAPPDATA%\opencpn\plugins\` (user-writable, no admin; create the `plugins` sub-folder
by hand). The program's own `plugins\` directory accepts only OpenCPN's five built-ins and
will silently skip anything else. Copy the built DLL there and restart OpenCPN:

```
copy build\Release\oESeries_pi.dll %LOCALAPPDATA%\opencpn\plugins\
```

Three notes that are easy to trip over when manually installing a plugin into OpenCPN
5.12.4:

- **The plugin's base class must match its reported API version.** oESeries reports API 1.20
  and derives from `opencpn_plugin_120`; a mismatch makes the loader's `dynamic_cast` fail
  and it logs "Incompatible plugin detected".
- **A failed load leaves a quarantine stamp.** Before loading a candidate OpenCPN writes a
  zero-byte stamp `C:\ProgramData\opencpn\load_stamps\<pluginname>`; it is removed only on a
  clean load. If a load fails, that stamp survives and the next launch refuses the plugin
  ("failed at last attempt") even after you replace the DLL. Delete the stamp to recover.
- **A brand-new unmanaged plugin needs a config entry to activate.** OpenCPN 5.12.4 loads a
  never-seen plugin but does not surface or run it until `opencpn.ini` has a
  `[PlugIns/<pluginname>.dll]` section with `bEnabled=1`. Add it (with OpenCPN closed), or
  enable the plugin once it appears in Options -> Plugins.

A successful load logs `oESeries: Init ...` (and, once syncing, `oESeries: sync ok ...`) in
`C:\ProgramData\opencpn\opencpn.log`; verbose detail goes to `oESeries.log` in OpenCPN's
private data dir (raise the Debug level in the plugin's preferences to see it).

## Environment layout

oESeries lives at `C:\src\OpenCPN\oESeries`. `ocpn_env.bat` now lives in the repo root (run it
from there), so the plugin is self-contained for building apart from two sibling trees one
level up, both referenced by ABSOLUTE path: `wxWidgets` (via `WXWIN` / `wxWidgets_ROOT_DIR` /
`wxWidgets_LIB_DIR` in `ocpn_env.bat`) and `opencpn_libs` (via `OCPN_LIBS_DIR` in
`CMakeLists.txt`, default `..\opencpn_libs`).

**Next:** Back to the [**Home**](readme.md) page.
