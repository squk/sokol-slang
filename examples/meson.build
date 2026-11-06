# Both examples use the same native window and graphics backend.
example_c_args = []
example_deps = [sokol_dep]
example_available = true
if host_machine.system() == 'darwin'
  example_c_args = ['-DSOKOL_METAL', '-x', 'objective-c']
  example_deps += dependency('appleframeworks', modules: app_frameworks + ['Metal'])
elif host_machine.system() == 'linux'
  example_available = app_available and egl_dep.found() and gl_dep.found()
  example_c_args = ['-DSOKOL_GLCORE']
  example_deps += [egl_dep, gl_dep] + app_deps
elif host_machine.system() == 'windows'
  example_c_args = ['-DSOKOL_D3D11', '-DSOKOL_WIN32_FORCE_MAIN']
  example_deps += [d3d11_dep] + app_deps
else
  example_available = false
endif

foreach example : [['triangle', 'triangle'], ['textured-quad', 'textured_quad']]
  example_name = example[0]
  example_module = example[1]
  subdir(example_name)
endforeach
