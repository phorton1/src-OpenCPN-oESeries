# OpenCPN Plugin ABI Limitations and WRT Round-Trip Survey

**Purpose.** A grounded reference for what the OpenCPN plugin ABI does and does not let a
plugin (oESeries) do with Waypoints, Routes, and Tracks (WRTs), so navMate's "be the home of
the WRTs with transparent round-tripping" goal can be planned against reality instead of hope.
Every claim below is cited to real source or real captured runtime behavior.

**Environment this was derived from.**
- Installed OpenCPN under test: **5.12.4** (`opencpn.exe` FileVersion 5.12.4) = plugin **API 1.20**
  (`opencpn_libs/api-20/ocpn_plugin.h`, `API_VERSION_MINOR 20`).
- Core source read for behavior: an OpenCPN checkout at **API 1.21**
  (`C:\_temp\base-apps-navMate\opencpn`), and **GitHub `OpenCPN/OpenCPN` master**, which is also
  **API 1.21**. 1.21 is the newest that exists; there is no later ABI to wait for.
- Source citations `file:line` refer to the 1.21 checkout unless noted. Where the installed
  5.12.4 binary behaves differently from that source, it is called out (see `scamax`).

---

## 1. Architectural limitations (why the losses below exist)

These are the design choices in the ABI that make the field-level losses in section 4 inevitable.

1. **Value-copy DTO structs, not handles.** The ABI hands the plugin a flat snapshot struct
   (`PlugIn_Track`, `PlugIn_Waypoint_ExV2`, ...). There is no opaque object handle and no
   get/set-by-property. Any field the struct does not declare is both unreadable and unsettable,
   and the core model can (and does) carry fields the struct omits.

2. **No patch / partial-update semantics; "update" is destructive.** There is no "change just
   this field of this GUID" call. The only metadata-edit path is `Update*`, and internally:
   - `UpdatePlugInTrack` = `DeleteTrack` + `AddPlugInTrack` (`gui/src/ocpn_plugin_gui.cpp:1239`).
   - `UpdatePlugInRouteExV2` = `DeleteRoute` + `AddPlugInRouteExV2` (`...:2219`).
   So an update **rebuilds the whole object from the (lossy) struct**, resetting every field the
   struct cannot carry back to defaults -- even though the call has the GUID in hand and could
   have preserved them. This is the "silently clobbers user data on edit" behavior.
   - Trigger is precise: fires on `update`, or `add` of an already-existing GUID (upsert). A fresh
     `add` (nothing pre-existed) and a `delete` are lossless. So routine, delta-only syncing that
     never re-asserts unchanged objects does not clobber; only a genuine in-place edit (or a
     blanket "re-add everything" resync) does.

3. **No runtime navobj reload.** The object DB (`navobj.db`) is read exactly once, at startup
   (`NavObj_dB::LoadNavObjects()`, called from `gui/src/ocpn_frame.cpp:4708`). Nothing re-reads it
   during a session and it is not exported to plugins. The one reload-ish call,
   `ConfigFlushAndReload()` (`...:3115`), reloads only `opencpn.ini` settings (general/S57/canvas),
   never routes/tracks/waypoints. Consequence: editing `navobj.db` directly is not a channel --
   a running OpenCPN will not read your change and will overwrite it from its in-memory model on
   its next save. A direct DB edit only "takes" if OpenCPN is shut down, edited, and relaunched
   (offline batch, Windows-same-machine only -- dead on the Linux/remote path).

4. **The ABI is a hard ceiling you cannot extend from a plugin.** The `DECL_EXP` functions are
   implemented and exported by `opencpn.exe` itself (they reach core-internal globals like
   `pRouteMan`/`pWayPointMan` the plugin cannot touch). Adding a new method (e.g.
   `SetTrackColour`) means rebuilding the OpenCPN executable; a plugin built against those exports
   only loads against that custom build, not stock installs. The only path that reaches users in
   the wild is upstreaming the change into an official OpenCPN release (then runtime-gating on
   `API_VERSION_MINOR`). In-process symbol/pattern-scanning to reach non-exported internals is
   technically possible (shared address space) but per-build/per-platform fragile and not a
   distribution strategy.

---

## 2. Concrete ABI bug hit this session (fixed plugin-side)

