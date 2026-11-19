project('fmt', 'cpp', default_options: ['cpp_std=c++20'])
fmt_inc = include_directories('include')
fmt_lib = static_library('fmt', 'src/format.cc', include_directories: fmt_inc)
fmt_dep = declare_dependency(link_with: fmt_lib, include_directories: fmt_inc)
if meson.is_cross_build()
  fmt_native_lib = static_library('fmt-native', 'src/format.cc', include_directories: fmt_inc, native: true)
  fmt_native_dep = declare_dependency(link_with: fmt_native_lib, include_directories: fmt_inc)
endif
