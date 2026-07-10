# oESeries - Sync Protocol (spec 1.0)

**[Home](readme.md)** --
**[Getting Started](getting_started.md)** --
**[Design](design.md)** --
**Protocol** --
**[Implementation](implementation.md)** --
**[Build](build.md)** --
**[Releases](releases.md)**

This is the canonical wire contract between oESeries and the hub, co-designed by the OpenCPN-plugin
side and the navMate-hub side. Plugin-side claims are grounded in the api-20 OpenCPN plugin API.
**This spec is `1.0`.**

## 1. Roles + transport

The plugin is a polling HTTP CLIENT; the hub is the SERVER (one endpoint, `/api/ocpn`). A
main-thread `wxTimer` heartbeat is the only clock; the plugin always initiates, the hub
always responds. HTTP runs off the main thread with a short connect timeout; there is no
persistent connection - each poll is a fresh, short-lived request, and a hub that is down is
simply retried on the next heartbeat.

## 2. The wire - one endpoint, one round-trip, both directions

- **GET `/api/ocpn`** -> `{ ok, navmate_dt, ocpn_dt, icon_hash, want_icons, lib_gen,
  commands:[...] }`. `commands` is the hub->OpenCPN batch; it is non-empty exactly when
  `navmate_dt` has advanced. `icon_hash`/`want_icons`/`lib_gen` drive the symbol channel (sec 7).
  The symbol fields are OPTIONAL additive envelope fields; a peer MUST tolerate their absence
  (missing `want_icons` = false, missing `lib_gen` = 0), distinct from the per-object no-omit rule
  of sec 2A.
- **POST `/api/ocpn`** body `{ dt, marks:[...], routes:[...], tracks:[...], results:[...],
  icon_hash, ocpn_icons:[...] }` -> returns the same view shape. `dt` = the plugin's current
  `DT_ocpn`; the inventory arrays are the OpenCPN->hub mirror; `results` acks the last command
  batch (sec 10); `icon_hash`/`ocpn_icons` are the OpenCPN->hub symbol report (sec 7).
- **GET `/api/ocpn?icons=1`** -> `{ ok, lib_gen, nm_icons:[...] }`: the plugin PULLS navMate's
  authored symbol library (sec 7, Direction B). Mirrors `?dump=1` - a side fetch, off the
  steady-state loop.
- Debug readback: **GET `/api/ocpn?dump=1`** adds `{ recv_count, last_recv, payload }`.

## 2A. Wire objects -- the exact JSON

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
// GET /api/ocpn  (poll view) -- hub self-announces protocol_version (sec Versioning)
{ "protocol_version": "1.0",
  "ok": true, "navmate_dt": 0, "ocpn_dt": 1719950400,
  "icon_hash": "<hex>", "want_icons": false, "lib_gen": 0,
  "commands": [ <command>, ... ] }

// POST /api/ocpn  (body) -- plugin self-announces protocol_version (sec Versioning)
{ "protocol_version": "1.0",
  "dt": 1719950400,
  "marks":   [ <mark>,   ... ],
  "routes":  [ <route>,  ... ],
  "tracks":  [ <track>,  ... ],
  "results": [ <result>, ... ],
  "icon_hash":  "<hex>",           // OpenCPN foreign-icon change trip-wire (sec 7)
  "ocpn_icons": [ <icon>, ... ] }  // [] unless the last GET set want_icons:true

