project('tint', 'cpp', default_options: ['cpp_std=c++20', 'warning_level=0'])
spirv_tools_dep = subproject('spirv-tools').get_variable('spirv_tools_dep')
incs = include_directories('.', 'include')
# Source inventory follows sokol-tools be7a968c, fibs.ts.
sources = import('fs').read('sources.txt').strip().split('\n')
flags = ['-DTINT_BUILD_SPV_READER=1', '-DTINT_BUILD_WGSL_WRITER=1']
# Generated tables and source-local names cannot share a translation unit.
lib = static_library('tint', sources, include_directories: incs,
  dependencies: spirv_tools_dep, cpp_args: flags, native: true, override_options: ['unity=off'])
tint_dep = declare_dependency(link_with: lib, include_directories: incs,
  dependencies: spirv_tools_dep, compile_args: flags)
