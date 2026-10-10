# DOOM, linked into the firmware: the doomgeneric engine and qdos-doom's host,
# found beside this tree as the Pi's build finds them. Empty unless
# CONFIG_QDOS_DOOM; see esp32/README.md.
#
# The same sources and flags as qdos-doom's Makefile builds libdoom.so from.

get_filename_component(top "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(DEFINED ENV{DOOMGENERIC})
	set(dg "$ENV{DOOMGENERIC}")
else()
	set(dg "${top}/doomgeneric")
endif()
if(DEFINED ENV{QDOS_DOOM})
	set(host "$ENV{QDOS_DOOM}")
else()
	set(host "${top}/qdos-doom")
endif()

# One registration either way: the requirements are read before the config is
if(CONFIG_QDOS_DOOM)
	if(NOT EXISTS "${dg}/src/doomgeneric/doomgeneric.c" OR NOT EXISTS "${host}/qdos_doom.c")
		message(FATAL_ERROR "DOOM needs doomgeneric at ${dg} and qdos-doom at ${host}.\n"
			"Check them out beside this project, or set DOOMGENERIC and QDOS_DOOM.")
	endif()
	file(GLOB engine "${dg}/src/doomgeneric/*.c")
	set(srcs ${engine} "${dg}/src/mono.c" "${dg}/src/keyqueue.c" "${host}/qdos_doom.c")
	set(includes "${dg}/src" "${dg}/src/doomgeneric")
	set(fragments "linker.lf")
endif()

idf_component_register(
	SRCS ${srcs}
	PRIV_INCLUDE_DIRS ${includes}
	PRIV_REQUIRES qdos_core
	LDFRAGMENTS ${fragments})

if(NOT CONFIG_QDOS_DOOM)
	return()
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
	DOOMGENERIC_RESX=320
	DOOMGENERIC_RESY=200
	# Every module calls its descriptor qdos_module; linked in, they need names
	qdos_module=qdos_module_doom)

target_compile_options(${COMPONENT_LIB} PRIVATE
	-std=gnu11
	-O2
	# Vanilla DOOM's code, as the Makefile takes it
	-w)

# DOOM leaves through exit(); qdos_doom.c turns that back into a return
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=exit")