// GET /api/ocpn?icons=1  (Direction B pull -- navMate's authored library)
{ "ok": true, "lib_gen": 3, "nm_icons": [ <icon>, ... ] }
```

### Versioning

The wire is **self-versioning**: each peer stamps its own `protocol_version` (a `MAJOR.MINOR`
string) in the envelope it sends -- the plugin in every POST, the hub in the GET poll view --
so either side can read what the other speaks. **This spec is `1.0`.**

- **MINOR** bump = upward-compatible, additive change (a new OPTIONAL field); a newer peer
  talking to an older one degrades gracefully.
- **MAJOR** bump = breaking change (a peer below the floor cannot safely interop).
- **Ignore unknown fields** -- a field a peer does not recognize is skipped, never fatal; this
  is what makes MINOR additions safe.
- **Absent `protocol_version` == `1.0`** (the baseline): a peer that predates the field is read
  as 1.0, so introducing it breaks nothing.
- **Soft floor** -- each side keeps a minimum it needs; below it, warn plainly ("peer speaks
  vX, I need vY") but keep functioning where the overlap allows. Never go dark on a skew.

`navmate_dt` is minted by the hub's outbound command path (bumped when a command batch is queued);
`lib_gen` is the hub's monotonic symbol-library token (sec 7). Steady state = both DTs match,
`icon_hash` unchanged, `lib_gen` unchanged = heartbeat only (sec 3).

### mark

A-fields (mappable) + the full OpenCPN-only **B** superset (sec 6). Every B-field maps 1:1 to a
`PlugIn_Waypoint_ExV2` member; the plugin always populates them from the live struct (no-omit rule)
and round-trips them via merge-on-apply (sec 8).

```json
{
  "guid":        "<128-bit OpenCPN GUID>",
  "name":        "<m_MarkName>",
  "lat":         9.1234567,
  "lon":         -82.1234567,
  "description": "<m_MarkDescription>",   // navMate 'comment'
  "icon":        "<IconName>",            // raw icon name (sec 7)
  "created_ts":  1719950400,              // m_CreateTime; 0 if unset

  // ---- B: OpenCPN-only, spoke-carried, winOCPN-edited (sec 6) ----
  "visible":        true,     // IsVisible -- OpenCPN's own show/hide (NOT navMate map-visibility)
  "name_shown":     false,    // IsNameVisible
  "active":         false,    // IsActive [R: up-only nav state; never pushed down]
  "scamin":         50000.0,  // scamin
  "scamin_on":      false,    // b_useScamin
  "scamax":         0.0,      // scamax
  "arrival_radius": 0.0,      // m_WaypointArrivalRadius
  "planned_speed":  0.0,      // m_PlannedSpeed (per-vertex)
  "etd":            0,        // m_ETD epoch secs; 0 = unset (wxInvalidDateTime)
  "tide_station":   "",       // m_TideStation
  "range_rings": { "count":0, "space":1.0, "units":0, "color":"#FF0000", "show":false },
                              //   units 0:nm 1:km ; color "#RRGGBB" (the ONE color on the wire)
  "hyperlinks": [ { "desc":"", "link":"", "type":"" } ]   // m_HyperlinkList (Plugin_Hyperlink)
}
```

There is NO waypoint color field in api-20 -- mark color IS the icon (sec 7). Hub-only ExtendedData
(`wp_type, color, depth_cm, temp_k, ts_source, source, collection_uuid, position, modified_ts`) is
kept hub-side by guid, NOT on the wire (sec 6).

### route

Ordered members; identity is the member `guid`, order is `position`. A member that is a
free-standing mark (`GetFSStatus()==true`) rides as a bare `{guid, position}` ref -- its full
object is in the same POST's `marks[]`. A pure vertex (`GetFSStatus()==false`, in no `marks[]`)
additionally embeds a full `mark` object (fields exactly as above).

```json
{
  "guid":        "<route GUID>",
  "name":        "<m_NameString>",
  "from":        "<m_StartString>",   // B
  "to":          "<m_EndString>",     // B
  "description": "<m_Description>",
  "visible":     true,                // B: m_isVisible (routes CAN show/hide -- unlike tracks)
  "active":      false,               // B [R]: m_isActive
  "points": [
    { "guid": "<mark guid>",   "position": 0 },
    { "guid": "<vertex guid>", "position": 1, "mark": { ...mark fields... } }
  ]
}
```

GAP (api-20, omitted from the wire): no route color / line style / route-level planned-speed
(per-leg speed is per-vertex, on each `mark.planned_speed`). Those stay hub-only (sec 6).

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
  "name":  "<m_NameString>",
  "from":  "<m_StartString>",   // B
  "to":    "<m_EndString>",     // B
  "points": [ { "lat": 9.1, "lon": -82.1, "ts": 1719950400 } ]
}
```

Flat, ordered, no segment boundaries (OpenCPN's `GetTrack_Plugin` flattens internal TrackSegs;
sec 11). Editable in place -- rename / `from`,`to` / points -- via `UpdatePlugInTrack` (sec 8/11).
GAP (api-20, omitted): no track `visible` / color / style (and, unlike routes, tracks expose NO
visibility member at all). `depth_cm`/`temp_k` are hub-only, never sent by the plugin.

