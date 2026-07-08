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

## 3A. The navobj.db schema (Tier 2 in full)

**Why this section exists.** Reading `navobj.db` directly is *outside the plugin ABI*. The ABI is a
compiled contract (versioned by `API_VERSION_MINOR`, dynamic-cast-checked at load); the DB schema is
not a contract to plugins at all -- it is an OpenCPN-internal implementation detail that no API
promises to keep stable. Anything built on it must treat the layout below as an **observed shape at a
known version**, not a guarantee, and validate before trusting it. This section documents the shape,
how OpenCPN builds it, and exactly where the drift risk lives.

**Provenance.** DDL below is verbatim from `model/src/navobj_db.cpp` `CreateTables()` (API 1.21
checkout) and was byte-confirmed against a live `.schema` dump of the installed **5.12.4** (API 1.20)
`C:\ProgramData\opencpn\navobj.db`. The two agree on every table and column. (The one difference is
that 1.21's `CreateTables` also builds the `idx_track_points` index, which the older 5.12.4 DB lacks
-- see 3A.3 for why that self-heals when a newer binary opens the DB.)

### 3A.1 The eight tables (verbatim DDL)

```sql
CREATE TABLE IF NOT EXISTS tracks (
    guid TEXT PRIMARY KEY NOT NULL,
    name TEXT, description TEXT, visibility INTEGER,
    start_string TEXT, end_string TEXT,
    width INTEGER, style INTEGER, color TEXT,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS trk_points (
    track_guid TEXT NOT NULL,
    latitude REAL NOT NULL, longitude REAL NOT NULL,
    timestamp TEXT NOT NULL, point_order INTEGER,
    FOREIGN KEY (track_guid) REFERENCES tracks(guid) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS track_html_links (
    guid TEXT PRIMARY KEY, track_guid TEXT NOT NULL,
    html_link TEXT, html_description TEXT, html_type TEXT,
    FOREIGN KEY (track_guid) REFERENCES tracks(guid) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS routes (
    guid TEXT PRIMARY KEY NOT NULL,
    name TEXT, start_string TEXT, end_string TEXT, description TEXT,
    planned_departure TEXT, plan_speed REAL, time_format TEXT,
    style INTEGER, width INTEGER, color TEXT,
    visibility INTEGER, shared_wp_viz INTEGER,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS routepoints (
    guid TEXT PRIMARY KEY NOT NULL,
    lat REAL, lon REAL,
    Symbol TEXT, Name TEXT, description TEXT, TideStation TEXT,
    plan_speed REAL, etd INTEGER, Type TEXT, Time TEXT,
    ArrivalRadius REAL,
    RangeRingsNumber INTEGER, RangeRingsStep REAL, RangeRingsStepUnits INTEGER,
    RangeRingsVisible INTEGER, RangeRingsColour TEXT,
    ScaleMin INTEGER, ScaleMax INTEGER, UseScale INTEGER,
    visibility INTEGER, viz_name INTEGER, shared INTEGER, isolated INTEGER,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS routepoints_link (
    route_guid TEXT, point_guid TEXT, point_order INTEGER,
    PRIMARY KEY (route_guid, point_order),
    FOREIGN KEY (route_guid) REFERENCES routes(guid) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS route_html_links (
    guid TEXT PRIMARY KEY, route_guid TEXT NOT NULL,
    html_link TEXT, html_description TEXT, html_type TEXT,
    FOREIGN KEY (route_guid) REFERENCES routes(guid) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS routepoint_html_links (
    guid TEXT PRIMARY KEY, routepoint_guid TEXT NOT NULL,
    html_link TEXT, html_description TEXT, html_type TEXT,
    FOREIGN KEY (routepoint_guid) REFERENCES routepoints(guid) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_track_points ON trk_points (track_guid);
```

### 3A.2 What each table holds

- **`routes`** -- one row per route. Carries the route-level styling/timing fields the ABI struct
  drops (`style`, `width`, `color`, `planned_departure`, `plan_speed`, `time_format`,
  `shared_wp_viz`) -- i.e. the section 4b "NO" list lives here, which is *why* those losses are real
  data and not just dialog chrome.
