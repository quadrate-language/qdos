project('qdos', 'c', 'cpp',
		version: '0.1.0',
		default_options: [
			'c_std=c11',
			'cpp_std=c++20',
			'warning_level=2',
		])

fs = import('fs')

# The about view reports what it is actually running, so the version is taken
# from here and the commit from git, rather than written down twice.
git_commit = 'unknown'
git = find_program('git', required: false)
if git.found()
	described = run_command(git, '-C', meson.current_source_dir(),
			'describe', '--always', '--dirty', check: false)
	if described.returncode() == 0
		git_commit = described.stdout().strip()
	endif
endif

version_h = configure_file(
		output: 'qdos_version.h',
		configuration: configuration_data({
			'QDOS_VERSION': '"' + meson.project_version() + '"',
			'QDOS_COMMIT': '"' + git_commit + '"',
		}))


# ---------------------------------------------------------------------------
# Quadrate
#
# QDOS links the interpreter (lib/interp), the compiler front-end it parses
# with (lib/qc, plus its u8t dependency) and the runtime (lib/rt). None of them
# pull in LLVM: execution goes through the runtime's C operations rather than
# generated code. See README.md.
# ---------------------------------------------------------------------------

quadrate_src = get_option('quadrate_src')
if not quadrate_src.startswith('/')
	quadrate_src = meson.current_source_dir() / quadrate_src
endif
quadrate_dist = quadrate_src / get_option('quadrate_dist')

# Order matters: interp references qc and rt, math and qc reference rt and u8t
quadrate_libs = []
foreach name : ['interp', 'qc', 'math', 'rt', 'u8t']
	quadrate_libs += quadrate_dist / 'lib/quadrate/lib' + name + '.a'
endforeach

foreach lib : quadrate_libs
	if not fs.exists(lib)
		error('\n'.join([
			'Quadrate static library not found:',
			'  ' + lib,
			'',
			'Check out the Quadrate source alongside this project and build it:',
			'  git clone https://github.com/quadrate-language/quadrate',
			'  cd quadrate && make release',
			'',
			'Or point at an existing tree:',
			'  meson setup build -Dquadrate_src=/path/to/quadrate',
		]))
	endif
endforeach

# -isystem rather than include_directories: the Quadrate tree is outside this
# project, which meson refuses to form absolute include paths into, and it also
# keeps its headers from tripping this project's warning level.
quadrate_dep = declare_dependency(
		compile_args: ['-isystem' + (quadrate_dist / 'include')],
		link_args: quadrate_libs + ['-lm'],
)

# ---------------------------------------------------------------------------
# QDOS
# ---------------------------------------------------------------------------

# '.' also brings in the build directory, where qdos_version.h is generated
qdos_inc = include_directories('include', 'src', '.')

# Core: the shell and the display layer — everything that is
# independent of which hardware backend is underneath. Split out as a library
# so tests can link it without dragging in a backend or main().
qdos_core = static_library('qdos_core',
		[
			'src/shell/shell.c',
			'src/shell/storage.c',
			'src/shell/complete.c',
			'src/shell/wordlist.c',
			'src/shell/editor.c',
			'src/shell/mathwords.c',
			version_h,
			'src/hal/sim/keypad_ui.c',
			'src/ui/console.c',
			'src/ui/font16x24.c',
			'src/ui/splash.c',
			'src/hal/device/framebuffer.c',
			'src/hal/device/keypad.c',
		],
		include_directories: qdos_inc,
		dependencies: [quadrate_dep])

qdos_core_dep = declare_dependency(
		link_with: qdos_core,
		include_directories: qdos_inc,
		dependencies: [quadrate_dep])

qdos_sources = ['src/main.c']
qdos_deps = [qdos_core_dep]
qdos_args = []

if get_option('sim')
	qdos_deps += dependency('sdl3', required: true)
	qdos_sources += 'src/hal/sim/sim_sdl3.c'
	qdos_args += '-DQDOS_WITH_SIM'
endif

if get_option('device')
	qdos_sources += 'src/hal/device/device_linux.c'
	qdos_args += '-DQDOS_WITH_DEVICE'
endif

if not get_option('sim') and not get_option('device')
	error('no backend selected: enable at least one of -Dsim or -Ddevice')
endif

qdos_link_args = []
if get_option('static')
	# A firmware rootfs then holds one file: no libc, libstdc++ or libgcc to
	# stage or version-match
	qdos_link_args += '-static'
endif

executable('qdos',
		qdos_sources,
		include_directories: qdos_inc,
		dependencies: qdos_deps,
		c_args: qdos_args,
		cpp_args: qdos_args,
		link_args: qdos_link_args,
		# QDOS is C, but the Quadrate front-end it links is C++ and needs the
		# C++ runtime pulled in at link time
		link_language: 'cpp',
		install: true)

subdir('tests')
