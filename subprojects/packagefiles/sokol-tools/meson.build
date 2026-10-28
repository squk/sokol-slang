project('sokol-tools', 'cpp', default_options: ['cpp_std=c++20'])
fmt_dep = subproject('fmt').get_variable('fmt_dep')
pystring_dep = subproject('pystring').get_variable('pystring_dep')
spirv_cross_dep = subproject('spirv-cross').get_variable('spirv_cross_dep')
glslang_dep = subproject('glslang').get_variable('glslang_dep')
shdc_deps = [fmt_dep, pystring_dep, spirv_cross_dep, glslang_dep]
shdc_inc = include_directories('src/shdc')
# Reuse Sokol's reflection and C generator with externally supplied SPIR-V.
shdc_lib = static_library('sokol-shdc-support',
  'src/shdc/input.cc', 'src/shdc/util.cc', 'src/shdc/reflection.cc',
  'src/shdc/generators/generator.cc', 'src/shdc/generators/sokolc.cc',
  include_directories: shdc_inc, dependencies: shdc_deps, native: true,
)
sokol_tools_dep = declare_dependency(link_with: shdc_lib,
  include_directories: shdc_inc, dependencies: shdc_deps)
sokol_generated_dep = declare_dependency(include_directories: include_directories('ext/generated'))
