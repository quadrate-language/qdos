# link_language: the tests are C, but they link the C++ Quadrate front-end
test_console = executable('test_console', 'test_console.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('console', test_console)

test_shell = executable('test_shell', 'test_shell.c',
		include_directories: qdos_inc,
		dependencies: [qdos_core_dep],
		link_language: 'cpp')
test('shell', test_shell)

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
