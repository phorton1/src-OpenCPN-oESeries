# ~~~
# Summary:      Local, non-generic plugin setup
# Copyright (c) 2020-2021 Mike Rossiter (framework)
# License:      GPLv3+
# ~~~

# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.


# -------- Options ----------
#
# Cloudsmith upload repositories. Placeholders - oESeries is not published yet;
# these only keep Metadata.cmake from configuring with an empty repo string.

set(OCPN_TEST_REPO
    "oeseries/oeseries-alpha"
    CACHE STRING "Default repository for untagged builds"
)
set(OCPN_BETA_REPO
    "oeseries/oeseries-beta"
    CACHE STRING
    "Default repository for tagged builds matching 'beta'"
)
set(OCPN_RELEASE_REPO
    "oeseries/oeseries-prod"
    CACHE STRING
    "Default repository for tagged builds not matching 'beta'"
)

#
#
# -------  Plugin setup --------
#
set(PKG_NAME oESeries_pi)
set(PKG_VERSION 0.1) # Major.Minor - OpenCPN plugins are X.Y; EXTERNAL version, bump by hand at each public release
set(PKG_PRERELEASE "")  # Empty, or a tag like 'beta'

set(DISPLAY_NAME oESeries)    # Dialogs, installer artifacts, ...
set(PLUGIN_API_NAME oESeries) # As of GetCommonName() in plugin API
set(PKG_SUMMARY "navMate spoke for OpenCPN")
set(PKG_DESCRIPTION [=[
Makes OpenCPN a driven spoke of navMate. A polling HTTP client that mirrors
OpenCPN's waypoints, routes and tracks to the navMate hub and applies commands
pushed back from it.
]=])

set(PKG_AUTHOR "Patrick Horton")
set(PKG_IS_OPEN_SOURCE "yes")
set(PKG_HOMEPAGE https://github.com/phorton1/src-OpenCPN-oESeries)
set(PKG_INFO_URL https://github.com/phorton1/src-OpenCPN-oESeries)

set(SRC
    src/oeSeries_pi.h
    src/oeSeries_pi.cpp
    src/oeSeries_log.h
    src/oeSeries_log.cpp
    src/oeSeries_http.h
    src/oeSeries_http.cpp
    src/json.hpp            # vendored nlohmann/json v3.11.3 (header-only)
)

set(PKG_API_LIB api-20)  #  A dir in opencpn_libs/ e.g., api-20 or api-19

# No MIN_API_VERSION, no late_init(), no add_plugin_libraries() - this minimal
# skeleton links only ocpn::api (and, via PluginLibs, wxWidgets + OpenGL).
