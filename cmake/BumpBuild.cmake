# ~~~
# Advance the build counter NNN and (re)write the internal build-number header.
# The CANONICAL counter is the "Current build number: NNN" line in docs/releases.md -
# there is NO build_number.txt. Run PRE_BUILD so NNN advances on every build; the
# EXTERNAL version X.Y is Patrick's (Plugin.cmake PKG_VERSION). Full version = X.Y.NNN.
# Args: -DRELEASES=<docs/releases.md> -DHEADER=<oe_buildnum.h> -DVERSION=<X.Y>
# ~~~
if (NOT EXISTS "${RELEASES}")
  message(FATAL_ERROR "BumpBuild: canonical build counter not found: ${RELEASES}")
endif ()

file(READ "${RELEASES}" _rel)
if (_rel MATCHES "Current build number: ([0-9]+)")
  set(_n "${CMAKE_MATCH_1}")
else ()
  message(FATAL_ERROR "BumpBuild: no 'Current build number: NNN' line in ${RELEASES}")
endif ()
math(EXPR _n "${_n} + 1")

# Write the advanced counter back into the canonical location (docs/releases.md).
string(REGEX REPLACE
  "Current build number: [0-9]+" "Current build number: ${_n}" _rel "${_rel}")
file(WRITE "${RELEASES}" "${_rel}")

# (Re)generate the per-build header the plugin compiles against, so OpenCPN reports X.Y.NNN.
get_filename_component(_dir "${HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
file(WRITE "${HEADER}"
  "#ifndef OE_BUILDNUM_H__\n"
  "#define OE_BUILDNUM_H__\n"
  "// Generated per build by cmake/BumpBuild.cmake from docs/releases.md - do not edit, do not commit.\n"
  "#define PLUGIN_BUILD_NUMBER ${_n}\n"
  "#define PLUGIN_VERSION_FULL \"${VERSION}.${_n}\"\n"
  "#endif  // OE_BUILDNUM_H__\n"
)

message(STATUS "oESeries build ${VERSION}.${_n} (canonical counter: docs/releases.md)")
