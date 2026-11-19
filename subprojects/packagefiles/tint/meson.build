project('tint', 'cpp', default_options: ['cpp_std=c++20', 'warning_level=0'])
spirv_tools_sp = subproject('spirv-tools')
spirv_tools_dep = spirv_tools_sp.get_variable('spirv_tools_dep')
incs = include_directories('.', 'include')
sources = import('fs').read('sources.txt').strip().split('\n')
flags = ['-DTINT_BUILD_SPV_READER=1', '-DTINT_BUILD_WGSL_WRITER=1']
lib = static_library('tint', sources, include_directories: incs,
  dependencies: spirv_tools_dep, cpp_args: flags, override_options: ['unity=off'])
tint_dep = declare_dependency(link_with: lib, include_directories: incs,
  dependencies: spirv_tools_dep, compile_args: flags)
if meson.is_cross_build()
  spirv_tools_native_dep = spirv_tools_sp.get_variable('spirv_tools_native_dep')
  native_lib = static_library('tint-native', sources, include_directories: incs,
    dependencies: spirv_tools_native_dep, cpp_args: flags, native: true, override_options: ['unity=off'])
  tint_native_dep = declare_dependency(link_with: native_lib, include_directories: incs,
    dependencies: spirv_tools_native_dep, compile_args: flags)
endif
