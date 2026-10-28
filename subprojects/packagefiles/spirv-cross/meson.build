project('spirv-cross', 'cpp', default_options: ['cpp_std=c++20'])
spirv_cross_inc = include_directories('.')
spirv_cross_lib = static_library('spirv-cross',
  'spirv_cross.cpp', 'spirv_parser.cpp', 'spirv_cross_parsed_ir.cpp', 'spirv_cfg.cpp',
  'spirv_glsl.cpp', 'spirv_msl.cpp', 'spirv_hlsl.cpp', 'spirv_reflect.cpp',
  include_directories: spirv_cross_inc, native: true,
)
spirv_cross_dep = declare_dependency(link_with: spirv_cross_lib, include_directories: spirv_cross_inc)
