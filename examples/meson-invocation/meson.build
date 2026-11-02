project('sokol-slang-meson-example', 'c', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++20', 'c_std=c11'])

python = import('python').find_installation('python3')

# ----------------------------------------------------------------------
# Approach 1: Consuming sokol-slang as a Meson subproject (recommended)
# ----------------------------------------------------------------------
# When sokol-slang is vendored or tracked as a wrap in subprojects/:
#
#   sokol_slang_proj = subproject('sokol-slang')
#   core = sokol_slang_proj.get_variable('core')
#   slang_frontend = sokol_slang_proj.get_variable('slang_frontend')
#   slangc = sokol_slang_proj.get_variable('slangc')
#
#   shader_header = custom_target('my-shader',
#     input: ['shader.slang', 'metadata.json'],
#     output: ['shader.h', 'shader.reflection.json'],
#     depfile: 'shader.d',
#     command: [
#       python, slang_frontend,
#       '--slangc', slangc,
#       '--core', core,
#       '--input', '@INPUT0@',
#       '--metadata', '@INPUT1@',
#       '--output', '@OUTPUT0@',
#       '--reflection', '@OUTPUT1@',
#       '--depfile', '@DEPFILE@'
#     ]
#   )

# ----------------------------------------------------------------------
# Approach 2: Using an installed sokol-slang executable from PATH
# ----------------------------------------------------------------------
sokol_slang = find_program('sokol-slang', required: false)

if sokol_slang.found()
  compiled_shader = custom_target('compiled-shader',
    input: ['shader.slang', 'metadata.json'],
    output: ['shader.h', 'shader.reflection.json'],
    depfile: 'shader.d',
    command: [
      sokol_slang,
      '--input', '@INPUT0@',
      '--metadata', '@INPUT1@',
      '--output', '@OUTPUT0@',
      '--reflection', '@OUTPUT1@',
      '--depfile', '@DEPFILE@'
    ]
  )
endif
