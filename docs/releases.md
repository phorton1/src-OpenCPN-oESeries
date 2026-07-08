# oESeries - Releases

**[Home](readme.md)** --
**[Design](design.md)** --
**[Protocol](protocol.md)** --
**[Implementation](implementation.md)** --
**[Build](build.md)** --
**Releases**

**Current build number: 11**

The OpenCPN plugin **tarball** for each public release is committed under
[`releases/`](../releases/) at the repo root (it is small and self-contained) and installed
into OpenCPN via **Options -> Plugins -> Import plugin...**. Requires **OpenCPN 5.12+,
Windows** (the standard 32-bit build); built against wxWidgets 3.2.10, plugin API 1.20.

## Version scheme

OpenCPN plugins are versioned **major.minor (`X.Y`)**. oESeries uses:

- **External `X.Y`** -- the public version (Plugin Manager; tarball filename
  `oESeries-X.Y_msvc-wx32-<winbuild>-x86.tar.gz`). Bumped by hand at each public release.
  Rebuilding one version overwrites its single tarball, so `releases/` holds exactly one
  file per public version -- no pile of throwaway builds.
- **Internal `X.Y.NNN`** -- `NNN` auto-increments on **every** build (untracked
  `build_number.txt`), is stamped into the DLL (`Init: vX.Y.NNN ...` in the plugin log and
  the `{op:diag,type:state}` readback), and refreshes the **Current build number** shown
  above so this page always reflects the latest local build.

This is a release LOG, not a changelog; git history is authoritative for what changed
between versions.

## Releases

| date | version | notes |
| ---- | ------- | ----- |
| (pending) | 0.1 | Initial public pre-release of the plugin -- see below. |

### oESeries 0.1 -- initial pre-release

> The initial public build. The plugin below is built, proven end-to-end against a live
> navMate, and committed; cutting the public tarball and tag is the remaining step.

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
