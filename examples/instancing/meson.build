header = custom_target('instancing-shader',
  input: 'shader.slang',
  output: ['instancing.h', 'instancing.reflection.json'],
  depfile: 'instancing.d',
  depend_files: files('../../tools/sokol-slang'),
  command: slang_command + ['--module', 'instancing'])

if example_available
  executable('instancing-app', 'main.c', header,
    c_args: example_c_args, dependencies: example_deps)
endif
