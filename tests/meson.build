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
