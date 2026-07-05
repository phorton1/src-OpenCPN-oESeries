# oESeries - Sync Protocol (spec v1)

**[Home](readme.md)** --
**[Design](design.md)** --
**Protocol** --
**[Implementation](implementation.md)** --
**[Build](build.md)**

This is the canonical wire contract between oESeries and the hub. It was co-designed by the
OpenCPN-plugin side and the navMate-hub side; the turn-by-turn derivation is preserved
(unlinked) in [notes/protocol_dialog.md](notes/protocol_dialog.md). Plugin-side claims are
grounded in the api-20 OpenCPN plugin API. Sections were tagged **[hub-review]** where
drafted from the design dialogue and pending hub-side verification. That pass ran 2026-07-04
against the navMate code: verified sections are now marked **[hub-verified 2026-07-04]**, and
claims the hub has not yet implemented are called out inline as **DEFERRED (hub)** (see the
consolidated list in sec 13). The concrete wire objects (exact JSON per type) were
co-designed and locked 2026-07-04 in sec 2A.

## 1. Roles + transport (the floor, proven)

The plugin is a polling HTTP CLIENT; the hub is the SERVER (one endpoint, `/api/ocpn`). A
main-thread `wxTimer` heartbeat is the only clock; the plugin always initiates, the hub
always responds. HTTP runs off the main thread with a short connect timeout; there is no
persistent connection - each poll is a fresh, short-lived request, and a hub that is down is
simply retried on the next heartbeat.

## 2. The wire - one endpoint, one round-trip, both directions

- **GET `/api/ocpn`** -> `{ ok, navmate_dt, ocpn_dt, commands:[...] }`. `commands` is the
  hub->OpenCPN batch; it is non-empty exactly when `navmate_dt` has advanced.
- **POST `/api/ocpn`** body `{ dt, marks:[...], routes:[...], tracks:[...], results:[...] }`
  -> returns the same view shape. `dt` = the plugin's current `DT_ocpn`; the inventory
  arrays are the OpenCPN->hub mirror; `results` acks the last command batch (sec 10).
  **Hub status 2026-07-04:** the hub v0 parses only `{ dt, waypoints:[...] }`; it reconciles
  to this full body (`marks`/`routes`/`tracks`/`results`) in Phase 1 of its OCPN-spoke
  buildout. Build to this spec - the hub is catching up to it.
- Debug readback: **GET `/api/ocpn?dump=1`** adds `{ recv_count, last_recv, payload }`.

## 2A. Wire objects -- the exact JSON  [locked 2026-07-04]

Co-designed and locked in [notes/json_and_test_oe.md](notes/json_and_test_oe.md) (Turns 1-5).
This is the concrete byte-level contract that sec 2's envelopes carry. navMate serializes with
`JSON::PP`, the plugin with nlohmann -- both emit standard JSON with correct UTF-8 and escaping.

### Serialization rules (no field is ever omitted or null)

- **Arrays** (`marks`, `routes`, `tracks`, `results`, `commands`, route `points`) are ALWAYS
  present; `[]` when empty. Never null, never omitted. An empty inventory still POSTs (proves
  transport).
