header = custom_target(example_name + '-shader',
  input: 'shader.slang',
  output: [example_module + '.h', example_module + '.reflection.json'],
  depfile: example_module + '.d',
  depend_files: files('../../tools/sokol-slang'),
  command: slang_command + ['--module', example_module])

if example_available
  executable(example_name + '-app', 'main.c', header,
    c_args: example_c_args, dependencies: example_deps)
endif
