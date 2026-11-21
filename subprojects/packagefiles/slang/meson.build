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

# An installed program that links slang_sdk_dep loads this library at run time: from libdir through its
# install rpath (macOS, Linux), or beside the executable (Windows). Install the file the link library
# resolves to, under the name the program records. A cross build's native SDK serves build tools only.
if not meson.is_cross_build()
  if host_machine.system() == 'windows'
    install_data('bin/slang.dll', 'bin/slang-compiler.dll', install_dir: get_option('bindir'))
  else
    sdk_library = host_machine.system() == 'darwin' ? 'libslang.dylib' : 'libslang.so'
    resolved = run_command(find_program('python3'), '-c',
      'import os,sys; print(os.path.basename(os.path.realpath(sys.argv[1])))',
      slang_sdk_root / 'lib' / sdk_library, check: true).stdout().strip()
    install_data('lib' / resolved, install_dir: get_option('libdir'))
  endif
endif