- **`routepoints`** -- one row per point, holding **both standalone marks and route vertices in a
  single table** (same as `GetWaypointGUIDArray`'s flat list). `shared` and `isolated` disambiguate:
  an isolated point is a lone mark; a shared point belongs to >=1 route. Note the ABI-visible rich
  fields (`Symbol`, `ArrivalRadius`, the five `RangeRings*`, `ScaleMin`/`ScaleMax`/`UseScale`, `etd`,
  `TideStation`) -- this is the "waypoints are the safe type" evidence from section 4a.
- **`routepoints_link`** -- the route<->point membership + ordering join. `(route_guid, point_order)`
  is the PK; a point can appear in many routes (shared) so `point_guid` is deliberately not unique.
  A route's geometry = `routepoints_link` ordered by `point_order`, joined to `routepoints`.
- **`tracks`** / **`trk_points`** -- track header (with the `description`/`visibility`/`color`/
  `style`/`width` that section 4c reports as walled off) and its ordered fix list
  (`latitude`,`longitude`,`timestamp`,`point_order`).
- **`*_html_links`** (route / routepoint / track) -- the hyperlink lists. Their existence is the
  proof-of-persistence cited in sections 3 and 5. Empty in the current live DB (0 rows each) but the
  tables are always created.

### 3A.3 How OpenCPN builds and versions it (the stability model)

OpenCPN has **two** update mechanisms, both run at every startup, and together they **do** migrate
existing DBs -- so the schema is versioned and maintained, not silently changed and abandoned:

1. **Additive changes self-heal via `CreateTables()`.** `CreateTables()` runs on **every** DB open,
   not just first creation (`navobj_db.cpp:602`, comment "Add any new tables"), and every statement is
   `CREATE TABLE/INDEX IF NOT EXISTS`. So a **new table or new index** added in a later release is
   created on an existing DB the next time a newer binary opens it. (This is exactly why the
   `idx_track_points` gap noted above is benign: a 1.21 binary opening the 5.12.4 DB would create it.)
   What `IF NOT EXISTS` **cannot** do is alter an existing table -- adding a column or changing a key
   needs mechanism 2.
2. **Structural changes go through a versioned migration ladder.** At startup (`ocpn_frame.cpp:4706`,
   right beside `LoadNavObjects`) OpenCPN calls `NavObj_dB::FullSchemaMigrate()` (`navobj_db.cpp:620`),
   which walks an ordered list of migrations (`needsMigration_0_1` -> `SchemaUpdate_0_1` ->
   `setUserVersion`, via a `DbMigrator` class with an explicit "More schema updates in sequence here"
   extension point). The shipped **0->1** migration is a real table rebuild: it recreates
   `routepoints_link` with a new primary key (`(route_guid, point_order)`), de-duplicates rows, drops
   the old table, and renames -- the standard SQLite "you can't `ALTER` a PK" idiom
   (`navobj_db_util.cpp:105`). So column/key/shape changes on existing DBs are explicitly supported.
3. **`PRAGMA user_version` is the schema version -- but can lag on older binaries.** The ladder stamps
   `user_version` to at least 1 (`setUserVersion`, `navobj_db_util.cpp:49`). **Caveat observed on the
   live DB:** the installed 5.12.4 reads `user_version = 0` *even though* its `routepoints_link`
   already has the v1 `(route_guid, point_order)` key -- because the migrator infrastructure post-dates
   5.12.4 (it is in the 1.21 checkout, not the 1.20 binary that wrote this DB). So the version stamp is
   authoritative on migrator-aware builds but can under-report on older ones; corroborate it with
   structure, do not trust it alone. (Legacy XML `navobj.xml` is a separate one-time import,
   `ImportLegacyNavobj()` `navobj_db.cpp:647`, not part of this ladder.)

### 3A.4 Drift risks -- where "what we expect" can diverge from reality

The schema is therefore **not frozen**: OpenCPN actively versions and migrates it, so it is a moving
target *across releases*. Reading it outside the ABI, these are the ways the on-disk shape can differ
from the snapshot above, ordered by how likely they are to bite:

- **A migration can rewrite shape out from under a cached assumption.** The 0->1 migration (3A.3)
  changed `routepoints_link`'s primary key and de-duplicated its rows. A reader that hard-codes the
  pre-migration shape -- or caches column positions across an OpenCPN upgrade -- would be wrong once
  the user updates OpenCPN. The shape on the bench can differ from the version we documented against.
- **New columns / tables in future releases.** A later release can add a column (through a new
  migration) or a whole new table (through `CreateTables`). `SELECT *` would silently reorder/extend;
  a new table we do not know about is invisible to us and to any "full copy" claim.
- **The version stamp can lag (see 3A.3.3).** `user_version` is reliable on migrator-aware builds but
  reads 0 on the pre-migrator 5.12.4 DB despite its v1 shapes. Read it, but do not treat it as the
  sole source of truth -- corroborate with structure.
- **Column-name casing is inconsistent and hand-maintained.** Within `routepoints` alone: `Symbol`,
  `Name`, `TideStation`, `Type`, `Time` are capitalized while `lat`, `lon`, `description`,
  `visibility` are not. SQLite matches column names case-insensitively so queries work regardless,
  but the inconsistency signals a hand-edited schema with no naming discipline -- exactly the kind of
  surface that gets "cleaned up" (renamed) in a refactor.
- **Declared types are affinities, not constraints.** SQLite is dynamically typed. `etd INTEGER`
  but ABI `etd` is a string; booleans (`visibility`, `shared`, `isolated`, `viz_name`, `UseScale`,
  `RangeRingsVisible`) are stored as INTEGER 0/1; GUIDs and timestamps are TEXT. Do not assume a
  column's storage class from its declared type; read defensively.
- **Read consistency is not free.** Journal mode is rollback (`delete`), **not** WAL -- there is no
  `-wal`/`-shm`, only a transient `-journal` during a write. An external reader gets a SHARED lock
  and briefly blocks during a COMMIT; set your own `busy_timeout`/retry. `PRAGMA foreign_keys=ON`
  with `ON DELETE CASCADE` means *any* direct write (which we do not do) must honor FK order; for a
  pure reader it only matters that deletes cascade children.

### 3A.5 Defensive rules for anything that reads this DB

Given a schema that is versioned and migrated (not frozen), the regression harness (the only
sanctioned reader -- see the scoping note in section 6 / project memory) should:

1. **Read `PRAGMA user_version` AND validate structure -- belt and suspenders.** Use the version
   stamp as the primary signal, but because it can lag on older binaries (3A.3.3), also enumerate
   `PRAGMA table_info(<table>)` for each table we touch and confirm the columns we read exist. If
   neither matches what we expect, **fail loud** ("navobj.db schema vN / shape mismatch: expected
   column X on table Y") rather than silently mis-read.
2. **Never `SELECT *`.** Always name the exact columns; that makes an added/reordered column a no-op
   and a removed/renamed column a clean, localized failure.
3. **Tolerate benign version skew.** A newer-but-additive DB (an extra table/column/index such as
   `idx_track_points`) should degrade gracefully; only a change to a column we actually read is an
   error.
4. **Do not write, ever.** The scoping decision (regression-harness-only, read as a verification
   *witness*) already forbids it; and a running OpenCPN would overwrite external DB edits from its
   in-memory model on its next save (section 1.3), so direct writes are non-durable regardless.
5. **Keep the DB read a *second* witness, not the source of truth.** The canonical, cross-platform,
   version-stable verification channel remains the plugin's own `{op:diag}` protocol over HTTP. The
   DB read exists only because it is a cheaper, independent cross-check available in the
   Windows/same-machine harness -- if the schema drifts and DB reads break, the diag channel is
   unaffected and the product is unaffected (the plugin never touches this DB).

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
- DB schema (Tier 2): `model/src/navobj_db.cpp` `CreateTables()` (and the live `navobj.db` schema
  dump) -- reproduced verbatim, with the versioning/drift analysis, in section 3A.
