header = custom_target('compute-image-shader',
  input: 'shader.slang',
  output: ['compute_image.h', 'compute_image.reflection.json'],
  depfile: 'compute_image.d',
  depend_files: files('../../tools/sokol-slang'),
  command: slang_command + [
    '--module', 'compute_image',
    '--program', 'generate:computeMain',
    '--program', 'display:vertexMain:fragmentMain',
  ])

if example_available
  executable('compute-image-app', 'main.c', header,
    c_args: example_c_args, dependencies: example_deps)
endif
