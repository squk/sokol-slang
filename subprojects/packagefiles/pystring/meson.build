project('pystring', 'cpp')
pystring_inc = include_directories('.')
pystring_lib = static_library('pystring', 'pystring.cpp', include_directories: pystring_inc, native: true)
pystring_dep = declare_dependency(link_with: pystring_lib, include_directories: pystring_inc)
