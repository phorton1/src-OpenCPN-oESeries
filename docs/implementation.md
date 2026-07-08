# oESeries - Implementation

**[Home](readme.md)** --
**[Design](design.md)** --
**[Protocol](protocol.md)** --
**Implementation** --
**[Build](build.md)** --
**[Releases](releases.md)**

An overview of the code that realizes the [Design](design.md) and the
[Protocol](protocol.md). The plugin is a single Win32/x86 DLL built from a handful of
source files in `src/`.

## Source layout

- **`oeSeries_pi.h` / `.cpp`** - the plugin class, preferences, the main-thread sync engine
  (marks/routes/tracks enumeration, hub-command apply, results), and the symbol/icon channel
- **`oeSeries_log.h` / `.cpp`** - the leveled logger (own timestamped log file)
- **`oeSeries_http.h` / `.cpp`** - the off-main-thread HTTP worker
- **`oe_sha256.h`** - the small self-contained SHA-256 used for the icon `byte_hash`
- **`json.hpp`** - the vendored nlohmann/json single header used for the wire payloads

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

1. **`EnumerateAndBuild()`** - reads OpenCPN's live **marks, routes, and tracks**
   (`GetWaypointGUIDArray` + `GetSingleWaypointExV2`; `GetRouteGUIDArray`/`GetTrackGUIDArray`
   with their reverse converters), sorts by GUID, and in one pass computes an FNV-1a hash over
   a canonical form of every carried field and builds the JSON inventory. Sorting first makes
   the hash order-independent, so it only changes on real content change. On a change it
   advances `DT_ocpn` (a strictly-increasing epoch-seconds token) and rebuilds the POST payload.
2. **Consume any completed HTTP result**: parse the hub's GET view, apply its `commands[]`
   (mark/route/track add/update/delete) to OpenCPN's live model as a field-level
   merge-on-apply, and stage the per-command `results[]` and `{op:diag}` readback for the next
   POST. Compare the two-DT tokens by equality to decide whether OpenCPN owes the hub a POST.
3. **Issue the next request** if the worker is idle - a POST (inventory + results, and the
   foreign-icon vocabulary when the hub asks via `want_icons`) when a resend is due, otherwise
   a GET. One request per tick, single-flight. A failed poll (hub down) is retried quietly next
   tick.

## What is built

The full plugin is built and proven end-to-end against a live navMate:

- **Preferences, the leveled logger, and the off-thread HTTP worker.**
- **Bidirectional marks / routes / tracks sync** over the two-DT loop - enumerate, hash,
  advance `DT_ocpn`, POST on change, quiesce on match, and re-sync on a user edit - with
  hub->OpenCPN pushes applied as a field-level merge that preserves OpenCPN-only data, GUIDs
  preserved verbatim in both directions, and idempotent add/update/delete.
- **The symbol / icon channel**: the live foreign-icon vocabulary reported up with per-icon
  `byte_hash` and a `builtin` flag, stock markicons and user icons rasterized to 48x48 PNG at
  emit, and navMate's `nm:` library registered down via `AddCustomWaypointIcon`.
- **Self-versioning wire** (`protocol_version` stamped both directions) and the unmanaged
  tarball install path (OpenCPN **Import plugin**).

**Next:** The [**Build**](build.md) instructions ...
