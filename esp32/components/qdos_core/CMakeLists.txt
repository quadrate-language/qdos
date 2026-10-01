# The QDOS core -- the shell and the display layer -- as an ESP-IDF component.
#
# The same sources as qdos_core in meson.build, less the two that read Linux
# devices (the evdev keypad and the fbdev blit). Keep the lists in step.

get_filename_component(QDOS "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# The about view reports what it is running; the version comes from
# meson.build, as there, and the commit from git
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

idf_component_register(
	SRCS
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
		"${QDOS}/src/hal/sim/keypad_ui.c"
		"${QDOS}/src/hal/sim/padfont.c"
		"${QDOS}/src/ui/console.c"
		"${QDOS}/src/ui/font16x24.c"
		"${QDOS}/src/ui/font8x16.c"
		"${QDOS}/src/ui/splash.c"
		"${QDOS}/src/hal/filestore.c"
		"${QDOS}/src/hal/storename.c"
		"${QDOS}/src/hal/wallclock.c"
	INCLUDE_DIRS
		"${QDOS}/include"
		"${QDOS}/src"
		"${version_dir}"
	REQUIRES quadrate
	LDFRAGMENTS "linker.lf")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
	# No dynamic linker, so no uploaded native modules; see src/shell/native.c
	QDOS_NO_DLOPEN
	# No pipe or dup2 to catch what print writes; see src/shell/guarded.c
	QDOS_CAPTURE_STDIO)

target_compile_options(${COMPONENT_LIB} PRIVATE
	$<$<COMPILE_LANGUAGE:C>:-std=gnu11>
	$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
	# The Meson build's warning level, not IDF's -Werror=all on top of it. The
	# shell truncates into fixed cells on purpose, which GCC at -Os reports.
	-Wno-error
	-Wno-format-truncation)