### icon (the symbol channel)

Same shape both directions; only the key field differs -- `name` for an OpenCPN-origin icon (up),
`key` for a navMate `nm:` icon (down):

```json
// ocpn_icons[] entry (up, Direction A) -- keyed by OpenCPN icon NAME
{ "name":"anchor",  "description":"Anchor", "fmt":"png", "data_b64":"<base64 48x48 RGBA PNG>", "byte_hash":"<hex>", "builtin":true }

// nm_icons[] entry (down, Direction B) -- keyed by navMate nm: KEY
{ "key":"nm:sym00", "description":"Sym 0",  "fmt":"png", "data_b64":"<base64 PNG>", "byte_hash":"<hex>" }
```

- `fmt` = `png` (a 48x48 RGBA PNG holding the icon's CONTENT scaled to FILL the box -- aspect-preserved,
  centered, with a small breathing margin -- NOT the raw SVG canvas; see sec 7) or `none` (a names-only
  entry: empty `data_b64`/`byte_hash`). **SVG never rides the wire in either direction.**
- The plugin renders each stock/user icon SOURCE FILE large, crops to its alpha content box, and scales
  the glyph to fit the 48x48 at emit time; icons with no locatable source stay `fmt:"none"`.
- `builtin` (Direction A only) = `true` for an OpenCPN default icon, `false`/absent for a user import;
  the plugin derives it from the `UserIcons/` dir (NOT icon-array order, which is not defaults-first),
  letting navMate group built-ins in its picker.
- `data_b64` is standard base64 (ASCII) -- preserves the pure-ASCII-wire invariant (R3).
- `byte_hash` is over the exact wire bytes, so each side re-stores only changed icons.

See sec 7 for the tokens (`icon_hash`/`want_icons`/`lib_gen`), the two directions, and the
ordering gate.

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

### diag commands (harness observability)

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

### Wire invariants

- **Strings are pure-ASCII JSON both directions** (non-ASCII escaped as `\uXXXX`, astral as
  surrogate pairs -- navMate `JSON::PP` ascii, plugin nlohmann `ensure_ascii=true`), so there is no
  HTTP charset ambiguity. Arbitrary UTF-8 (quotes, control chars, CJK, astral emoji, combining
  marks) round-trips codepoint-equal. **Compare strings by CODEPOINT after parse and numbers
  NUMERICALLY -- never byte-compare** (shortest-repr doubles and escaping differ, values don't).
- **Routes round-trip by vertex IDENTITY, not just order.** `AddPlugInRouteExV2` preserves the
  caller's per-vertex `m_GUID` verbatim (as do `AddSingleWaypointExV2` for marks and `AddPlugInTrack`
  for tracks); write-side GUID preservation holds for all three object types.
- **Layer objects are wire-invisible as such.** OpenCPN's api-20 layer exclusion
  (`OBJECTS_NO_LAYERS`) is a stub in 5.12.4, so read-only layer marks/route-vertices can leak into
  `marks[]`/`routes[]`. The wire cannot mark an object as belonging to a layer, so the hub tolerates
  unexpected inbound objects and reads `results[].ok` (a push back to a read-only layer object
  no-ops). Layer objects are transient -- not in navobj.db -- and drop out of the sync when the
  layer is unloaded. Non-corrupting.

## 3. The two-DT version gate

Each side keeps `{DT_ocpn, DT_navmate}`. `DT_ocpn` is minted ONLY by the plugin (a
strictly-increasing epoch-seconds token, bumped past the last value); `DT_navmate` ONLY by
the hub. Each is compared by EQUALITY against a cached copy of itself (single-minter ->
clock-skew immune; a mismatch alone names the direction). Steady state = both match =
heartbeat only. Plugin loop: GET the view; if `ocpn_dt != my DT_ocpn`, POST the inventory;
if `navmate_dt` advanced, apply `commands` and POST `results`. The HASH (content-derived,
over the sorted per-object key fields) is the local change DETECTOR; the DT is the wire
TOKEN. `navmate_dt` is MINTED by the hub's outbound command path (bumped when a command batch is
queued -- single-minter, strictly increasing).

## 4. Identity - the GUID is the unit

