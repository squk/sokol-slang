quad_header = custom_target('textured-quad-shader',
  input: 'shader.slang',
  output: ['textured_quad.h', 'textured_quad.reflection.json'],
  depfile: 'textured_quad.d',
  depend_files: files('../../tools/sokol-slang'),
  command: slang_command + ['--module', 'textured_quad'])

quad_c_args = []
quad_deps = [sokol_dep]

if host_machine.system() == 'darwin'
  quad_c_args += ['-DSOKOL_METAL', '-x', 'objective-c']
  quad_deps += dependency('appleframeworks', modules: app_frameworks + ['Metal'])
elif host_machine.system() == 'linux' and app_available
  quad_c_args += ['-DSOKOL_GLCORE']
  quad_deps += [egl_dep, gl_dep] + app_deps
elif host_machine.system() == 'windows'
  quad_c_args += ['-DSOKOL_D3D11', '-DSOKOL_WIN32_FORCE_MAIN']
  quad_deps += [d3d11_dep] + app_deps
endif

if host_machine.system() != 'linux' or app_available
  executable('textured-quad-app', 'main.c', quad_header,
    c_args: quad_c_args,
    dependencies: quad_deps)
endif