**`~PlugIn_Track()` dereferences `pWaypointList` with no null guard**
(`gui/src/ocpn_plugin_gui.cpp:793`), while its sibling `~PlugIn_Route_ExV2()` **is** null-guarded
(`...:1909`). The idiomatic "null the list to avoid a double-free" pattern (safe for routes) makes
the track destructor crash OpenCPN on stack unwind -- after the track has already been committed to
the DB. This took OpenCPN down on the first route+track push. Fixed in the plugin by letting the
core destructor own the list container (free the point data ourselves, do not null or delete the
list). Documents that the ABI leaks C++ ownership rules onto the plugin and gets them inconsistent
between sibling types.

---

## 3. The three tiers used for the survey

- **Tier 1 -- user's notion of "my data":** the Route & Mark Manager / Properties dialogs. What a
  user can set and expects to persist.
- **Tier 2 -- what OpenCPN actually persists:** the `navobj.db` schema (below).
- **Tier 3 -- what the plugin ABI can touch:** the `PlugIn_*` structs + their forward
  (`Add*`/`Update*`) and reverse (`Get*_Plugin`) converters.

**Loss = (Tier 1 intersect Tier 2) that is not round-trippable through Tier 3.**

navobj.db tables (Tier 2): `routes`, `routepoints`, `routepoints_link`, `tracks`, `trk_points`,
and per-type `*_html_links` (routepoint/route/track). The presence of `route_html_links`,
`track_html_links`, and `routepoint_html_links` confirms OpenCPN persists hyperlinks ("links") for
all three object types.

---

## 4. Field reachability survey

Legend: OK = round-trips; NO = walled off by the ABI (not in the struct); DEFER = ABI-capable but
not yet implemented in the plugin; VER = version-dependent.

### 4a. Waypoints / Marks -- mostly round-trippable (the good case)

ABI struct `PlugIn_Waypoint_ExV2` is rich; reverse `PlugInExV2FromRoutePoint`
(`gui/src/ocpn_plugin_gui.cpp:1917`) reads nearly everything; forward `CreateNewPoint` (`...:2000`)
and `UpdateSingleWaypointExV2` (`...:2073`) write nearly everything.

| Field (dialog / DB) | Read | Write | Round-trips |
|---|---|---|---|
| position, name, description, icon/symbol | yes | yes | OK |
| visibility, show-name | yes | yes | OK |
| scamin + use-scale | yes | yes | OK |
| arrival radius, planned speed, ETD, tide station | yes | yes | OK |
| range rings {number, step, units, colour, show} | yes | yes | OK |
| hyperlinks / "links" | yes | yes (`UpdateSingleWaypointExV2:2095-2104` clones the list) | DEFER (plugin `ApplyMarkFields` has "hyperlinks apply deferred"; implementable, NOT a wall) |
| scamax | yes | source calls `SetScaMax` (`...:2145`) | VER: NO on installed 5.12.4 (verified: plugin passed 5000, model read back 0); source-level 1.21 should apply it (unverified against a running 1.21 binary) |
| font color, name-label offset, blink | no | no | not in navobj.db -> not durable data anyway |

**Bottom line:** waypoints -- the "exceptionally complicated" type -- are the *safest*. Every
persisted, user-editable field round-trips, INCLUDING links (once we implement the deferred apply).
The only hard ABI loss is `scamax`, and only on the current binary.

### 4b. Routes -- lose styling, timing, and route-level links

ABI struct `PlugIn_Route_ExV2` = {name, from(start), to(end), GUID, active(read), visible,
description, points}. Reverse `GetRouteExV2_Plugin` (`...:2236`) fills only those.

| Field (dialog / DB) | In ABI struct | Round-trips |
|---|---|---|
| name, from, to, description, visibility, active(read) | yes | OK |
| vertices (+ per-vertex GUIDs; per-vertex speed/arrival/etc.) | yes | OK |
| color, style, width | no | NO |
| planned_departure, time_format | no | NO |
| shared_wp_viz | no | NO |
| route-level hyperlinks (`route_html_links`) | no (no list member) | NO |
| route-level planned_speed / arrival radius | no (route level) | reachable per-vertex only |

### 4c. Tracks -- gutted (the worst case)

ABI struct `PlugIn_Track` = {name, from(start), to(end), GUID, points}. It has never had an "Ex"
variant; it is frozen at five fields. Reverse `GetTrack_Plugin` (`...:1562`) fills only those.

| Field (dialog / DB) | In ABI struct | Round-trips |
|---|---|---|
| name, from, to | yes | OK |
| points (lat/lon/time) | yes | OK |
| description | no | NO |
| visibility | no | NO |
| color, style, width | no | NO |
| hyperlinks (`track_html_links`) | no | NO |