The seam's unit of identity is the object GUID; a shared point is ONE object, referenced
(not copied) by routes. Confirmed both sides: navMate's `route_waypoints` is a pure ref
table `(route_uuid, wp_uuid, position)`. `Add/UpdateSingleWaypointEx` adopt the caller's
128-bit GUID VERBATIM (grounded: `if (!pGUID.IsEmpty()) m_GUID = pGUID`). navMate maps its
64-bit uuid <-> the 128-bit OpenCPN GUID: navMate-origin objects carry a synthesized
RFC-4122-v4-shaped GUID embedding the 8 uuid bytes + a "navMate" MAGIC tag (recognized
locally, no table); foreign OpenCPN-born GUIDs get a row in a hub-internal foreign-guid map
(table shape is the hub's private choice) + a `0x4f` OCPN provenance byte (vs `0x4e` navMate,
`0x46` FSH). Dispatch on the provenance byte both ways; the map makes re-enumeration idempotent.

## 5. Object model + enumeration

OpenCPN gives NO edit event for marks/routes (only for track points, sec 11), so the plugin
DISCOVERS marks/routes by ENUMERATION each heartbeat.

- **Marks** = free-standing waypoints. `GetWaypointGUIDArray()` returns EVERY RoutePoint
  (marks AND route vertices - they share one list). Classify each by
  `PlugIn_Waypoint_Ex::GetFSStatus()`: `true` -> a mark (incl. a "shared" mark reused in a
  route); `false` -> a pure route vertex (delivered only inside its route). This split prevents
  route vertices from being double-counted as marks.
- **Routes** = `GetRouteGUIDArray()` + `GetRoute_Plugin(guid)` -> `PlugIn_Route_Ex`
  (metadata + ordered point list).
- **Tracks** = `GetTrackGUIDArray()` + `GetTrack_Plugin(guid)` -> flat ordered point list.

## 6. Field model - GUID-anchored, OpenCPN-only fields spoke-carried

Principle: **the GUID carries the linkage.** Every field the api-20 struct exposes is CARRIED on
the wire, so navMate's winOCPN
editor is a faithful, familiar remote for OpenCPN's own property dialogs. Fields split by where
their CANONICAL home is:

- **A - mappable** (a navMate canonical concept; carried both ways): per mark `guid`, `name` <->
  `m_MarkName`, `lat`, `lon`, `description` <-> `m_MarkDescription` (navMate `comment`), `icon`
  <-> `IconName` (sec 7), `created_ts` <-> `m_CreateTime`. Per route `name`, `from`/`to`
  (`m_StartString`/`m_EndString`), `description`, members. Per track `name`, points.
- **B - OpenCPN-only, SPOKE-CARRIED** (no canonical navMate home): carried on the wire, held in
  the LIVE ocdb spoke projection, edited ONLY in winOCPN, NOT added to navMate.db, and DROPPED at
  a paste->navMate.db boundary. Per mark: `visible`, `name_shown`, `active`[R], `scamin`/
  `scamin_on`/`scamax`, `arrival_radius`, `planned_speed`, `etd`, `tide_station`, `range_rings{}`,
  `hyperlinks[]`. Per route: `from`/`to`, `visible`, `active`[R]. Per track: `from`/`to`. The plugin
  applies them via merge-on-apply (sec 8) so nothing clobbers, and the hub carries them WITHOUT a
  navMate.db column.
- **hub-only ExtendedData**: `wp_type` (from sym), `color`, `depth_cm`,
  `temp_k`, `ts_source`, `source`, `collection_uuid`, `position`, `modified_ts` - navMate concepts
  with no OpenCPN home; kept hub-side by GUID, never on the wire.

**Reachability ceilings (the api-20 DTO is the limit).** The plugin sees a curated value-COPY of
OpenCPN's internal object (a `PlugIn_*` marshalling struct), never the object; a core field with
no struct member is unreachable, and there is no widening it short of a new OpenCPN API version
(`PlugIn_Track` is identical in api-20 and api-21). Documented gaps, omitted from the wire: mark -
no color (mark color IS the icon, sec 7); route - no color/style/route-speed; track - no
visible/color/style (and, unlike routes, no visibility member at all). `GetFSStatus` /
`GetRouteMembershipCount` are read-only computed views, never settable.

