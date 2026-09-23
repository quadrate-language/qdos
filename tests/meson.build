# Real shared objects, so what the module tests exercise is the dynamic linker
# and the descriptor a module author writes, not a stand-in for them. 'demo' and
# 'other' are the same module under two names, which is how the scoping is
# checked; the rest are the shapes the loader has to turn away.
module_deps = []
foreach name : ['demo', 'other']
	module_deps += shared_library(name, 'module_demo.c', include_directories: qdos_inc)
endforeach

foreach bad : ['magic', 'abi', 'arch', 'words', 'open']
	module_deps += shared_library('bad' + bad, 'module_bad.c',
			include_directories: qdos_inc,
			c_args: ['-DBAD_' + bad.to_upper()])
endforeach

# One with no descriptor at all, which is what any other .so on the card is
module_deps += shared_library('nosym', 'module_bad.c',
		include_directories: qdos_inc,
		c_args: ['-DNO_SYMBOL'])

# A program rather than a library: it has a main, and takes its bare name
module_deps += shared_library('app', 'module_app.c', include_directories: qdos_inc)

# link_language: the tests are C, but they link the C++ Quadrate front-end
test_console = executable('test_console', 'test_console.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('console', test_console)

test_shell = executable('test_shell', 'test_shell.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		c_args: ['-DMODULE_DIR="' + meson.current_build_dir() + '"'],
		link_language: 'cpp')
test('shell', test_shell, depends: module_deps)

test_framebuffer = executable('test_framebuffer', 'test_framebuffer.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('framebuffer', test_framebuffer)

test_keypad = executable('test_keypad', 'test_keypad.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('keypad', test_keypad)

test_storage = executable('test_storage', 'test_storage.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('storage', test_storage)

test_complete = executable('test_complete', 'test_complete.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('complete', test_complete)

test_padui = executable('test_padui', 'test_padui.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('padui', test_padui)

test_editor = executable('test_editor', 'test_editor.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('editor', test_editor)

test_mathwords = executable('test_mathwords', 'test_mathwords.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('mathwords', test_mathwords)

test_native = executable('test_native', 'test_native.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		c_args: ['-DMODULE_DIR="' + meson.current_build_dir() + '"'],
		link_language: 'cpp')
test('native', test_native, depends: module_deps)

# The simulator backend is only built where the simulator is, and it needs SDL
# and a video driver -- the dummy one, since this never shows a window.
if get_option('sim')
	test_simhal = executable('test_simhal', ['test_simhal.c', '../src/hal/sim/sim_sdl3.c'],
			include_directories: qdos_inc,
			dependencies: [qdos_core_dep, dependency('sdl3')],
			link_language: 'cpp')
	test('simhal', test_simhal, env: {'SDL_VIDEODRIVER': 'dummy'}, timeout: 30)
endif

test_lint = executable('test_lint', 'test_lint.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('lint', test_lint)

test_guarded = executable('test_guarded', 'test_guarded.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('guarded', test_guarded)

# The font's source is a bitmap, so nothing checks its metrics at compile time.
# A glyph a pixel off the baseline is invisible on its own and obvious in a
# word, which is exactly the kind of thing to let a machine notice.
python = find_program('python3', required: false)
if python.found()
	test('font', python,
			args: [meson.project_source_root() / 'tools/genfont.py', '--check'],
			workdir: meson.project_source_root())
	test('smallfont', python,
			args: [meson.project_source_root() / 'tools/gensmallfont.py', '--check'],
			workdir: meson.project_source_root())
endif

# The store half of the device backend is plain file handling, so it is tested
# wherever the backend is built rather than only on the hardware
if get_option('device')
	test_device_store = executable('test_device_store', 'test_device_store.c',
			include_directories: qdos_inc,
			dependencies: [qdos_device_dep],
			link_language: 'cpp')
	test('device_store', test_device_store)
endif
