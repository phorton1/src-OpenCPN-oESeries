# ~~~
# Increment the untracked build counter and (re)write the internal build-number header.
# Run as a PRE_BUILD step so NNN advances on EVERY build; the EXTERNAL version stays X.Y.Z.
# Args: -DCOUNTER=<build_number.txt> -DHEADER=<oe_buildnum.h> -DVERSION=<X.Y>
#       [-DRELEASES=<docs/releases.md>]  (its "Current build number: NNN" line is refreshed)
# ~~~
if (EXISTS "${COUNTER}")
  file(READ "${COUNTER}" _n)
  string(STRIP "${_n}" _n)
endif ()
if (NOT _n MATCHES "^[0-9]+$")
  set(_n 0)
endif ()
math(EXPR _n "${_n} + 1")
file(WRITE "${COUNTER}" "${_n}\n")

get_filename_component(_dir "${HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
file(WRITE "${HEADER}"
  "#ifndef OE_BUILDNUM_H__\n"
  "#define OE_BUILDNUM_H__\n"
  "// Generated per build by cmake/BumpBuild.cmake - do not edit, do not commit.\n"
  "#define PLUGIN_BUILD_NUMBER ${_n}\n"
  "#define PLUGIN_VERSION_FULL \"${VERSION}.${_n}\"\n"
  "#endif  // OE_BUILDNUM_H__\n"
)
# Refresh the live "Current build number" shown at the top of the tracked releases doc.
if (RELEASES AND EXISTS "${RELEASES}")
  file(READ "${RELEASES}" _rel)
  string(REGEX REPLACE "Current build number: [0-9]+" "Current build number: ${_n}" _rel "${_rel}")
  file(WRITE "${RELEASES}" "${_rel}")
endif ()

message(STATUS "oESeries internal build ${VERSION}.${_n}")
