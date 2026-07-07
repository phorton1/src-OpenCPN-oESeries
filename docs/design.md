# oESeries - Design

**[Home](readme.md)** --
**Design** --
**[Protocol](protocol.md)** --
**[Implementation](implementation.md)** --
**[Build](build.md)** --
**[Releases](releases.md)**

This page describes the architecture of oESeries and the enduring capabilities it
provides. The precise wire contract lives in [Protocol](protocol.md); the code that
realizes it is described in [Implementation](implementation.md).

## What oESeries is

oESeries is a **replica-reconciliation engine** living inside OpenCPN. OpenCPN owns a live
model of navigation objects - **W**aypoints (marks), **R**outes, and **T**racks (WRTs) -
and so does the hub. oESeries keeps the two in agreement: it detects change on the OpenCPN
side, exchanges only what differs with the hub, and applies changes the hub pushes back -
all keyed by object **GUID**, idempotently, in both directions.

The engine is deliberately **peer-agnostic**. The concrete peer today is **navMate** (the
boat-navigation hub), reached over HTTP. The same engine is intended to serve other peers -
for example a direct Raymarine **E-Series** bridge that would run without navMate present.
Nothing in the core assumes navMate specifically; only the transport and the field/symbol
mapping are peer-specific.

## Roles - hub and spoke

- **The hub is the authoritative model and the HTTP server.** With navMate, its `navServer`
  already exists; oESeries writes no server code.
- **oESeries is a polling HTTP client** - the spoke. It never serves; it only initiates
  outbound requests. Same-box `localhost` and cross-box LAN are identical code (only a host
  string differs).
- **The spoke is a lossy projection of the hub.** The hub carries richer data than OpenCPN
  can show; the spoke carries OpenCPN-specific data the hub has no column for. Neither is a
  superset - see the field model in [Protocol](protocol.md).

This direction was chosen because the hub already runs a server (no plugin-side server
needed), the plugin only needs outbound HTTP, and it fits the hub-and-spoke model cleanly.

## The two sync directions

A `wxTimer` heartbeat on OpenCPN's main thread is the only clock; **the plugin always
initiates, the hub always responds.** Every heartbeat is one request that carries both
directions:

- **OpenCPN -> hub (mirror the user's edits).** OpenCPN fires **no event** when the user
  creates or edits a mark or route, so oESeries cannot wait to be told. It **discovers**
  state by ENUMERATION - it reads the full current set of objects, detects change, and
  sends what differs. (Tracks are the one exception; see below.)
- **hub -> OpenCPN (apply pushes).** The plugin receives a batch of commands
  (add/update/delete by GUID) and applies them to OpenCPN's live model, then reports the
  results. Applying an update is a **field-level merge** so OpenCPN-only data is never
  clobbered.

## Change detection and the version gate

Because OpenCPN gives no edit event, the irreducible cost is a periodic **enumerate + hash**
on the main thread. oESeries keeps this cheap and turns it into an efficient sync with two
mechanisms:

- **A content hash is the local change DETECTOR.** Each heartbeat, the plugin enumerates
  the objects, builds a canonical form of the fields it carries, and hashes it. If the hash
  is unchanged, nothing needs to be sent.
- **A two-DT version gate is the wire TOKEN.** Each side keeps a pair of date-time tokens
  `{DT_ocpn, DT_navmate}`. `DT_ocpn` is minted only by the plugin (advanced when its hash
  changes); `DT_navmate` only by the hub. Each is compared by EQUALITY against a cached copy
  of itself, so it is immune to clock skew and a mismatch alone names which side changed.
  When both match, the heartbeat is a no-op; only the half whose token advanced is
  transferred.

The result: steady state is quiet, a user edit propagates within a heartbeat, and a
hub-side change arrives the same way. Full-state exchange is the floor; sending only deltas
is the efficiency ceiling, and it falls out of the same hash+token machinery. See
[Protocol](protocol.md) for the exact tokens and message shapes.

## Threading and performance

- The `wxTimer` fires on the **main thread**; OpenCPN's navigation model may only be
  touched there. So enumeration, hashing, and applying pushed commands all run on the main
  thread - where their steady-state cost is negligible.
- **Network I/O never runs on the main thread.** A failed connect can block for the OS
  default (~21 s on Windows for an unreachable host), which would freeze the UI. HTTP runs
  on a worker thread with an explicit short connect timeout; the finished payload is handed
  back to the main thread.
- **No persistent connection.** Each poll is a fresh, short-lived request. If the hub is
  down, the poll simply fails and is retried on the next heartbeat - quietly, indefinitely,
  self-healing. Loose coupling is a feature: either side can restart and the loop re-syncs.

## Tracks - the one place OpenCPN gives an event

Unlike marks and routes, the **active (recording) track** does raise an event
(`OCPN_TRK_POINT_ADDED`) each time OpenCPN lays down a point. So oESeries handles tracks
with a split strategy: the active track is streamed by **event-append** (one point per
event, no re-scan), while completed tracks are immutable and enumerated once, like marks.
Details in [Protocol](protocol.md).

## Logging

OpenCPN is a GUI-subsystem application with no console, so oESeries logs two ways:

- **Milestones and errors** go to OpenCPN's own log (`opencpn.log`) via `wxLogMessage`,
  prefixed `oESeries:` - sparse, for events worth seeing without turning anything on.
- **Verbose sync churn** goes to the plugin's **own leveled log file** in OpenCPN's private
  data directory. It mirrors navMate's `display(level, indent, msg)` convention: a global
  debug level filters messages, and the level is exposed live in the plugin's preferences
  dialog so the sync loop can be watched breathing while it is brought up.

## Coordination with the hub

oESeries owns the client and the OpenCPN model; the hub owns its server side - the HTTP
endpoints, its in-memory representation of the spoke, GUID reconciliation, and its own
gating rules. Those are the hub's to build; the [Protocol](protocol.md) defines only the
seam between them. With navMate specifically, the spoke is held in memory and is not
persisted to navMate's database until a deliberate user action moves it there - so OpenCPN
remains the source of truth for its own live state.

**Next:** The [**Sync Protocol**](protocol.md) specification ...
