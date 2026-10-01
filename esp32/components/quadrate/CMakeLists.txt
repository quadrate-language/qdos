# Quadrate, built from its own source tree as an ESP-IDF component.
#
# The same libraries the Pi links -- the runtime, the compiler front-end, the
# AST interpreter, u8t and the math module -- compiled here rather than taken
# from dist/, because the archives `make release` produces are for the host.
# Meson is Quadrate's build and stays its build; this file only lists sources.
#
# The tree is found at QUADRATE_SRC, which defaults to a checkout beside this
# one, as the Meson build expects.

if(DEFINED ENV{QUADRATE_SRC})
	set(QD "$ENV{QUADRATE_SRC}")
else()
	get_filename_component(QD "${CMAKE_CURRENT_LIST_DIR}/../../../../quadrate" ABSOLUTE)
endif()

if(NOT EXISTS "${QD}/lib/interp/src/interp.cc")
	message(FATAL_ERROR "Quadrate source tree not found at ${QD}.\n"
		"Check it out beside this project, or set QUADRATE_SRC.")
endif()
if(NOT EXISTS "${QD}/subprojects/u8t/src/scanner.c")
	message(FATAL_ERROR "u8t is missing from ${QD}/subprojects.\n"
		"Run `meson subprojects download` in the Quadrate tree, or build it once.")
endif()

set(rt "${QD}/lib/rt/src")
set(qc "${QD}/lib/qc/src")

idf_component_register(
	SRCS
		# lib/rt, with the platform layer for a target that has no processes
		# no dynamic linker and no C11 threads
		"${rt}/natives.c"
		"${rt}/runtime.c"
		"${rt}/runtime_stack.c"
		"${rt}/runtime_ops.c"
		"${rt}/runtime_io.c"
		"${rt}/stack.c"
		"${rt}/memory.c"
		"${rt}/qd_string.c"
		"${rt}/qd_struct.c"
		"${rt}/array.c"
		"${rt}/platform/pthread/thread_pthread.c"
		"${rt}/platform/pthread/mutex_pthread.c"
		"${rt}/platform/none/dynlib_none.c"
		"${rt}/platform/none/process_none.c"
		"${rt}/platform/none/exe_path_none.c"
		# lib/qc
		"${qc}/ast.cc"
		"${qc}/ast_statements.cc"
		"${qc}/ast_expressions.cc"
		"${qc}/ast_declarations.cc"
		"${qc}/ast_types.cc"
		"${qc}/ast_printer.cc"
		"${qc}/colors.cc"
		"${qc}/error_reporter.cc"
		"${qc}/source_formatter.cc"
		"${qc}/semantic_validator.cc"
		"${qc}/semantic_validator_modules.cc"
		"${qc}/semantic_validator_collect.cc"
		"${qc}/semantic_validator_typecheck.cc"
		"${qc}/semantic_validator_instructions.cc"
		# lib/interp
		"${QD}/lib/interp/src/interp.cc"
		# u8t
		"${QD}/subprojects/u8t/src/scanner.c"
		# stdlib/math
		"${QD}/stdlib/math/src/math.c"
	INCLUDE_DIRS
		# newlib's gaps, filled for u8t's public header
		"compat"
		"${QD}/lib/rt/include"
		"${QD}/lib/qc/include"
		"${QD}/lib/interp/include"
		"${QD}/subprojects/u8t/include"
		"${QD}/stdlib/math/include"
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

# The version the runtime reports, from the tags as Meson takes it
execute_process(
	COMMAND git -C "${QD}" describe --tags --abbrev=0
	OUTPUT_VARIABLE qd_version
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET)
if(NOT qd_version MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
	set(qd_version "0.0.0-unknown")
endif()
string(REGEX MATCH "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${qd_version}")
set(qd_major "${CMAKE_MATCH_1}")
set(qd_minor "${CMAKE_MATCH_2}")
set(qd_patch "${CMAKE_MATCH_3}")
math(EXPR qd_api "${qd_major} * 10000 + ${qd_minor} * 100 + ${qd_patch}")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
	QD_VERSION="${qd_version}"
	QD_VERSION_MAJOR=${qd_major}
	QD_VERSION_MINOR=${qd_minor}
	QD_VERSION_PATCH=${qd_patch}
	QD_VERSION_API=${qd_api}
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
