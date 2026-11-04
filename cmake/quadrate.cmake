# The Quadrate release QDOS builds against, for the two builds that compile it
# from source: the browser (web/) and the ESP32 (esp32/components/quadrate).
# Meson takes the same release prebuilt, through subprojects/quadrate.wrap, and
# the firmware image from firmware/buildroot/package/quadrate. To move to a new
# release, change the version and hashes in all three.
#
# The tag's source archive is downloaded and unpacked in the build directory,
# along with the two libraries Quadrate's own build brings in as Meson
# subprojects: u8t and libmpdec. The versions are the ones its wraps name.
#
# QUADRATE_SRC in the environment points at a local tree instead, for working
# on Quadrate and QDOS together. That tree needs `meson subprojects download`
# run in it once.
#
# Sets QD (the Quadrate tree), QD_U8T and QD_MPDEC (its subprojects) and
# QD_VERSION (the version the runtime reports).

set(QUADRATE_VERSION 0.7.0)
set(QUADRATE_SHA256 0ad42302ab095bdad4437cb346bfcbf08eef0880472b52c0e804d48218bf332e)
set(QUADRATE_U8T_VERSION 1.4.2)
set(QUADRATE_U8T_SHA256 97d0d30b8b6f69531fd95ceefd724b9770875f14fb9a3c466c26dc155b4aab61)
set(QUADRATE_MPDEC_VERSION 4.0.1)
set(QUADRATE_MPDEC_SHA256 96d33abb4bb0070c7be0fed4246cd38416188325f820468214471938545b1ac8)

# Unpacks the archive at url to dest, unless an earlier configure already did
function(qd_fetch url sha256 dest)
	if(EXISTS "${dest}/.qd-fetched")
		file(READ "${dest}/.qd-fetched" fetched)
		if(fetched STREQUAL sha256)
			return()
		endif()
	endif()

	get_filename_component(parent "${dest}" DIRECTORY)
	get_filename_component(name "${dest}" NAME)
	set(archive "${parent}/${name}.tar.gz")
	set(unpack "${parent}/${name}.unpack")

	message(STATUS "Fetching ${url}")
	file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${sha256} STATUS status)
	list(GET status 0 code)
	if(NOT code EQUAL 0)
		list(GET status 1 reason)
		message(FATAL_ERROR "Could not download ${url}: ${reason}\n"
			"Set QUADRATE_SRC to build against a local Quadrate tree instead.")
	endif()

	file(REMOVE_RECURSE "${unpack}" "${dest}")
	file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${unpack}")
	# Each archive holds a single top-level directory, named after its version
	file(GLOB top LIST_DIRECTORIES true "${unpack}/*")
	file(RENAME "${top}" "${dest}")
	file(REMOVE_RECURSE "${unpack}" "${archive}")
	file(WRITE "${dest}/.qd-fetched" "${sha256}")
endfunction()

if(DEFINED ENV{QUADRATE_SRC})
	get_filename_component(QD "$ENV{QUADRATE_SRC}" ABSOLUTE)
	set(QD_U8T "${QD}/subprojects/u8t")
	set(QD_MPDEC "${QD}/subprojects/mpdecimal-${QUADRATE_MPDEC_VERSION}")

	if(NOT EXISTS "${QD}/lib/interp/src/interp.cc")
		message(FATAL_ERROR "No Quadrate source tree at QUADRATE_SRC (${QD}).")
	endif()
	if(NOT EXISTS "${QD_U8T}/src/scanner.c" OR NOT EXISTS "${QD_MPDEC}/libmpdec/mpdecimal.c")
		message(FATAL_ERROR "u8t or libmpdec is missing from ${QD}/subprojects.\n"
			"Run `meson subprojects download` in the Quadrate tree, or build it once.")
	endif()

	# A working tree reports the tag it is on, as Meson takes it
	execute_process(
		COMMAND git -C "${QD}" describe --tags --abbrev=0
		OUTPUT_VARIABLE QD_VERSION
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET)
	if(NOT QD_VERSION MATCHES "^v?[0-9]+\\.[0-9]+\\.[0-9]+")
		set(QD_VERSION "0.0.0-unknown")
	endif()
	message(STATUS "Quadrate: ${QD} (${QD_VERSION})")
else()
	set(deps "${CMAKE_BINARY_DIR}/_deps")
	set(QD "${deps}/quadrate-${QUADRATE_VERSION}")
	set(QD_U8T "${deps}/u8t-${QUADRATE_U8T_VERSION}")
	set(QD_MPDEC "${deps}/mpdecimal-${QUADRATE_MPDEC_VERSION}")
	set(QD_VERSION "${QUADRATE_VERSION}")

	qd_fetch("https://github.com/quadrate-language/quadrate/archive/refs/tags/${QUADRATE_VERSION}.tar.gz"
		${QUADRATE_SHA256} "${QD}")
	qd_fetch("https://github.com/klahr/libu8t/archive/refs/tags/${QUADRATE_U8T_VERSION}.tar.gz"
		${QUADRATE_U8T_SHA256} "${QD_U8T}")
	qd_fetch("https://www.bytereef.org/software/mpdecimal/releases/mpdecimal-${QUADRATE_MPDEC_VERSION}.tar.gz"
		${QUADRATE_MPDEC_SHA256} "${QD_MPDEC}")
	message(STATUS "Quadrate: ${QUADRATE_VERSION}")
endif()

string(REGEX MATCH "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${QD_VERSION}")
set(QD_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(QD_VERSION_MINOR "${CMAKE_MATCH_2}")
set(QD_VERSION_PATCH "${CMAKE_MATCH_3}")
if(QD_VERSION_MAJOR STREQUAL "")
	set(QD_VERSION_MAJOR 0)
	set(QD_VERSION_MINOR 0)
	set(QD_VERSION_PATCH 0)
endif()
math(EXPR QD_VERSION_API "${QD_VERSION_MAJOR} * 10000 + ${QD_VERSION_MINOR} * 100 + ${QD_VERSION_PATCH}")
