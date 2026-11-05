# The simulator in a browser: the SDL3 backend, compiled with Emscripten.
#
#   emcmake cmake -S web -B build-web -G Ninja
#   cmake --build build-web
#   emrun build-web/qdos.html      (or any static file server)
#
# As on the ESP32, Quadrate is compiled from source here rather than linked
# from its release archives, which are for the host. Which release, and how it
# is fetched, is in cmake/quadrate.cmake. Meson is Quadrate's build and stays
# its build; this file only lists sources.

cmake_minimum_required(VERSION 3.20)
project(qdos_web C CXX)

if(NOT EMSCRIPTEN)
	message(FATAL_ERROR "This is the browser build; configure it with emcmake:\n"
		"  emcmake cmake -S web -B build-web")
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(QDOS "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

include("${QDOS}/cmake/quadrate.cmake")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS ON)

# ---------------------------------------------------------------------------
# Quadrate
# ---------------------------------------------------------------------------

set(rt "${QD}/lib/rt/src")
set(mpd "${QD_MPDEC}/libmpdec")

# libmpdec, behind d128, in the portable ANSI C configuration Quadrate's Meson
# build picks, and the 32-bit one, as wasm32 is
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

set(MPD_HEADER_CONFIG "/* ABI: 32-bit */\n#define MPD_CONFIG_32 1")
configure_file("${mpd}/mpdecimal.h.in" "${CMAKE_CURRENT_BINARY_DIR}/mpdec/mpdecimal.h" @ONLY)
set_source_files_properties(${mpd_sources} PROPERTIES
	COMPILE_DEFINITIONS "ANSI;CONFIG_32;NDEBUG"
	COMPILE_OPTIONS "-w")

# lib/qc is whatever its directory holds, as its meson.build lists all of it
file(GLOB qc_sources CONFIGURE_DEPENDS "${QD}/lib/qc/src/*.cc")

# The module registration tables are generated from their .qd declarations,
# exactly as the Meson build does it
set(module_registers)
foreach(module math decimal)
	set(out "${CMAKE_CURRENT_BINARY_DIR}/${module}_register.c")
	set(decl "${QD}/stdlib/${module}/qd/${module}/${module}.qd")
	add_custom_command(
		OUTPUT "${out}"
		COMMAND Python3::Interpreter "${QD}/tools/gen_module_register.py" --module ${module} "${decl}" "${out}"
		DEPENDS "${QD}/tools/gen_module_register.py" "${decl}"
		VERBATIM)
	list(APPEND module_registers "${out}")
endforeach()

add_library(quadrate STATIC
	# lib/rt, with the platform layer for a target that has no processes and
	# no dynamic linker
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
	"${rt}/platform/mutex.c"
	"${rt}/platform/none/dynlib_none.c"
	"${rt}/platform/none/process_none.c"
	"${rt}/platform/none/exe_path_none.c"
	${qc_sources}
	"${QD}/lib/interp/src/interp.cc"
	"${QD_U8T}/src/scanner.c"
	${mpd_sources}
	"${QD}/stdlib/math/src/math.c"
	"${QD}/stdlib/decimal/src/decimal.c"
	${module_registers})

target_include_directories(quadrate SYSTEM PUBLIC
	"${QD}/lib/rt/include"
	"${QD}/lib/qc/include"
	"${QD}/lib/interp/include"
	"${QD_U8T}/include"
	"${QD}/stdlib/math/include"
	"${QD}/stdlib/decimal/include")
target_include_directories(quadrate PRIVATE
	"${QD}/lib/rt"
	"${QD}/lib/platform/include"
	"${CMAKE_CURRENT_BINARY_DIR}/mpdec"
	"${mpd}")

target_compile_definitions(quadrate PRIVATE
	QD_VERSION="${QD_VERSION}"
	QD_VERSION_MAJOR=${QD_VERSION_MAJOR}
	QD_VERSION_MINOR=${QD_VERSION_MINOR}
	QD_VERSION_PATCH=${QD_VERSION_PATCH}
	QD_VERSION_API=${QD_VERSION_API})

target_compile_options(quadrate PRIVATE -w)

# ---------------------------------------------------------------------------
# QDOS
# ---------------------------------------------------------------------------

# The about view reports what it is running; the version comes from
# meson.build, and the commit from git
file(READ "${QDOS}/meson.build" meson_build)
string(REGEX MATCH "version: '([^']+)'" _ "${meson_build}")
set(qdos_version "${CMAKE_MATCH_1}")
execute_process(
	COMMAND git -C "${QDOS}" describe --always --dirty
	OUTPUT_VARIABLE qdos_commit
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET)
if(NOT qdos_commit)
	set(qdos_commit "unknown")
