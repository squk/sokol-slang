project('slang-compiler', version: '2026.20')
# This executable runs on the build machine even when the application is cross-built.
slangc_name = build_machine.system() == 'windows' ? 'slangc.exe' : 'slangc'
slangc = find_program(meson.current_source_dir() / 'bin' / slangc_name, native: true)
