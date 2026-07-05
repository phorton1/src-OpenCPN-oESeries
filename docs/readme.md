# oESeries - OpenCPN Waypoint / Route / Track Sync Plugin

**Home** --
**[Design](design.md)** --
**[Protocol](protocol.md)** --
**[Implementation](implementation.md)** --
**[Build](build.md)**

**oESeries** is a C++ plugin for [**OpenCPN**](https://opencpn.org) that mirrors
OpenCPN's **waypoints, routes, and tracks** to and from an external hub over a small
HTTP sync protocol. The plugin runs inside OpenCPN as a polling client; the hub is the
authoritative peer. Today that hub is [**navMate**](#please-also-see); the same
reconciliation engine is intended to serve other peers as well (a possible direct
Raymarine **E-Series** bridge, for example).

At its core oESeries is a **replica-reconciliation engine**: it watches OpenCPN's live
navigation objects, detects change, and exchanges only what differs with the hub - by
GUID, idempotently, in both directions - so an edit made in OpenCPN shows up at the hub,
and an object pushed from the hub shows up on the chart.

## Documentation Outline

- **[Design](design.md)** - the architecture and the enduring capabilities: hub/spoke
  roles, the two sync directions, change-detection and the version gate, the threading
  model, and logging.
- **[Protocol](protocol.md)** - the wire specification (spec v1): the endpoint, the
  message shapes, identity/GUID, the field model, symbols, push mechanics, and the gate
  model. This is the canonical contract between oESeries and the hub.
- **[Implementation](implementation.md)** - an overview of the source code: the plugin
  class, the leveled logger, the off-thread HTTP worker, and the main-thread sync engine.
- **[Build](build.md)** - the toolchain and how to build the plugin, plus the dev-test
  install and the OpenCPN host-load notes.

## Credits

This project is built on the public open-source work of others:

- [**OpenCPN**](https://opencpn.org) and its [**plugin API**](https://github.com/OpenCPN/OpenCPN)
- [**wxWidgets**](https://www.wxwidgets.org/) - the cross-platform C++ framework OpenCPN and its plugins use
- The [**OpenCPN plugin CMake framework**](https://github.com/OpenCPN/opencpn-libs) by
  Alec Leamas, Mike Rossiter, and Pavel Kalian - the cross-platform build/package system
- The [**Shipdriver**](https://github.com/Rasbats/shipdriver_pi) reference plugin by Mike
  Rossiter, used to validate the toolchain

## License

Copyright (c) 2026 Patrick Horton

This program, project, and repository is free software: you can redistribute it and/or
modify it under the terms of the GNU General Public License Version 3 as published by the
Free Software Foundation.

These materials are distributed in the hope that they will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR ANY
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

Please see [LICENSE.TXT](../LICENSE.TXT) for more information.

## Please Also See

[**navMate**](https://github.com/phorton1/base-apps-navMate) - the boat-navigation hub that
oESeries is, today, a spoke of. navMate holds
the authoritative waypoint/route/track model and reconciles OpenCPN's objects with its
other spokes (Raymarine E-Series plotters, FSH fishfinders, and its own database). The
[Protocol](protocol.md) is co-owned with navMate; the sync loop was brought up and proven
against a live navMate server.

**Next:** A [**Design Overview**](design.md) of the plugin ...
