# oESeries - Implementation

**[Home](readme.md)** --
**[Design](design.md)** --
**[Protocol](protocol.md)** --
**Implementation** --
**[Build](build.md)**

An overview of the code that realizes the [Design](design.md) and the
[Protocol](protocol.md). The plugin is a single Win32/x86 DLL built from a handful of
source files in `src/`.

## Source layout

- **`oeSeries_pi.h` / `.cpp`** - the plugin class, preferences, and the main-thread sync engine
- **`oeSeries_log.h` / `.cpp`** - the leveled logger (own timestamped log file)
- **`oeSeries_http.h` / `.cpp`** - the off-main-thread HTTP worker

## The plugin class - `oeSeries_pi`

`oESeriesPi` derives from `opencpn_plugin_120`. The **base class must match the API version
the plugin reports** (`GetAPIVersionMajor/Minor` return 1.20 from `config.h`): OpenCPN's
loader does `dynamic_cast<opencpn_plugin_120*>` on a 1.20 plugin, so a mismatched base
yields NULL and the host logs "Incompatible plugin detected". The `create_pi`/`destroy_pi`
factories, the version/name/description getters, and `GetPlugInBitmap` are the required
overrides. `Init()` returns `WANTS_PREFERENCES | WANTS_CONFIG`, opens the log, starts the
HTTP worker and the heartbeat timer; `DeInit()` stops the timer, joins the worker, saves
config, and closes the log.

## Preferences

`ShowPreferencesDialog` builds a small dialog with two controls - a **navMate host:port**
field and a **Debug level** spinner (0..9) - shown from Options -> Plugins. Values persist
to `opencpn.ini` under `[PlugIns/oESeries]` (`HostPort`, `DebugLevel`) and reload on
`Init()`. The debug level is mirrored to a file-global `g_oeseries_debug_level` so the
logger and worker can read it without a plugin pointer.

## The leveled logger - `oeSeries_log`

`oeLog(level, indent, printf-fmt, ...)` writes a timestamped, indented line to the plugin's
own `oESeries.log` (in OpenCPN's private data dir) only when `level <= g_oeseries_debug_level`.
It flushes each write and is `wxMutex`-guarded so the main thread and the worker can both
call it. It uses `vsnprintf` (plain C printf semantics) to avoid wxString `%s` pitfalls;
callers pass C types. `wxLogMessage` is still used for milestones, which land in
`opencpn.log`.

## The HTTP worker - `oeSeries_http`

`HttpWorker` is a single persistent `wxThread` (joinable) with a mutex + condition
**mailbox**: the main thread `Submit()`s one request and, on a later heartbeat, polls
`TryGetResult()` - no `wxEvtHandler` plumbing, and single-flight by construction. The
request is an HTTP/1.1 GET or POST performed with `wxSocketClient(wxSOCKET_BLOCK)` (blocking,
so no GUI event loop is needed off-thread) and an explicit ~3 s connect/IO timeout - the
short timeout the design requires so an unreachable host cannot stall the loop. `wxSocket`
was chosen over `wxWebRequest` because `wxUSE_SOCKETS` is reliably present in OpenCPN's
wxWidgets build.

## The sync engine - the heartbeat

`oeTimer` (a `wxTimer` subclass, so the plugin needn't be a `wxEvtHandler`) fires
`OnTimer()` on the main thread every ~2 s. Each tick:

1. **`EnumerateAndBuild()`** - reads OpenCPN's live waypoints (`GetWaypointGUIDArray` +
   `GetSingleWaypointEx`), sorts them by GUID, and in one pass computes an FNV-1a hash over a
   canonical `guid|name|lat|lon|desc|icon|visible` and builds the JSON inventory. Sorting
   first makes the hash order-independent, so it only changes on real content change. On a
   change it advances `DT_ocpn` (a strictly-increasing epoch-seconds token) and rebuilds the
   POST payload.
2. **Consume any completed HTTP result**, parse the `{ok, navmate_dt, ocpn_dt}` view, and
   decide: if `ocpn_dt != DT_ocpn`, the hub is behind and the plugin should POST.
3. **Issue the next request** if the worker is idle - a POST of the inventory when a resend
   is due, otherwise a GET. One request per tick, single-flight. A failed poll (hub down) is
   retried quietly next tick.

## What is built vs. specified

- **Built and proven end-to-end** (against a live navMate): preferences, the logger, the
  off-thread HTTP worker, and the marks-inventory + two-DT loop - enumerate, hash, advance
  `DT_ocpn`, POST on change, quiesce on match, and re-sync when the user edits a waypoint.
- **Specified but not yet built** (see [Protocol](protocol.md) sec 13): route and track
  enumeration, the hub->OpenCPN push/apply direction (field-level merge-on-apply), the
  active-track event-append, and the v1 wire-field change (add `created_ts`, drop `visible`).

**Next:** The [**Build**](build.md) instructions ...
