project('slang-compiler', version: '2026.20')
# This executable runs on the build machine even when the application is cross-built.
slangc_name = build_machine.system() == 'windows' ? 'slangc.exe' : 'slangc'
slangc = find_program(meson.current_source_dir() / 'bin' / slangc_name, native: true)

# The SDK owns its include path so parents can consume it as a dependency.
add_languages('cpp', native: true)
slang_sdk_root = meson.current_source_dir()
slang_sdk_dep = declare_dependency(
  include_directories: include_directories('include'),
  dependencies: meson.get_compiler('cpp', native: true).find_library('slang', dirs: slang_sdk_root / 'lib'))
