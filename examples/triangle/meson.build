# examples/triangle/meson.build

triangle_header = custom_target('triangle-shader',
  input: 'shader.slang',
  output: ['triangle.h', 'triangle.reflection.json'],
  depfile: 'triangle.d',
  depend_files: files('../../tools/sokol-slang'),
  command: slang_command + ['--module', 'triangle'])

triangle_c_args = []
triangle_deps = [sokol_dep]

if host_machine.system() == 'darwin'
  triangle_c_args += ['-DSOKOL_METAL', '-x', 'objective-c']
  triangle_deps += dependency('appleframeworks', modules: app_frameworks + ['Metal'])
elif host_machine.system() == 'linux' and app_available
  triangle_c_args += ['-DSOKOL_GLCORE']
  triangle_deps += [egl_dep, gl_dep] + app_deps
elif host_machine.system() == 'windows'
  triangle_c_args += ['-DSOKOL_D3D11', '-DSOKOL_WIN32_FORCE_MAIN']
  triangle_deps += [d3d11_dep] + app_deps
endif

if host_machine.system() != 'linux' or app_available
  executable('triangle-app', 'main.c', triangle_header,
    c_args: triangle_c_args,
    dependencies: triangle_deps)
endif