**`visible`.** OpenCPN `IsVisible` is a stored per-point bool = "shown in OpenCPN," carried as a B
field. It is ORTHOGONAL to navMate's own display-layer map-visibility (navVisibility); winOCPN
labels the two distinctly ("Visible in OpenCPN" vs "Show on navMate map").

`collection_uuid`/hierarchy has no OpenCPN home: OpenCPN-origin marks land in a default bucket; the
user re-files in the hub.

## 7. Symbols - a bidirectional channel

OpenCPN has NO symbol-authoring UX (43 hardcoded defaults from `ProcessDefaultIcons`; file-drop
user import via `ProcessUserIcons`; a pick-from-dropdown selector; the only creation primitive is
a programmatic inject). navMate carries the richer model (`sym` 0..35, `wp_type`, `color`), so the
seam is: **navMate is the symbol AUTHORITY and library; OpenCPN is a live, re-provisionable render
target.** Symbols flow BOTH ways. This SUPERSEDES the old "navMate owes a static 36-entry
`sym -> icon` table"; the raw `IconName` is still preserved end-to-end for round-trip fidelity, and
the hub's internal `wp_type` <-> `sym` mapping (`%WP_DEFAULT_SYMS`, sym space `0..35`) is unchanged
and plugin-invisible. (Derivation: [notes/oe_symbol_protocol.md](notes/oe_symbol_protocol.md).)

### The api-20 primitives (plugin-side, grounded)
- `GetIconNameArray()` - the live icon vocabulary (defaults + user + other-plugin injects).
- `FindSystemWaypointIcon(name)` -> `wxBitmap*` - declared in api-20 but NOT exported by
  opencpn.exe 5.12.4, so a foreign icon's rendered raster cannot be fetched by name. Instead the
  UP-path rasterizes the icon's SOURCE FILE via `GetBitmapFromSVGFile` (exported), from the known
  locations `GetSharedDataDir()/uidata/markicons/` (stock SVGs) and `GetPrivateDataDir()/UserIcons/`
  (user png/xpm/svg); an icon with no locatable source file emits `fmt:"none"` (names-only).
  `GetIconNameArray` and `AddCustomWaypointIcon` ARE exported.
- `GetIcon_PlugIn(name)` -> `wxBitmap` - exported (ord 296) but returns toolbar/UI STYLE icons
  (`style->GetIcon`), NOT waypoint markicons; not a raster-by-name path. Not used.
- `AddCustomWaypointIcon(bitmap, key, description)` - the ONLY inject primitive; NO `b_permanent`
  arg -> registration is SESSION-ONLY (in-memory `WayPointman`, not persisted). No delete API.
- `GetBitmapFromSVGFile(file, w, h)` - rasterize a stock/user SVG FILE to a bitmap; used on the
  UP-path (emit) to produce the 48x48 PNG. (Down-path PNGs are decoded via `wxImage`, not this.)

### Direction A - OpenCPN vocabulary UP (plugin PUSH, content HASH)
The plugin reports OpenCPN's live icon set to navMate, replacing the old static hardcoded-43
assumption. Because the plugin is always the HTTP client (sec 1), this is a PUSH: the plugin reports
`icon_hash` in its POST; the hub caches+compares; on change the hub raises `want_icons` in the GET
view; the plugin then includes `ocpn_icons[]` (names + bitmaps) in its next POST.

`icon_hash` is a CONTENT HASH (not a DT), over the sorted FOREIGN (non-`nm:`) icon-name set. It must
be a hash, not a DT, because the icon registry is a MULTI-WRITER namespace with no single owner
(OpenCPN defaults + user imports + other plugins + navMate) - so nothing can mint a monotonic token
(contrast sec 3's single-minter DTs). The set is append/override-only within a session (no delete
API), so an in-session hash change is always an add/override; removals surface only across a restart
(which resets everything). Two-tier: the coarse name-hash is the trip-wire; each `ocpn_icons[]`
entry carries a `byte_hash` so navMate re-stores only changed bitmaps.

