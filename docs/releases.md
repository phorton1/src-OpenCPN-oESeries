# oESeries - Releases

**[Home](readme.md)** --
**[Getting Started](getting_started.md)** --
**[Design](design.md)** --
**[Protocol](protocol.md)** --
**[Implementation](implementation.md)** --
**[Build](build.md)** --
**Releases**

**Current build number: 14**

The OpenCPN plugin **tarball** for each public release is committed under
[`releases/`](../releases/) at the repo root (it is small and self-contained) and installed
into OpenCPN via **Options -> Plugins -> Import plugin...**. Requires **OpenCPN 5.12+,
Windows** (the standard 32-bit build); built against wxWidgets 3.2.10, plugin API 1.20.

## Version scheme

The full version is **`X.Y.NNN`**:

- **`X.Y`** -- the public major.minor, owned by the author and bumped by hand at each public
  release (`PKG_VERSION` in `Plugin.cmake`). Each published release increments at least `X`.
- **`NNN`** -- the build number, owned by the build. It advances on **every** build, and its
  **canonical home is the `Current build number:` line at the top of this file** -- there is no
  `build_number.txt`. The build reads it here, increments it, writes it back, and regenerates the
  version header the plugin compiles against.

The same `X.Y.NNN` shows through everywhere: OpenCPN's plugin display, the git tag (`vX.Y.NNN`),
the tarball filename, and the tarball's `metadata.xml`. So the version shown in OpenCPN is exactly
the build that was installed.

> **Do not hand-edit the `Current build number:` line** -- the build owns it. Editing it desyncs
> the counter from the built artifacts.

This is a release LOG, not a changelog; git history is authoritative for what changed between
versions.

## Releases

| date | version | notes |
| ---- | ------- | ----- |
| 2026-07-10 | 0.1.14 | Initial public pre-release of the plugin -- see below. |

### oESeries 0.1.14 -- initial pre-release

> The initial public build -- built, proven end-to-end against a live navMate, committed, and
> released as the tarball below.

First numbered build of the oESeries plugin -- OpenCPN as a driven spoke of navMate:

- Bidirectional waypoint / route / track sync over the polling HTTP protocol
  ([protocol.md](protocol.md)), carrying the full OpenCPN-only **B-field superset**
  (visibility, scamin, arrival radius, range rings, planned speed, ETD, hyperlinks, ...).
- Track upsert via `UpdatePlugInTrack` (rename + points preserved); route add-of-existing
  guard.
- **Symbol channel**: `icon_hash` + the live foreign icon vocabulary reported up; navMate
  `nm:` library registered down via `AddCustomWaypointIcon`; `icons_ensured` ordering gate.
- **Direction-A icon images**: the foreign icon set rasterized to **48x48 PNG** at emit
  (stock markicons and user icons located on disk), with a per-icon `byte_hash` and a
  `builtin` flag; icons with no locatable source stay names-only.
