# The release archive is Quadrate's dist/ directory: static libraries under
# lib/quadrate and headers under include. This declares the ones QDOS links.
project('quadrate', version: '0.7.0', meson_version: '>=0.63.0')

# Order matters: interp references qc and rt, math, decimal and qc reference rt and u8t
quadrate_libs = []
foreach name : ['interp', 'qc', 'math', 'decimal', 'rt', 'u8t']
	quadrate_libs += meson.current_source_dir() / 'lib/quadrate/lib' + name + '.a'
endforeach

quadrate_dep = declare_dependency(
		include_directories: include_directories('include', is_system: true),
		link_args: quadrate_libs + ['-lm'],
		version: meson.project_version())
