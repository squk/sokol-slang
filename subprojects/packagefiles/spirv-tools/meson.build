project('spirv-tools', 'cpp', default_options: ['cpp_std=c++20', 'warning_level=0'])
headers = subproject('spirv-headers').get_variable('spirv_headers_dep')
generated = subproject('sokol-tools').get_variable('sokol_generated_dep')
# Source inventory follows sokol-tools be7a968c, fibs.ts.
sources = import('fs').read('sources.txt').strip().split('\n')
incs = include_directories('.', 'include')
# Generated tables and source-local names cannot share a translation unit.
lib = static_library('spirv-tools', sources, include_directories: incs,
  dependencies: [headers, generated], native: true, override_options: ['unity=off'])
spirv_tools_dep = declare_dependency(link_with: lib,
  include_directories: incs, dependencies: [headers, generated])
