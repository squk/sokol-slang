project('fmt', 'cpp', default_options: ['cpp_std=c++20'])
fmt_inc = include_directories('include')
fmt_lib = static_library('fmt', 'src/format.cc', include_directories: fmt_inc, native: true)
fmt_dep = declare_dependency(link_with: fmt_lib, include_directories: fmt_inc)