### Direction B - navMate library DOWN (plugin PULL, generation TOKEN)
navMate pushes its authored `nm:` library into OpenCPN so navMate-origin marks render with their
intended glyphs. Because the plugin is the client, this is a PULL: when `lib_gen` (in the GET view)
advances, the plugin GETs `?icons=1` and registers each `nm_icons[]` entry via
`AddCustomWaypointIcon` (decoding the entry's PNG via `wxImage`). SVG never rides the wire.

`lib_gen` is a monotonic GENERATION TOKEN (like `navmate_dt`), because navMate SINGLE-OWNS its
library - the single-minter property that lets sec 3 use a DT applies here. It is an INDEPENDENT
`key_values` counter (NOT folded onto `navmate_dt`, so a library edit doesn't spuriously tick the
command gate and vice-versa) and stays `1` (the initial fixed 36-sym set) until navMate grows an
authoring surface. The hash-up / token-down split is the same detector-by-ownership logic as
sec 3 / sec 4, derived a second time from first principles.

### Namespacing + the self-echo
navMate keys its icons `nm:<...>` (e.g. `nm:sym00`). This (a) COEXISTS with OpenCPN's built-ins
instead of overriding them (no-delete makes an override sticky-for-the-session); (b) is a table-free
MAGIC recognizer exactly like the navMate-origin GUID codec (sec 4) - an inbound `nm:` icon reverses
to its `sym` with no lookup; (c) KILLS the self-echo: the plugin's `icon_hash` covers only the
FOREIGN (non-`nm:`) set, so navMate never sees its own pushes bounce back as drift. For its own
`nm:` set navMate does presence-REPAIR (re-provision on the generation/restart reset), not
change-detection.

### Ordering gate
A mark referencing an unregistered icon name falls back to a default at render. So the plugin holds
an `icons_ensured` flag per connection: it will NOT apply any command whose `icon` is an `nm:` key
until the `nm:` set is registered. A generation reset (`ocpn_dt -> 0` / restart) clears the flag ->
re-provision.

### Color is a SYMBOL feature, not a field
`PlugIn_Waypoint_ExV2` has NO color member - a mark's color in OpenCPN IS its icon. So navMate's
`color` axis reaches OpenCPN ONLY as colored injected icons (per-`(sym,color)` `nm:` glyphs), never
as a wire field. v1 injects the fixed 36 `nm:symNN` glyphs by identity; per-`(sym,color)` on-demand
injects are the generalization when navMate's palette exceeds its base glyphs. (This is the single
point where the symbol channel and the field model, sec 6, couple.)

### Fidelity
**PNG 48x48 is THE icon-image format, both directions; SVG never rides the wire.** navMate is
raster-only (its Leaflet map uses PNG rasters, its picker is a raster `OwnerDrawnComboBox`), so
SVG-passthrough had zero consumers and was dropped. **The 48x48 is the icon CONTENT scaled to fill,
not the raw SVG canvas**: a stock markicon's glyph sits
small inside a padded viewBox, so the plugin renders the source SVG LARGE (256), crops to the alpha
content bounding box, and scales the glyph to fit the 48x48 (aspect-preserved, ~44px content + a
breathing margin). Resolution thus lives at the source; navMate only ever downscales (crisp), never
upscales. (Trade-off, accepted: content-fill normalizes every icon to ~one size, dropping OpenCPN's
relative-scale-on-chart -- correct for a picker; navMate renders map marks via derived syms anyway.)
navMate decodes PNG straight into its bitmaps -- no SVG renderer either side.

## 8. Push mechanics - field-level + merge-on-apply

`UpdateSingleWaypointEx/ExV2` OVERWRITE the entire field set from the passed struct (and
`m_HyperlinkList->clear()` + repopulate); there is no partial-update API. So a full-object
push built from a shadow would silently REVERT any OpenCPN-only field the shadow missed.
Instead the push is **field-level** and the plugin applies as read-modify-write:
`GetSingleWaypointExV2(guid)` -> overlay ONLY the fields the hub changed ->
`UpdateSingleWaypointExV2`. OpenCPN-only fields survive (never overwritten), using live
values -> zero staleness, and the hub need not shadow them. Apply runs on the MAIN thread
(model access); the worker fetches/returns off-thread. The overlay set now spans all carried
B-fields (sec 6); merge-on-apply is subset-agnostic - it Gets the live struct, overlays exactly
the changed fields, and Updates - so the wider field set adds no new clobber risk.

**Idempotency (the transport retries until the hub sees the result):**
- `add` of an existing GUID -> treated as `update` (upsert), `ok:true`.
- `update` of a vanished GUID -> `ok:false` + error; the hub re-drives as a full `add`.
- `delete` of an absent GUID -> `ok:true` ("ensure absent"), never an error.
- Route apply is idempotent too: check `GetRouteGUIDArray` for the route GUID -> update/skip
  if present, never re-Add (re-Add duplicates every point).
