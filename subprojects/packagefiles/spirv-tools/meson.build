project('spirv-tools', 'cpp', default_options: ['cpp_std=c++20', 'warning_level=0'])
headers = subproject('spirv-headers').get_variable('spirv_headers_dep')
generated = dependency('sokol-generated')
sources = import('fs').read('sources.txt').strip().split('\n')
incs = include_directories('.', 'include')
lib = static_library('spirv-tools', sources, include_directories: incs,
  dependencies: [headers, generated], override_options: ['unity=off'])
spirv_tools_dep = declare_dependency(link_with: lib,
  include_directories: incs, dependencies: [headers, generated])
if meson.is_cross_build()
  native_lib = static_library('spirv-tools-native', sources, include_directories: incs,
    dependencies: [headers, generated], native: true, override_options: ['unity=off'])
  spirv_tools_native_dep = declare_dependency(link_with: native_lib,
    include_directories: incs, dependencies: [headers, generated])
endif
