project('sokol-tools', 'cpp', default_options: ['cpp_std=c++20'])
fmt_sp = subproject('fmt')
pystring_sp = subproject('pystring')
spirv_cross_sp = subproject('spirv-cross')
glslang_dep = subproject('glslang').get_variable('glslang_dep')
shdc_deps = [
  fmt_sp.get_variable('fmt_dep'),
  pystring_sp.get_variable('pystring_dep'),
  spirv_cross_sp.get_variable('spirv_cross_dep'),
  glslang_dep,
]
shdc_inc = include_directories('src/shdc')
shdc_srcs = [
  'src/shdc/input.cc', 'src/shdc/util.cc', 'src/shdc/reflection.cc',
  'src/shdc/generators/generator.cc', 'src/shdc/generators/sokolc.cc',
]
shdc_lib = static_library('sokol-shdc-support', shdc_srcs, include_directories: shdc_inc, dependencies: shdc_deps)
sokol_tools_dep = declare_dependency(link_with: shdc_lib, include_directories: shdc_inc, dependencies: shdc_deps)
sokol_generated_dep = declare_dependency(include_directories: include_directories('ext/generated'))
meson.override_dependency('sokol-generated', sokol_generated_dep)
if meson.is_cross_build()
  shdc_native_deps = [
    fmt_sp.get_variable('fmt_native_dep'),
    pystring_sp.get_variable('pystring_native_dep'),
    spirv_cross_sp.get_variable('spirv_cross_native_dep'),
    glslang_dep,
  ]
  shdc_native_lib = static_library('sokol-shdc-support-native', shdc_srcs, include_directories: shdc_inc, dependencies: shdc_native_deps, native: true)
  sokol_tools_native_dep = declare_dependency(link_with: shdc_native_lib, include_directories: shdc_inc, dependencies: shdc_native_deps)
endif
