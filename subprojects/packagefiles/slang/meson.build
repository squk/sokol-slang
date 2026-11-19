project('slang-compiler', version: '2026.20')
# Build-tool consumers select the build-machine SDK with a native subproject.
slangc_name = host_machine.system() == 'windows' ? 'slangc.exe' : 'slangc'
if not meson.is_cross_build()
  slangc = find_program(meson.current_source_dir() / 'bin' / slangc_name)
endif

# The SDK owns its include path so parents can consume it as a dependency.
add_languages('cpp', native: false)
slang_sdk_root = meson.current_source_dir()
slang_sdk_dep = declare_dependency(
  include_directories: include_directories('include'),
  dependencies: meson.get_compiler('cpp').find_library('slang', dirs: slang_sdk_root / 'lib'))