endif()
set(version_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${version_dir}")
file(WRITE "${version_dir}/qdos_version.h"
	"#define QDOS_COMMIT \"${qdos_commit}\"\n#define QDOS_VERSION \"${qdos_version}\"\n")

# The same sources as qdos_core in meson.build, less the two that read Linux
# devices (the evdev keypad and the fbdev blit). Keep the lists in step.
add_executable(qdos
	"${QDOS}/src/main.c"
	"${QDOS}/src/hal/sim/sim_sdl3.c"
	"${QDOS}/src/shell/shell.c"
	"${QDOS}/src/shell/storage.c"
	"${QDOS}/src/shell/complete.c"
	"${QDOS}/src/shell/wordlist.c"
	"${QDOS}/src/shell/wordhelp.c"
	"${QDOS}/src/shell/editor.c"
	"${QDOS}/src/shell/graph.c"
	"${QDOS}/src/shell/surface.c"
	"${QDOS}/src/shell/guarded.c"
	"${QDOS}/src/shell/lint.cc"
	"${QDOS}/src/shell/native.c"
	"${QDOS}/src/shell/mathwords.c"
	"${QDOS}/src/shell/complex.c"
	"${QDOS}/src/shell/numeric.c"
	"${QDOS}/src/shell/infix.c"
	"${QDOS}/src/shell/stats.c"
	"${QDOS}/src/shell/statwords.c"
	"${QDOS}/src/shell/decimal.c"
	"${QDOS}/src/hal/sim/keypad_ui.c"
	"${QDOS}/src/hal/sim/padfont.c"
	"${QDOS}/src/ui/console.c"
	"${QDOS}/src/ui/font16x24.c"
	"${QDOS}/src/ui/font8x16.c"
	"${QDOS}/src/ui/splash.c"
	"${QDOS}/src/hal/filestore.c"
	"${QDOS}/src/hal/serial_console.c"
	"${QDOS}/src/hal/storename.c"
	"${QDOS}/src/hal/wallclock.c"
	builtins.c)

target_include_directories(qdos PRIVATE "${QDOS}/include" "${QDOS}/src" "${version_dir}")

target_compile_definitions(qdos PRIVATE
	QDOS_WITH_SIM
	# No dynamic linker worth the name: the native modules are linked in and
	# listed in builtins.c; see src/shell/native.c
	QDOS_NO_DLOPEN)

target_compile_options(qdos PRIVATE --use-port=sdl3 -Wall -Wextra -Wno-format-truncation)

# The system store is the shipped programs, packed beside the wasm, so the
# shell finds them at the path the desktop simulator reads them from
set(system_dir "${QDOS}/programs/system")
file(GLOB_RECURSE system_files CONFIGURE_DEPENDS "${system_dir}/*")
# The keys the page can press, by name, numbered as keys.h numbers them: one
# entry per line of its enum, in order, so a key added there is a key here
set(keys_h "${QDOS}/include/qdos/keys.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${keys_h}")
file(STRINGS "${keys_h}" key_lines REGEX "^\tQDOS_KEY_[A-Z0-9_]+")
set(key_entries "")
set(key_number 0)
foreach(line IN LISTS key_lines)
	string(REGEX MATCH "QDOS_KEY_([A-Z0-9_]+)" _ "${line}")
	set(key_name "${CMAKE_MATCH_1}")
	if(NOT key_name STREQUAL "_COUNT")
		string(APPEND key_entries "\t\"${key_name}\": ${key_number},\n")
		math(EXPR key_number "${key_number} + 1")
	endif()
endforeach()
set(keys_js "${CMAKE_CURRENT_BINARY_DIR}/keys.js")
file(WRITE "${keys_js}" "// Generated from include/qdos/keys.h by web/CMakeLists.txt\nvar QDOS_KEYS = {\n${key_entries}};\n")

set_property(TARGET qdos APPEND PROPERTY LINK_DEPENDS
	${system_files} "${keys_js}" "${CMAKE_CURRENT_LIST_DIR}/pre.js" "${CMAKE_CURRENT_LIST_DIR}/shell.html")

set_target_properties(qdos PROPERTIES SUFFIX ".html")

target_link_libraries(qdos PRIVATE quadrate)
target_link_options(qdos PRIVATE
	--use-port=sdl3
	"--shell-file=${CMAKE_CURRENT_LIST_DIR}/shell.html"
	"--pre-js=${keys_js}"
	"--pre-js=${CMAKE_CURRENT_LIST_DIR}/pre.js"
	"--preload-file=${system_dir}@/programs/system"
	-lidbfs.js
	# The shell is a loop that blocks on its keypad, as on every other
	# backend; Asyncify lets that wait hand the browser its thread back
	-sASYNCIFY
	-sASYNCIFY_STACK_SIZE=65536
	-sSTACK_SIZE=1048576
	-sALLOW_MEMORY_GROWTH
	-sEXIT_RUNTIME=0
	-sEXPORTED_RUNTIME_METHODS=FS)