- Track apply is idempotent: an `add`/`update` of an existing track GUID must route through
  `UpdatePlugInTrack` (`ocpn_plugin.h:3267`), NOT `AddPlugInTrack` (which duplicates). This is
  what makes a winOCPN track RENAME stick (the live bug that drove the track correction).
  **Source-verified (5.12.4):** `UpdatePlugInTrack` is internally `DeleteTrack` + `AddPlugInTrack`
  (delete-then-reinsert) - it PRESERVES the caller's `m_GUID` but REBUILDS the track from the
  passed `pWaypointList`. So the plugin must apply it as a full read-modify-write:
  `GetTrack_Plugin(guid)` -> change metadata -> pass the FULL current point list ->
  `UpdatePlugInTrack`. A metadata-only struct would wipe the points. (navMate's winOCPN save
  already sends the whole point list.)

**Routes own their points on push.** There is no API to build a route that REFERENCES an
existing mark by GUID (`AddPlugInRoute` does `new RoutePoint(...)` per vertex). So each hub
waypoint manifests in OpenCPN exactly ONCE - as a standalone mark XOR as a vertex inside its
route, never both. The rare true-shared case (one point in multiple routes) is a hub-side
manifestation policy, WIRE-INVISIBLE to the plugin (navMate's model can express a shared point; how
it manifests in OpenCPN is the hub's choice, invisible to the wire either way).

## 9. Gate model

navMate's navOps runs silent, side-effect-free predicate gates as preflight BEFORE any
mutation is queued. A hub->OpenCPN push originates as a user PASTE into the hub's OpenCPN
spoke; if a gate rejects, the hub shows the USER an error and NOTHING is queued. **The
plugin never receives a gate rejection - it only ever GETs already-approved,
already-transformed commands.** The OpenCPN spoke is the gentlest: upsert-by-GUID removes the
collision gate; unbounded OpenCPN strings remove the truncation gate; the surviving
structural-legality gates are hub-internal and invisible. The plugin's ONLY failure channel
is apply-results (sec 10).

## 10. Command + result shapes

- **Command** (hub -> plugin, in `commands[]`): `{ op: add|update|delete, type:
  mark|route|track, guid, fields:{...} }`. `fields` = full mapped set for `add`, changed-only
  for `update`, absent for `delete`. For `type: route|track`, `fields.points[]` is the
  ORDERED member list - a route point is a full (route-owned) mark object; a track point is
  `{ lat, lon, ts }`.
- **Result** (plugin -> hub, in `results[]`): `{ guid, op, ok, error? }` per applied command.
  Deletes map 1:1 to `DeleteSingleWaypoint` / `DeletePlugInRoute` / `DeletePlugInTrack`.

## 11. Tracks

One object (metadata + points). Metadata carries `name` + `from`/`to` (`m_StartString`/
`m_EndString`). `track_points` is FLAT ordered on both sides
(`position,lat,lon,ts` [+ hub-only `depth_cm,temp_k`, kept by GUID]) - OpenCPN exposes no
segment boundaries, so segments are a non-issue between the two (they only ever mattered vs
GPX). Per OpenCPN point = `{lat, lon, ts}` (UTC, seconds); no per-point name/icon.

Tracks are ENUMERATED on change like marks and routes (OpenCPN gives no track edit event). A track
is EDITABLE in place: `UpdatePlugInTrack` preserves the caller's GUID, so a winOCPN rename or point
edit round-trips WITHOUT a new identity. It is internally delete+reinsert (source-verified, sec 8),
so the caller must pass the FULL point list. GAP (api-20): tracks expose no `visible`/color/style --
unlike routes, which expose `m_isVisible`; those native track properties are unreachable to a plugin.

## 12. Delta / efficiency

Full-state is the floor. The efficiency ceiling falls out with no new design:
field-level push IS the hub->OpenCPN delta; the plugin's hash+DT gate IS the OpenCPN->hub
delta (its hash expands to cover exactly the fields carried two-way, so a change to any
carried field re-syncs).

**Next:** The [**Implementation**](implementation.md) overview ...