---

## 5. The "links" question, specifically

Hyperlinks ("links") exist in the model and DB for all three types
(`routepoint_html_links`, `route_html_links`, `track_html_links`).

- **Waypoint links: reachable.** `PlugIn_Waypoint_ExV2` has `m_HyperlinkList`; core clones it on
  both add (`cloneHyperlinkListExV2`) and update (`UpdateSingleWaypointExV2:2095-2104`). The plugin
  simply defers applying them today -- a plugin TODO, not an ABI wall.
- **Route and Track links: walls.** Neither `PlugIn_Route_ExV2` nor `PlugIn_Track` declares a
  hyperlink list, so route/track links cannot be read or written by any plugin. Unreachable.

---

## 6. Implications for "navMate as the home of the WRTs"

- **Delete:** non-issue (lossless) for all three types.
- **Waypoints:** the transparent full-copy round-trip goal *is* achievable -- every persisted,
  user-editable field round-trips once the deferred hyperlink apply is implemented; only `scamax`
  (version-dependent) is a residual caveat. navMate's richest type is its safest.
- **Routes:** round-trippable as *navigation* objects (identity, geometry, names, visibility) but
  **not as styled/timed** objects. Pushing a full copy resets color/style/width, planned departure,
  time format, shared-WP visibility, and drops route-level links.
- **Tracks:** navMate can own identity, endpoint labels, and shape, but **description, visibility,
  styling, and links cannot survive a push** and reset on every full-copy write.

**Containment (all ABI-free, mostly navMate/hub policy):**
1. **Delta-only sync -- never re-assert an unchanged track/route.** This alone prevents routine
   sync from ever rebuilding (and thus clobbering) a styled object; the destructive path then only
   fires on a genuine user edit.
2. **Do not propagate in-place track metadata edits** (tracks are historical; skipping track
   upserts eliminates the biggest single loss set -- description+visibility+color+style+width+links
   -- at near-zero functional cost).
3. **Never silent.** Where a real edit must rebuild a route/track, disclose it (release note /
   user-facing warning) rather than silently resetting styling.

The transparent-round-trip promise holds for waypoints, is partial for routes, and mostly fails for
tracks -- exactly along the line of which structs OpenCPN chose to enrich.

---

## 7. Open items to verify before relying on this

1. **Tier-1 waypoint dialog field list** is grounded in the navobj.db schema + `RoutePoint` model
   members, not cross-checked control-by-control against `MarkInfoDlg`. Confirm no user-editable,
   persisted waypoint field is missing from section 4a.
2. **`scamax` on a running 1.21 core.** Source-level, 1.21's `UpdateSingleWaypointExV2` calls
   `SetScaMax` (`:2145`), so it *should* round-trip there; this is unverified against an actual
   running 1.21 binary. On installed 5.12.4 it is confirmed broken (empirical trace: in 5000, out 0).
3. **Whether any 1.21 change makes `Update*` non-destructive** -- not observed; the delete+re-add
   structure is unchanged in the 1.21 source and on master.

---

## 8. Key source references

- Struct defs: `opencpn_libs/api-20/ocpn_plugin.h` -- `PlugIn_Waypoint_ExV2` (~5332),
  `PlugIn_Track` (~2139), `PlugIn_Route_ExV2` (~5484). Identical member sets on 1.21 / master.
- Converters / behavior (`gui/src/ocpn_plugin_gui.cpp`): `PlugInExV2FromRoutePoint` 1917,
  `GetSingleWaypointExV2` 1967, `CreateNewPoint` 2000, `UpdateSingleWaypointExV2` 2073 (SetScaMax
  2145; hyperlink clone 2095), `AddPlugInTrack` 1180, `UpdatePlugInTrack` 1239 (=delete+add),
  `~PlugIn_Track` 793 (unguarded), `AddPlugInRouteExV2` 2160, `UpdatePlugInRouteExV2` 2219
  (=delete+add), `~PlugIn_Route_ExV2` 1909 (guarded), `GetRouteExV2_Plugin` 2236,
  `GetTrack_Plugin` 1562, `ConfigFlushAndReload` 3115.
- Startup-only navobj load: `gui/src/ocpn_frame.cpp:4708` (`LoadNavObjects`).
- DB schema (Tier 2): `model/src/navobj_db.cpp` (and the live `navobj.db` schema dump).