- **Strings** (`name`, `description`, `icon`, `error`) are ALWAYS present; `""` when empty,
  never null. (navMate's store is NOT-NULL with `''` sentinel; a JSON null would mismatch.)
- **Integers** -- `dt`, `ocpn_dt`, `navmate_dt`, `created_ts`, `ts` are JSON integers (UTC
  epoch seconds), never strings. `created_ts`/`ts` always present; `0` = unknown (a vertex with
  no real stamp emits `0`, which navMate stamps at ingest).
- **Numbers** -- `lat`, `lon` are full-precision JSON numbers (the model's `double` verbatim).
  NO wire rounding -- the plugin's `%.6f` change-hash is deliberately lossy; the wire must not
  be, or coordinates drift every round-trip.
- **Bool** -- `ok` is a JSON bool, everywhere.
- **`position`** is a 0-based, contiguous, gap-free integer index within a route's `points[]`;
  it maps directly to navMate `route_waypoints.position`.

### Envelopes

```json
// GET /api/ocpn  (poll view)
{ "ok": true, "navmate_dt": 0, "ocpn_dt": 1719950400, "commands": [ <command>, ... ] }

// POST /api/ocpn  (body)
{ "dt": 1719950400,
  "marks":   [ <mark>,   ... ],
  "routes":  [ <route>,  ... ],
  "tracks":  [ <track>,  ... ],
  "results": [ <result>, ... ] }
```

`navmate_dt` stays `0` until the hub's db_version counter lands (sec 13). Steady state = both
DTs match = heartbeat only (sec 3).

### mark

```json
{
  "guid":        "<128-bit OpenCPN GUID>",
  "name":        "<m_MarkName>",
  "lat":         9.1234567,
  "lon":         -82.1234567,
  "description": "<m_MarkDescription>",   // navMate 'comment'
  "icon":        "<IconName>",            // raw; navMate owns sym<->icon internally (sec 7)
  "created_ts":  1719950400               // m_CreateTime; 0 if unset
}
```

Hub-only ExtendedData (`wp_type, color, depth_cm, temp_k, ts_source, source, collection_uuid,
position, modified_ts`) is kept hub-side by guid, NOT on the wire (sec 6). `visible` deferred.

### route

Ordered members; identity is the member `guid`, order is `position`. A member that is a
free-standing mark (`GetFSStatus()==true`) rides as a bare `{guid, position}` ref -- its full
object is in the same POST's `marks[]`. A pure vertex (`GetFSStatus()==false`, in no `marks[]`)
additionally embeds a full `mark` object (fields exactly as above).

```json
{
  "guid":        "<route GUID>",
  "name":        "<m_NameString>",
  "description": "<m_Description>",
  "points": [
    { "guid": "<mark guid>",   "position": 0 },
    { "guid": "<vertex guid>", "position": 1, "mark": { ...mark fields... } }
  ]
}
```

**Direction rule (asymmetric, same shape):**
- **Outbound inventory (plugin -> hub):** refs + embedded vertices as above. Leaner wire, 1:1
  with `route_waypoints`.
- **Inbound command (hub -> plugin):** EVERY point embeds its full `mark` (no bare refs).
  `AddPlugInRouteExV2` builds a route by constructing new RoutePoints per vertex -- there is no
  api-20 call to reference an existing mark by guid (sec 8) -- so the plugin needs each point's
  full data. Only "is a bare ref allowed" differs by direction.

### track

```json
{
  "guid":  "<track GUID>",
  "name":  "<track name>",
  "points": [ { "lat": 9.1, "lon": -82.1, "ts": 1719950400 } ]
}
```

Flat, ordered, no segment boundaries (OpenCPN's `GetTrack_Plugin` flattens internal TrackSegs;
sec 11). `depth_cm`/`temp_k` are hub-only, never sent by the plugin.

### command / result

```json
// command  (hub -> plugin, in commands[])
{ "op": "add|update|delete", "type": "mark|route|track", "guid": "<GUID>", "fields": { ... } }
//   fields = full mapped set for 'add', changed-only for 'update', absent for 'delete'.
//   for type route|track, fields carries the route/track object above (route = full-embed).

// result  (plugin -> hub, in results[])
{ "guid": "<GUID>", "op": "add|update|delete|diag", "ok": true,
  "error": "<msg if !ok, else \"\">", "data": { ... } }
//   'data' present only for diag ops (below).
```

### diag commands (harness observability, sec Goal B / json_and_test_oe.md)

The plugin answers diagnostic commands via `results[].data`, giving the harness a probe into
otherwise-invisible plugin state:

```json
{ "op":"diag", "type":"inventory" }           // data: {dt_ocpn, hash, marks, vertices, routes, tracks, layer_seen}
{ "op":"diag", "type":"object", "guid":"X" }  // data: live view -- mark->mark, route->route (FULL-embed
                                              //   every point), track->SUMMARY {guid,name,n_points,
                                              //   first_ts,last_ts}; unknown -> {found:false}
{ "op":"diag", "type":"state" }               // data: {reachable, synced, want_post, last_applied_batch, echo_baseline}
```

(A `type:"track_points"` op can be added if a full track-point assertion is ever needed.)

### The echo-round-trip invariant (hub-side)

Applying a hub command mutates the plugin model, changes the plugin's hash, advances
`DT_ocpn`, so the applied objects reappear in the plugin's NEXT inventory POST. **That
reappearance IS the round-trip confirmation** -- not an echo bug. The hub MUST treat the
reappearance of an object it just commanded as confirmation, not a new user edit, or the two
sides ping-pong. navMate holds this by construction: ingesting an inbound inventory writes only
the in-memory ocdb (a spoke projection), never canonical navMate.db, so `navmate_dt` never
advances on an echo and no new command is minted. `results[]` is the independent per-command
ack, consumed before the reconcile.

### Shared risks carried from the lock -- all three BENCH-MEASURED 2026-07-05

Measured in the Mode-2 alpha (`notes/build_and_test_oe.md`), real plugin vs live hub. These are
no longer open risks; the results are recorded here.

- **R1 (layer leakage, plugin-side) -- MEASURED: LEAKS (transient).** api-20 layer exclusion
  (`OBJECTS_NO_LAYERS`) IS a stub in 5.12.4 (the `// FIXME (dave)`). Bench-proven by staging a
  read-only GPX layer: `GetWaypointGUIDArray` returns layer marks AND layer-route vertices,
  `GetRouteGUIDArray` returns the layer route -- they leak into `marks[]`/`routes[]` and reach the
  hub (which minted `0x4f` for the foreign layer guids). The leak is TRANSIENT (layer objects are
  read-only overlays, not in navobj.db; they vanish from the sync the moment the layer is unloaded,
  proven by a full-state-replace re-sync dropping them). Mitigations: **plugin-side** -- try the
  layer-aware `GetRouteGUIDArray/GetTrackGUIDArray(OBJECT_LAYER_REQ)` overloads + a per-mark layer
  check, and make `diag inventory.layer_seen` real (currently a stub). **Hub-side** -- the wire
  cannot mark a layer object, so the hub rightly tolerates unexpected inbound and reads
  `results[].ok` (a push back to a read-only layer object no-ops). Non-corrupting.
- **R2 (write-side vertex GUID) -- MEASURED: PASS.** `AddPlugInRouteExV2` **preserves the caller's
  per-vertex `m_GUID` verbatim** (bench-proven: pushed a route with 3 navMate-origin vertex guids;
  the plugin's `diag object` readback via `GetRouteExV2_Plugin` returned the exact same 3 guids, and
  the hub minted 0 new provenance bytes -- reversed table-free). **Routes round-trip by vertex
  IDENTITY, not just position/order.** (Same holds for `AddSingleWaypointExV2` marks, sec 4, and
  `AddPlugInTrack` tracks -- write-side guid preservation confirmed for all three object types.)
- **R3 (string encoding) -- MEASURED: PASS (codepoint-equality).** Names/descriptions/icons carry
  arbitrary UTF-8. Both sides emit strict-ASCII JSON (non-ASCII as `\uXXXX`, astral as surrogate
  pairs -- navMate `JSON::PP` ascii, plugin nlohmann `ensure_ascii=true`), so the wire is pure ASCII
  both directions (no HTTP charset ambiguity). The "nasty strings" fixture (quote, backslash, tab,
  newline, forward slash, CJK U+6D77, astral emoji U+1F6A3, combining U+0301, a 0x01 control)
  round-trips **codepoint-equal** both ways. Compare strings by codepoint after parse, numbers
  numerically -- never byte-compare (shortest-repr doubles and escaping differ, values don't).

## 3. The two-DT version gate

Each side keeps `{DT_ocpn, DT_navmate}`. `DT_ocpn` is minted ONLY by the plugin (a
strictly-increasing epoch-seconds token, bumped past the last value); `DT_navmate` ONLY by
the hub. Each is compared by EQUALITY against a cached copy of itself (single-minter ->
clock-skew immune; a mismatch alone names the direction). Steady state = both match =
heartbeat only. Plugin loop: GET the view; if `ocpn_dt != my DT_ocpn`, POST the inventory;
if `navmate_dt` advanced, apply `commands` and POST `results`. The HASH (content-derived,
over the sorted per-object key fields) is the local change DETECTOR; the DT is the wire
TOKEN. `navmate_dt` is hardcoded 0 today (the generation counter is deferred, sec 13).

## 4. Identity - the GUID is the unit  [hub-verified 2026-07-04]

The seam's unit of identity is the object GUID; a shared point is ONE object, referenced
(not copied) by routes. Confirmed both sides: navMate's `route_waypoints` is a pure ref
table `(route_uuid, wp_uuid, position)`. `Add/UpdateSingleWaypointEx` adopt the caller's
128-bit GUID VERBATIM (grounded: `if (!pGUID.IsEmpty()) m_GUID = pGUID`). navMate maps its
64-bit uuid <-> the 128-bit OpenCPN GUID: navMate-origin objects carry a synthesized
RFC-4122-v4-shaped GUID embedding the 8 uuid bytes + a "navMate" MAGIC tag (recognized
locally, no table); foreign OpenCPN-born GUIDs get a `ocpn_guid_map(ocpn_guid, nav_uuid,
first_seen)` row + a `0x4f` OCPN provenance byte (vs `0x4e` navMate, `0x46` FSH). Dispatch
on the provenance byte both ways; the map makes re-enumeration idempotent.

**Hub verification:** `route_waypoints` is confirmed the exact `(route_uuid, wp_uuid,
position)` ref table (PK is `(route_uuid, position)`, so one `wp_uuid` may legally recur in a
route). The navMate-origin GUID codec is real and reversible table-free (MAGIC = ascii
"navMate") - it currently lives in the GPX spoke module and will be promoted to shared hub
code. `guid` is SYNTHESIZED from the 64-bit uuid, not a stored column. Provenance bytes
`0x4e` (navMate) and `0x46` (FSH) are minted today. **DEFERRED (hub):** the `0x4f` OCPN mint
byte and the `ocpn_guid_map(ocpn_guid, nav_uuid, first_seen)` table are not built yet - they
land in the hub's schema 13.1 migration (foreign-GUID persistence).

## 5. Object model + enumeration

OpenCPN gives NO edit event for marks/routes (only for track points, sec 11), so the plugin
DISCOVERS marks/routes by ENUMERATION each heartbeat.

- **Marks** = free-standing waypoints. `GetWaypointGUIDArray()` returns EVERY RoutePoint
  (marks AND route vertices - they share one list). Classify each by
  `PlugIn_Waypoint_Ex::GetFSStatus()`: `true` -> a mark (incl. a "shared" mark reused in a
  route); `false` -> a pure route vertex (delivered only inside its route). This split is
  what removed the ~84 double-counted vertices seen in the first live inventory.
- **Routes** = `GetRouteGUIDArray()` + `GetRoute_Plugin(guid)` -> `PlugIn_Route_Ex`
  (metadata + ordered point list).
- **Tracks** = `GetTrackGUIDArray()` + `GetTrack_Plugin(guid)` -> flat ordered point list.

## 6. Field model - GUID-anchored shadows

Principle: **the GUID carries the linkage; each side's store carries the data.** Because the
GUID round-trips reliably, neither side ships the other's non-mappable fields - each keeps a
shadow keyed by GUID and re-attaches on return. Three classes:

- **Carry both ways** (mapped wire fields, per mark): `guid`, `name` <-> `m_MarkName`,
  `lat`, `lon`, `description` <-> `m_MarkDescription` (navMate `comment`), `icon` <->
  `IconName` (via the sym table, sec 7), `created_ts` <-> `m_CreateTime` (UTC).
- **hub-only ExtendedData** [hub-verified 2026-07-04]: `wp_type` (derived from sym), `color`,
  `depth_cm`, `temp_k`, `ts_source`, `source`, `collection_uuid`, `position`, `modified_ts` -
  dropped from the wire, kept hub-side by GUID, merged onto the plugin's edits on return.
  *Hub: all nine are confirmed columns on the `waypoints` record; the only net-new shadow
  field is a raw `icon_name` for foreign icons with no sym (sec 7).*
- **OpenCPN-only fields**: `scamin/scamax`, range rings, `TideStation`, `PlannedSpeed`,
  `WaypointArrivalRadius`, `HyperlinkList`, and the raw `IconName` - NOT shadowed by the hub.
  They survive round-trips via plugin merge-on-apply (sec 8), which uses LIVE values, so
  there is no staleness to shadow.
- **`visible`**: DEFERRED. OpenCPN `IsVisible` is a stored per-point bool, but navMate models
  visibility as a display-layer overlay; a field mapping fights both. Not carried in v1.

`collection_uuid`/hierarchy has no OpenCPN home: OpenCPN-origin marks land in a default
bucket; the user re-files in the hub.

## 7. Symbols - a two-hop map, the plugin sees one hop  [hub-verified 2026-07-04]

- Hop A (hub-internal, plugin never touches): `wp_type` (enum) <-> `sym` (0..35).
- Hop B (the seam): `sym` <-> OpenCPN `IconName`. The hub delivers this as a separate
  artifact: a 36-entry `sym -> icon` table chosen from OpenCPN's 43 default icon names, plus
  `icon -> sym` with a catch-all sym for unrecognized names.

OpenCPN's default icon vocabulary (43, from `ProcessDefaultIcons`): `empty, triangle,
activepoint, boarding, airplane, anchorage, anchor, boundary, buoy1, buoy2, campfire,
camping, coral, fishhaven, fishing, fish, float, food, greenlite, kelp, light, light1,
litevessel, mob, mooring, oilbuoy, platform, redgreenlite, redlite, rock1, rock2, sand,
scuba, shoal, snag, square, diamond, circle, wreck1, wreck2, xmblue, xmgreen, xmred`. This
is NOT a closed set - users add/override icons - so `sym <-> icon` is not a bijection.
**The raw OpenCPN `IconName` is preserved** (merge-on-apply for the plugin; the hub stores
it per GUID for its own re-symbolization) so an OpenCPN-born mark round-trips its exact icon
even when the hub has no sym for it. Round-trip fidelity beats classification.

**Hub verification:** Hop A exists (`wp_type` enum <-> `sym` via the `%WP_DEFAULT_SYMS` map);
usable sym space is `0..35` as stated (syms `36..39` exist only as unused legacy).
**DEFERRED (hub):** Hop B's 36-entry `sym -> icon` table is not built yet (sec 13). Note the
`wp_type`-from-`sym` reverse used in sec 6 is not a clean inverse of the one-directional
default map; the hub will pin that down when it builds Hop B.

## 8. Push mechanics - field-level + merge-on-apply

`UpdateSingleWaypointEx/ExV2` OVERWRITE the entire field set from the passed struct (and
`m_HyperlinkList->clear()` + repopulate); there is no partial-update API. So a full-object
push built from a shadow would silently REVERT any OpenCPN-only field the shadow missed.
Instead the push is **field-level** and the plugin applies as read-modify-write:
`GetSingleWaypointExV2(guid)` -> overlay ONLY the fields the hub changed ->
`UpdateSingleWaypointExV2`. OpenCPN-only fields survive (never overwritten), using live
values -> zero staleness, and the hub need not shadow them. Apply runs on the MAIN thread
(model access); the worker fetches/returns off-thread.

**Idempotency (the transport retries until the hub sees the result):**
- `add` of an existing GUID -> treated as `update` (upsert), `ok:true`.
- `update` of a vanished GUID -> `ok:false` + error; the hub re-drives as a full `add`.
- `delete` of an absent GUID -> `ok:true` ("ensure absent"), never an error.
- Route apply is idempotent too: check `GetRouteGUIDArray` for the route GUID -> update/skip
  if present, never re-Add (re-Add duplicates every point).

**Routes own their points on push.** There is no API to build a route that REFERENCES an
existing mark by GUID (`AddPlugInRoute` does `new RoutePoint(...)` per vertex). So each hub
waypoint manifests in OpenCPN exactly ONCE - as a standalone mark XOR as a vertex inside its
route, never both. The rare true-shared case (one point in multiple routes) is a hub-side
manifestation policy, WIRE-INVISIBLE to the plugin. **[hub-verified 2026-07-04: model
confirmed / policy DEFERRED]** navMate's model CAN express a shared point (`route_waypoints`
+ `getRoutesForWaypoint`), but the mark-vs-vertex manifestation POLICY is not built yet - it
arrives with the hub's outbound push path (Phase 2). Wire-invisible either way.

## 9. Gate model  [hub-verified 2026-07-04]

navMate's navOps runs silent, side-effect-free predicate gates as preflight BEFORE any
mutation is queued. A hub->OpenCPN push originates as a user PASTE into the hub's OpenCPN
spoke; if a gate rejects, the hub shows the USER an error and NOTHING is queued. **The
plugin never receives a gate rejection - it only ever GETs already-approved,
already-transformed commands.** The OpenCPN spoke is the gentlest: upsert-by-GUID removes the
collision gate; unbounded OpenCPN strings remove the truncation gate; the surviving
structural-legality gates are hub-internal and invisible. The plugin's ONLY failure channel
is apply-results (sec 10).

**Hub verification:** the silent, side-effect-free predicate gate layer is confirmed (shared
by the menu builders and the navOps preflight; `emit_as` user_error/impl_error). The
per-spoke absence of the collision gate (upsert) and the truncation gate (unbounded strings)
is architecturally correct. **DEFERRED (hub):** the OpenCPN spoke as a PASTE target is not
wired yet - paste/push destinations today are `e80`/`fsh`/`database`; this whole
push-origination path is Phase 2. Until then this section is a verified design prediction,
not an exercised path.

## 10. Command + result shapes

- **Command** (hub -> plugin, in `commands[]`): `{ op: add|update|delete, type:
  mark|route|track, guid, fields:{...} }`. `fields` = full mapped set for `add`, changed-only
  for `update`, absent for `delete`. For `type: route|track`, `fields.points[]` is the
  ORDERED member list - a route point is a full (route-owned) mark object; a track point is
  `{ lat, lon, ts }`.
- **Result** (plugin -> hub, in `results[]`): `{ guid, op, ok, error? }` per applied command.
  Deletes map 1:1 to `DeleteSingleWaypoint` / `DeletePlugInRoute` / `DeletePlugInTrack`.

## 11. Tracks

One object (metadata + points). `track_points` is FLAT ordered on both sides
(`position,lat,lon,ts` [+ hub-only `depth_cm,temp_k`, kept by GUID]) - OpenCPN exposes no
segment boundaries, so segments are a non-issue between the two (they only ever mattered vs
GPX). Per OpenCPN point = `{lat, lon, ts}` (UTC, seconds); no per-point name/icon.

- **Active (recording) track = event-append.** OpenCPN fires `OCPN_TRK_POINT_ADDED`
  (`{lat, lon, Track_ID}`, requires `WANTS_PLUGIN_MESSAGING`) per point laid down -> the
  plugin streams incrementally (one point per event, no re-scan). Caveat: OpenCPN may PRUNE
  a just-added point (collinear reduction), so the event stream slightly over-reports; a
  periodic `GetTrack_Plugin` reconcile trues it up. The hub ingests whatever point set is
  posted. Which track is "active" = the `g_pActiveTrack`; its GUID rides the event payload.
- **Old/completed tracks** are immutable -> enumerate-once, like marks.

## 12. Delta / efficiency

Full-state is the floor (proven). The efficiency ceiling falls out with no new design:
field-level push IS the hub->OpenCPN delta; the plugin's hash+DT gate IS the OpenCPN->hub
delta (its hash expands to cover exactly the fields carried two-way, so a change to any
carried field re-syncs).

## 13. Open / deferred

- **`sym <-> icon` table** - the hub to deliver (sec 7). Until then the plugin passes `icon`
  strings through unmapped.
- **Generation token** - for safe deltas across a hub restart (in-memory spoke): a generation
  that changes on reset lets the plugin detect "hub lost state" and full-resync (compare-and-
  swap on `{generation, dt}`). Deferred; today's crude signal is `ocpn_dt` resetting to 0.
- **`visible`** wiring (sec 6).
- **Plugin wire fields today** send `{guid,name,lat,lon,description,icon,visible}`; the v1
  target set is `{guid,name,lat,lon,description,icon,created_ts}` (add `created_ts`, drop
  `visible`) plus `routes[]`/`tracks[]` - a plugin-side to-do.
- **Hub-side deferred (2026-07-04 review)** - the hub has NOT yet built: the
  `0x4f`/`ocpn_guid_map` foreign-GUID persistence (sec 4), Hop B's `sym -> icon` table
  (sec 7), the mark-vs-vertex manifestation policy (sec 8), and the entire OpenCPN
  paste-target / `commands[]` outbound path (sec 2/9). The hub's **Phase 1** is INBOUND only
  (plugin POST -> hub parse -> user paste into navMate.db); **Phase 2** is outbound.
  `navmate_dt` stays `0` until the hub's `db_version` generation counter lands (Phase 2).

**Next:** The [**Implementation**](implementation.md) overview ...
