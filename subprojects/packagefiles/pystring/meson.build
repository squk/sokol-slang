project('pystring', 'cpp')
pystring_inc = include_directories('.')
pystring_lib = static_library('pystring', 'pystring.cpp', include_directories: pystring_inc)
pystring_dep = declare_dependency(link_with: pystring_lib, include_directories: pystring_inc)
if meson.is_cross_build()
  pystring_native_lib = static_library('pystring-native', 'pystring.cpp', include_directories: pystring_inc, native: true)
  pystring_native_dep = declare_dependency(link_with: pystring_native_lib, include_directories: pystring_inc)
endif
