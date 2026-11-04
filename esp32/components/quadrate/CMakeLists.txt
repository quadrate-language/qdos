# Quadrate, built from source as an ESP-IDF component.
#
# The same libraries the Pi links -- the runtime, the compiler front-end, the
# AST interpreter, u8t and the math and decimal modules -- compiled here rather
# than taken from a release archive, because those are for the host. Which
# release, and how it is fetched, is in cmake/quadrate.cmake. Meson is
# Quadrate's build and stays its build; this file only lists sources.

# IDF reads every component once early, only for its requirements, and this
# one has none: the download waits for the real configure
if(CMAKE_BUILD_EARLY_EXPANSION)
	idf_component_register()
	return()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/quadrate.cmake")

set(rt "${QD}/lib/rt/src")
# lib/qc is whatever its directory holds, as its meson.build lists all of it
file(GLOB qc_sources CONFIGURE_DEPENDS "${QD}/lib/qc/src/*.cc")
set(mpd "${QD_MPDEC}/libmpdec")

# libmpdec, behind d128, in the portable ANSI C configuration Quadrate's Meson
# build picks, and the 32-bit one, as the S3 is
set(mpd_sources
	"${mpd}/basearith.c"
	"${mpd}/constants.c"
	"${mpd}/context.c"
	"${mpd}/convolute.c"
	"${mpd}/crt.c"
	"${mpd}/difradix2.c"
	"${mpd}/fnt.c"
	"${mpd}/fourstep.c"
	"${mpd}/io.c"
	"${mpd}/mpalloc.c"
	"${mpd}/mpdecimal.c"
	"${mpd}/mpsignal.c"
	"${mpd}/numbertheory.c"
	"${mpd}/sixstep.c"
	"${mpd}/transpose.c")

idf_component_register(
	SRCS
		# lib/rt, with the platform layer for a target that has no processes
		# no dynamic linker and no C11 threads, so its mutex is a pthread one
		"${rt}/d128.c"
		"${rt}/natives.c"
		"${rt}/runtime.c"
		"${rt}/runtime_stack.c"
		"${rt}/runtime_ops.c"
		"${rt}/runtime_io.c"
		"${rt}/runtime_d128.c"
		"${rt}/stack.c"
		"${rt}/qd_string.c"
		"${rt}/qd_struct.c"
		"${rt}/array.c"
		"${rt}/platform/pthread/mutex_pthread.c"
		"${rt}/platform/none/dynlib_none.c"
		"${rt}/platform/none/process_none.c"
		"${rt}/platform/none/exe_path_none.c"
		${qc_sources}
		# lib/interp
		"${QD}/lib/interp/src/interp.cc"
		# u8t
		"${QD_U8T}/src/scanner.c"
		${mpd_sources}
		# stdlib/math
		"${QD}/stdlib/math/src/math.c"
		# stdlib/decimal
		"${QD}/stdlib/decimal/src/decimal.c"
	INCLUDE_DIRS
		# newlib's gaps, filled for u8t's public header
		"compat"
		"${QD}/lib/rt/include"
		"${QD}/lib/qc/include"
		"${QD}/lib/interp/include"
		"${QD_U8T}/include"
		"${QD}/stdlib/math/include"
		"${QD}/stdlib/decimal/include"
	PRIV_INCLUDE_DIRS
		"${QD}/lib/rt"
		"${QD}/lib/platform/include"
	LDFRAGMENTS "linker.lf")

# The math module's registration table is generated from its .qd declarations,
# exactly as the Meson build does it.
idf_build_get_property(python PYTHON)
set(math_register "${CMAKE_CURRENT_BINARY_DIR}/math_register.c")
add_custom_command(
	OUTPUT "${math_register}"
	COMMAND "${python}" "${QD}/tools/gen_module_register.py" --module math
		"${QD}/stdlib/math/qd/math/math.qd" "${math_register}"
	DEPENDS "${QD}/tools/gen_module_register.py" "${QD}/stdlib/math/qd/math/math.qd"
	VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${math_register}")

set(decimal_register "${CMAKE_CURRENT_BINARY_DIR}/decimal_register.c")
add_custom_command(
	OUTPUT "${decimal_register}"
	COMMAND "${python}" "${QD}/tools/gen_module_register.py" --module decimal
		"${QD}/stdlib/decimal/qd/decimal/decimal.qd" "${decimal_register}"
	DEPENDS "${QD}/tools/gen_module_register.py" "${QD}/stdlib/decimal/qd/decimal/decimal.qd"
	VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${decimal_register}")

# mpdecimal.h is generated, as libmpdec's own build does it
set(MPD_HEADER_CONFIG "/* ABI: 32-bit */\n#define MPD_CONFIG_32 1")
configure_file("${mpd}/mpdecimal.h.in" "${CMAKE_CURRENT_BINARY_DIR}/mpdec/mpdecimal.h" @ONLY)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/mpdec" "${mpd}")
set_source_files_properties(${mpd_sources} PROPERTIES
	COMPILE_DEFINITIONS "ANSI;CONFIG_32;NDEBUG"
	COMPILE_OPTIONS "-w;-O2")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
	QD_VERSION="${QD_VERSION}"
	QD_VERSION_MAJOR=${QD_VERSION_MAJOR}
	QD_VERSION_MINOR=${QD_VERSION_MINOR}
	QD_VERSION_PATCH=${QD_VERSION_PATCH}
	QD_VERSION_API=${QD_VERSION_API}
	# No coverage on a calculator: the tables are 144 KB of static RAM
	QD_MAX_COVERED_FUNCS=1
	# Each level of the walk is about 1.25 KB of C stack on the S3, and the
	# shell's stack has to be internal RAM (see main/main.c). 256 would want
	# 360 KB of it.
	QD_INTERP_MAX_CALL_DEPTH=${CONFIG_QDOS_CALL_DEPTH})

target_compile_options(${COMPONENT_LIB} PRIVATE
	$<$<COMPILE_LANGUAGE:C>:-std=gnu11>
	$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
	# Quadrate is warning-clean under its own flags, not under IDF's -Werror=all.
	# Format mismatches stay errors: on a 32-bit target they print wrong numbers.
	-Wno-error
	-Werror=format)
