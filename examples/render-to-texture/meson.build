header = custom_target('render-to-texture-shader',
  input: 'shader.slang',
  output: ['render_to_texture.h', 'render_to_texture.reflection.json'],
  depfile: 'render_to_texture.d',
  depend_files: files('../../tools/sokol-slang'),
  command: slang_command + [
    '--module', 'render_to_texture',
    '--program', 'pattern:vertexMain:patternFragment',
    '--program', 'display:vertexMain:displayFragment',
  ])

if example_available
  executable('render-to-texture-app', 'main.c', header,
    c_args: example_c_args, dependencies: example_deps)
endif
