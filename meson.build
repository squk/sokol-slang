# Set the SDK before Meson checks compilers. host_machine is not available
# until project() runs, so Python identifies the build OS here. Cross builds
# keep their toolchain SDK; explicit Meson compiler options override these defaults.
project('sokol-slang', ['c', 'cpp'], version: '0.1.0', meson_version: '>=1.3',
  default_options: ['c_std=c11', 'cpp_std=c++20', 'build.cpp_std=c++20', 'warning_level=1'] + (
    run_command('python3', '-c', 'import sys; print(sys.platform)', check: true).stdout().strip() == 'darwin'
    and not meson.is_cross_build() ? [
      'c_args=-isysroot "' + run_command('xcrun', '--sdk', 'macosx', '--show-sdk-path', check: true).stdout().strip() + '"',
      'c_link_args=-isysroot "' + run_command('xcrun', '--sdk', 'macosx', '--show-sdk-path', check: true).stdout().strip() + '"',
      'cpp_args=-isysroot "' + run_command('xcrun', '--sdk', 'macosx', '--show-sdk-path', check: true).stdout().strip() + '"',
      'cpp_link_args=-isysroot "' + run_command('xcrun', '--sdk', 'macosx', '--show-sdk-path', check: true).stdout().strip() + '"',
    ] : []
  ),
)
python = import('python').find_installation('python3')
fs = import('fs')
# iOS and simulator cross files also use Darwin; only macOS has CGL.
macos_host = host_machine.system() == 'darwin' and meson.get_compiler('cpp').get_define('__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__') != ''
sokol_tools_dep = subproject('sokol-tools').get_variable('sokol_tools_dep')
sokol_dep = subproject('sokol').get_variable('sokol_dep')
tint_dep = subproject('tint').get_variable('tint_dep')
json_dep = subproject('json').get_variable('json_dep')
manifest_schema = custom_target('manifest-schema', input: 'schema/manifest.schema.json',
  output: 'manifest_schema.h', command: [python, files('tools/embed_schema.py'), '@INPUT@', '@OUTPUT@'])
compiler_threads_dep = dependency('threads', native: true)
backend_compiler = static_library('sokol-slang-backend', 'src/backend_compile.cc',
  'src/compiler_stack.cc',
  'src/manifest.cc', 'src/program_bindings.cc', 'src/native_bytecode.cc', manifest_schema,
  dependencies: [sokol_tools_dep, sokol_dep.partial_dependency(includes: true), tint_dep, json_dep, compiler_threads_dep],
  native: true)
backend_compiler_dep = declare_dependency(link_with: backend_compiler,
  include_directories: include_directories('src'),
  dependencies: [sokol_tools_dep, sokol_dep.partial_dependency(includes: true), tint_dep, json_dep, compiler_threads_dep])


# Export build-machine tools to consumers without adding rendering fixtures.
# The compiler reads Sokol declarations; it must not link the parent's runtime.
slang_frontend = files('tools/sokol-slang')
if get_option('slang')
  if get_option('slangc') != ''
    slangc = find_program(get_option('slangc'), native: true)
    if get_option('compiler_library')
      slang_sdk_root = fs.parent(fs.parent(slangc.full_path()))
      slang_sdk_dep = declare_dependency(
        include_directories: include_directories(slang_sdk_root / 'include'),
        dependencies: meson.get_compiler('cpp', native: true).find_library('slang', dirs: slang_sdk_root / 'lib'))
    endif
  else
    slang_platform = build_machine.system() == 'darwin' ? 'macos' : build_machine.system()
    slang_cpu = build_machine.cpu_family()
    if not ['macos', 'linux', 'windows'].contains(slang_platform) or not ['x86_64', 'aarch64'].contains(slang_cpu)
      error('No pinned Slang package for this build machine. Set -Dslangc=/path/to/slangc (2026.20).')
    endif
    slang_sdk = subproject('slang-' + slang_platform + '-' + slang_cpu)
    slangc = slang_sdk.get_variable('slangc')
    slang_sdk_dep = slang_sdk.get_variable('slang_sdk_dep')
  endif
endif
# The CLI and native editor/tool callers share the build-machine Slang library.
# Cross-built players consume cooked data and never link this dependency.
if get_option('compiler_library') and get_option('slang')
  source_compiler = static_library('sokol-slang-source', 'src/slang_compile.cc',
    dependencies: slang_sdk_dep, native: true)
  source_compiler_dep = declare_dependency(link_with: source_compiler,
    include_directories: include_directories('src'), dependencies: slang_sdk_dep)
  program_compiler = static_library('sokol-slang-program', 'src/program_compile.cc',
    dependencies: [source_compiler_dep, backend_compiler_dep], native: true)
  program_compiler_dep = declare_dependency(link_with: program_compiler,
    include_directories: include_directories('src'),
    dependencies: [source_compiler_dep, backend_compiler_dep])
endif
core_dependencies = [backend_compiler_dep]
core_defines = []
if get_option('compiler_library') and get_option('slang')
  core_dependencies += source_compiler_dep
  core_defines += '-DSOKOL_SLANG_SOURCE_COMPILER=1'
endif
core = executable('sokol-spirv', 'src/sokol_spirv.cc', dependencies: core_dependencies,
  cpp_args: core_defines, native: true,
  install: not meson.is_subproject() and not meson.is_cross_build())

if meson.is_subproject()
  subdir_done()
endif

core_command = [core, '--manifest', '@INPUT0@', '--output', '@OUTPUT0@',
  '--reflection', '@OUTPUT1@', '--depfile', '@DEPFILE@']
if get_option('slang')
  slang_command = [python, slang_frontend, '--core', core,
    '--input', '@INPUT0@', '--output', '@OUTPUT0@',
    '--reflection', '@OUTPUT1@', '--depfile', '@DEPFILE@']
endif

test_source = 'tests/test_app.cc'
native_bytecode_args = []
if build_machine.system() == 'darwin'
  native_bytecode_args = ['-DEXPECT_METAL_BYTECODE']
elif build_machine.system() == 'windows'
  native_bytecode_args = ['-DEXPECT_DXBC_BYTECODE']
endif
if host_machine.system() == 'darwin'
  app_frameworks = macos_host ? ['Cocoa', 'QuartzCore'] : ['Foundation', 'UIKit', 'QuartzCore', 'CoreGraphics']
endif

test('frontend-bindings', python, args: files('tests/test_frontend_bindings.py'))

compiler_stack_test = executable('test-compiler-stack',
  ['tests/test_compiler_stack.cc', 'src/compiler_stack.cc'],
  include_directories: include_directories('src'),
  dependencies: compiler_threads_dep, native: true)
test('compiler-stack', compiler_stack_test, suite: 'compiler-library')

if get_option('compiler_library') and get_option('slang')
  program_compile_test = executable('test-program-compile', 'tests/test_program_compile.cc',
    dependencies: program_compiler_dep, native: true)
  test('program-compile', program_compile_test, suite: 'compiler-library', timeout: 60)
  source_declarations_test = executable('test-source-declarations', 'tests/test_source_declarations.cc',
    dependencies: source_compiler_dep, native: true)
  test('source-declarations', source_declarations_test, suite: 'compiler-library', timeout: 60)
endif

if host_machine.system() == 'windows'
  d3d11_dep = meson.get_compiler('cpp').find_library('d3d11')
  app_deps = [meson.get_compiler('cpp').find_library('user32'),
    meson.get_compiler('cpp').find_library('shell32'), meson.get_compiler('cpp').find_library('dxgi'),
    meson.get_compiler('cpp').find_library('gdi32')]
  d3dcompiler_deps = [meson.get_compiler('cpp').find_library('d3dcompiler'),
    meson.get_compiler('cpp').find_library('dxguid')]
endif

if host_machine.system() == 'linux'
  app_deps = [dependency('x11', required: false), dependency('xi', required: false),
    dependency('xcursor', required: false), dependency('threads', required: false),
    meson.get_compiler('cpp').find_library('dl', required: false),
    meson.get_compiler('cpp').find_library('m', required: false)]
  app_available = true
  foreach dep : app_deps
    app_available = app_available and dep.found()
  endforeach
  if not app_available
    warning('Sokol app tests disabled: install X11, Xi, and Xcursor development packages.')
  endif
  egl_dep = dependency('egl', required: false)
  gl_dep = dependency('gl', required: false)
  gles_dep = dependency('glesv2', required: false)
  linux_gl_targets = [
    ['glsl410', 'gl-proof', gl_dep, false], ['glsl430', 'gl43-proof', gl_dep, false],
    ['glsl300es', 'gles-proof', gles_dep, true], ['glsl310es', 'gles31-proof', gles_dep, true],
  ]
  if not egl_dep.found() or not gl_dep.found() or not gles_dep.found()
    warning('Install EGL, OpenGL, and GLES development packages to enable all native Linux rendering tests.')
  endif
endif

if host_machine.system() == 'emscripten'
  browser = find_program(get_option('browser'), native: true, required: true)
  node = find_program('node', native: true)
  browser_link_args = ['-fexceptions',
    '-sEXIT_RUNTIME=0', '-sENVIRONMENT=web', '-sALLOW_MEMORY_GROWTH=1', '-sGROWABLE_ARRAYBUFFERS=0',
    '--shell-file', meson.current_source_dir() / 'tests/browser/shell.html']
  webgl_link_args = browser_link_args + ['-sMIN_WEBGL_VERSION=2', '-sMAX_WEBGL_VERSION=2']
  webgpu_args = ['--use-port=emdawnwebgpu']
endif

compute_fixtures = []
portable_fixtures = []

if get_option('slang')
  install_data('tools/sokol-slang', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')

  proof_header = custom_target('proof-shader',
    input: 'tests/shaders/proof.slang',
    output: ['proof.h', 'proof.reflection.json'],
    depfile: 'proof.d',
    depend_files: files('tools/sokol-slang'),
    command: slang_command + ['--targets', 'metal_macos:metal_ios:metal_sim'],
  )
  if host_machine.system() == 'darwin'
    proof = executable('metal-proof', test_source, proof_header,
      cpp_args: ['-x', 'objective-c++', '-DSOKOL_METAL', '-DTEST_LAYOUT'],
      dependencies: [sokol_dep, dependency('appleframeworks', modules: app_frameworks + ['Metal'])])
    test('metal-proof', proof)
  endif

  test('compiler-contract', python, args: [files('tests/test_compiler.py'), '--core', core.full_path()], depends: core, timeout: 60)

  compute_header = custom_target('compute-shader', input: 'tests/fixtures/compute/shader.slang',
    output: ['compute.h', 'compute.reflection.json'], depfile: 'compute.d',
    depend_files: files('tools/sokol-slang'),
    command: slang_command + ['--module', 'compute'])
  compute_fixtures += [['', 'compute.h', compute_header]]

  portable_header = custom_target('portable-shader',
    input: 'tests/fixtures/portable/shader.slang',
    build_by_default: true, output: ['portable.h', 'portable.reflection.json'], depfile: 'portable.d',
    depend_files: files('tools/sokol-slang'),
    command: slang_command + ['--module', 'portable'])
  portable_descriptors = executable('portable-descriptors', 'tests/descriptors.cc', portable_header,
    cpp_args: ['-DSHADER_HEADER="portable.h"'] + native_bytecode_args, dependencies: sokol_dep)
  test('portable-descriptors', portable_descriptors)
  portable_fixtures += [['', 'portable', portable_header,
    files('tests/fixtures/portable/shader.slang'), slang_command + ['--module', 'portable'],
    files('tools/sokol-slang')]]

  subdir('examples')

endif

# GLSL fixtures exercise the core independently of the optional Slang frontend.
glslang = find_program('glslangValidator', native: true, required: false)
if glslang.found()
  generic_compute_spirv = custom_target('generic-compute-spirv', input: 'tests/fixtures/compute/glsl/shader.comp',
    output: 'generic_compute.spv', command: [glslang, '-V', '--target-env', 'vulkan1.1', '@INPUT@', '-o', '@OUTPUT@'])
  generic_compute_manifest = configure_file(input: 'tests/fixtures/compute/glsl/manifest.json', output: 'generic_compute_manifest.json', copy: true)
  generic_compute_header = custom_target('generic-compute-shader', input: [generic_compute_manifest, generic_compute_spirv],
    output: ['generic_compute.h', 'generic_compute.reflection.json'], depfile: 'generic_compute.d',
    command: core_command)
  compute_fixtures += [['generic-', 'generic_compute.h', generic_compute_header]]
  test('compute-contract', python, args: [files('tests/test_compute.py'), '--core', core.full_path(),
    '--glslang', glslang.full_path()], depends: core, timeout: 120)
  generic_vertex = custom_target('generic-vertex', input: 'tests/fixtures/portable/glsl/vertex.vert',
    output: 'generic_vertex.spv', command: [glslang, '-V', '--target-env', 'vulkan1.1', '@INPUT@', '-o', '@OUTPUT@'])
  generic_fragment = custom_target('generic-fragment', input: 'tests/fixtures/portable/glsl/fragment.frag',
    output: 'generic_fragment.spv', command: [glslang, '-V', '--target-env', 'vulkan1.1', '@INPUT@', '-o', '@OUTPUT@'])
  generic_manifest = configure_file(input: 'tests/fixtures/portable/glsl/manifest.json', output: 'generic_manifest.json', copy: true)
  generic_header = custom_target('generic-shader', input: [generic_manifest, generic_vertex, generic_fragment],
    output: ['generic.h', 'generic.reflection.json'], depfile: 'generic.d',
    command: core_command)
  if build_machine.system() in ['darwin', 'windows']
    test('native-bytecode', python, args: [files('tests/test_native_bytecode.py'),
      '--core', core.full_path(), '--manifest', generic_manifest],
      depends: [core, generic_vertex, generic_fragment], timeout: 120)
  endif
  generic_descriptors = executable('generic-descriptors', 'tests/descriptors.cc', generic_header,
    cpp_args: ['-DSHADER_HEADER="generic.h"'] + native_bytecode_args, dependencies: sokol_dep)
  test('generic-descriptors', generic_descriptors, suite: 'core-smoke')
  program_set_manifest = configure_file(input: 'tests/fixtures/program_set.json', output: 'program_set.json', copy: true)
  program_set_header = custom_target('program-set-shader',
    input: [program_set_manifest, generic_vertex, generic_fragment, generic_compute_spirv],
    output: ['program_set.h', 'program_set.reflection.json'], depfile: 'program_set.d',
    command: core_command)
  program_set_descriptors = executable('program-set-descriptors', 'tests/program_set_descriptors.cc', program_set_header,
    dependencies: sokol_dep)
  test('program-set-descriptors', program_set_descriptors)
  test('program-set-contract', python, args: [files('tests/test_program_sets.py'),
    '--core', core.full_path(), '--manifest', program_set_manifest],
    depends: [core, generic_vertex, generic_fragment, generic_compute_spirv], timeout: 120)
  portable_fixtures += [['generic-', 'generic', generic_header,
    [generic_manifest, generic_vertex, generic_fragment], core_command, []]]
  test('core-contract', python, args: [files('tests/test_core.py'), '--core', core.full_path(),
    '--glslang', glslang.full_path()], depends: core, timeout: 120)
  test('texture-type-contract', python, args: [files('tests/test_texture_types.py'), '--core', core.full_path(),
    '--glslang', glslang.full_path()], depends: core, timeout: 120)
else
  warning('glslangValidator not found: independent GLSL fixtures and core-contract tests are disabled')
endif

# Slang and GLSL fixtures share the same backend application and pixel oracle.
foreach fixture : compute_fixtures
  prefix = fixture[0]
  header_name = fixture[1]
  header = fixture[2]
  fixture_args = ['-DTEST_COMPUTE', '-DSHADER_HEADER="' + header_name + '"']
  core_suite = prefix == 'generic-' ? ['core-smoke'] : []
  if host_machine.system() == 'darwin'
    name = prefix + 'metal-compute'
    proof = executable(name, test_source, header,
      cpp_args: ['-x', 'objective-c++', '-DSOKOL_METAL'] + fixture_args,
      dependencies: [sokol_dep, dependency('appleframeworks', modules: app_frameworks + ['Metal'])])
    test(name, proof, suite: core_suite)
  elif host_machine.system() == 'windows'
    name = prefix + 'd3d11-compute'
    proof = executable(name, test_source, header,
      cpp_args: ['-DSOKOL_D3D11', '-DSOKOL_WIN32_FORCE_MAIN'] + fixture_args,
      dependencies: [sokol_dep, d3d11_dep] + app_deps)
    test(name, proof, suite: core_suite)
    name = prefix + 'hlsl-compute-compile'
    compiler = executable(name, 'tests/hlsl_compute_compile.cc', header,
      cpp_args: ['-DSHADER_HEADER="' + header_name + '"'], dependencies: [sokol_dep] + d3dcompiler_deps)
    test(name, compiler)
  elif host_machine.system() == 'emscripten'
    name = prefix + 'webgpu-compute'
    proof = executable(name, test_source, header,
      name_suffix: 'html', cpp_args: ['-DSOKOL_WGPU', '-fexceptions'] + fixture_args + webgpu_args,
      link_args: browser_link_args + webgpu_args, dependencies: sokol_dep,
      link_depends: files('tests/browser/shell.html'))
    test(name, node, args: [files('tests/browser/run.mjs'), browser.full_path(), proof.full_path()],
      depends: proof, timeout: 90, suite: 'browser')
  elif host_machine.system() == 'linux' and app_available and egl_dep.found()
    foreach target : [['gl', gl_dep, false], ['gles', gles_dep, true]]
      if target[1].found()
        name = prefix + target[0] + '-compute'
        proof = executable(name, test_source, header,
          cpp_args: fixture_args + (target[2] ? ['-DSOKOL_GLES3'] : ['-DSOKOL_GLCORE']),
          dependencies: [sokol_dep, egl_dep, target[1]] + app_deps)
        test(name, proof, suite: core_suite)
      endif
    endforeach
  endif
endforeach

foreach fixture : portable_fixtures
  prefix = fixture[0]
  base = fixture[1]
  header = fixture[2]
  fixture_args = ['-DSHADER_HEADER="' + base + '.h"']
  core_suite = prefix == 'generic-' ? ['core-smoke'] : []
  if host_machine.system() == 'windows'
    name = prefix + 'd3d11-proof'
    proof = executable(name, test_source, header,
      cpp_args: ['-DSOKOL_D3D11', '-DSOKOL_WIN32_FORCE_MAIN'] + fixture_args,
      dependencies: [sokol_dep, d3d11_dep] + app_deps)
    test(name, proof, suite: core_suite)
    hlsl4_header = custom_target(base + '-hlsl4-shader',
      input: fixture[3], depend_files: fixture[5],
      output: [base + '_hlsl4.h', base + '_hlsl4.reflection.json'], depfile: base + '_hlsl4.d',
      command: fixture[4] + ['--targets', 'hlsl4'])
    name = prefix + 'd3d11-hlsl4-proof'
    proof = executable(name, test_source, hlsl4_header,
      cpp_args: ['-DSOKOL_D3D11', '-DSOKOL_WIN32_FORCE_MAIN', '-DSHADER_HEADER="' + base + '_hlsl4.h"'],
      dependencies: [sokol_dep, d3d11_dep] + app_deps)
    test(name, proof)
    foreach entry : [['hlsl-compile', base + '.h', header], ['hlsl4-compile', base + '_hlsl4.h', hlsl4_header]]
      name = prefix + entry[0]
      compiler = executable(name, 'tests/hlsl_compile.cc', entry[2],
        cpp_args: ['-DSHADER_HEADER="' + entry[1] + '"'], dependencies: [sokol_dep] + d3dcompiler_deps)
      test(name, compiler)
    endforeach
  elif macos_host
    name = prefix + 'gl-proof'
    proof = executable(name, test_source, header,
      cpp_args: ['-x', 'objective-c++', '-DSOKOL_GLCORE'] + fixture_args,
      dependencies: [sokol_dep, dependency('appleframeworks', modules: app_frameworks + ['OpenGL'])])
    test(name, proof, suite: core_suite)
  elif host_machine.system() == 'emscripten'
    foreach target : [
      ['webgl', ['-DSOKOL_GLES3', '-fexceptions'], webgl_link_args],
      ['webgpu', ['-DSOKOL_WGPU'] + webgpu_args, browser_link_args + webgpu_args],
    ]
      name = prefix + target[0] + '-proof'
      proof = executable(name, test_source, header,
        name_suffix: 'html', cpp_args: target[1] + fixture_args,
        link_args: target[2], dependencies: sokol_dep,
        link_depends: files('tests/browser/shell.html'))
      test(name, node, args: [files('tests/browser/run.mjs'), browser.full_path(), proof.full_path()],
        depends: proof, timeout: 90, suite: 'browser')
    endforeach
  elif host_machine.system() == 'linux' and app_available and egl_dep.found()
    foreach target : linux_gl_targets
      if target[2].found()
        shader_name = base + '_' + target[0]
        shader = custom_target(shader_name + '-shader', input: fixture[3], depend_files: fixture[5],
          output: [shader_name + '.h', shader_name + '.json'], depfile: shader_name + '.d',
          command: fixture[4] + ['--targets', target[0]])
        name = prefix + target[1]
        proof = executable(name, test_source, shader,
          cpp_args: ['-DSHADER_HEADER="' + shader_name + '.h"'] + (target[3] ? ['-DSOKOL_GLES3'] : ['-DSOKOL_GLCORE']),
          dependencies: [sokol_dep, egl_dep, target[2]] + app_deps)
        test(name, proof, suite: target[0] in ['glsl410', 'glsl300es'] ? core_suite : [])
      endif
    endforeach
  endif
endforeach

# Exercise the actual install rules without writing to the configured prefix.
# Cross builds must validate their host packages on the corresponding machine.
if not meson.is_cross_build() and (get_option('slang') or glslang.found())
  meson_program = find_program('meson', native: true)
  install_args = [files('tests/test_install.py'), '--build-dir', meson.current_build_dir(),
    '--meson', meson_program.full_path()]
  if get_option('slang')
    install_args += ['--slangc', slangc.full_path()]
  endif
  if glslang.found()
    install_args += ['--glslang', glslang.full_path()]
  endif
  test('installed-tools', python, args: install_args, depends: core,
    suite: ['install', 'core-smoke'], is_parallel: false, timeout: 120)
endif

if host_machine.system() == 'linux'
  vulkan_dep = dependency('vulkan', version: '>=1.3', required: false)
  if vulkan_dep.found() and app_available
    vulkan_fixtures = []
    vulkan_readback_shader = []
    if glslang.found()
      vulkan_readback_shader = custom_target('vulkan-readback-shader', input: 'tests/vulkan_readback.comp',
        output: 'vulkan_readback_spirv.h',
        command: [glslang, '-V', '--vn', 'vulkan_readback_spirv', '@INPUT@', '-o', '@OUTPUT@'])
    endif
    if get_option('slang')
      vulkan_fixtures += [
        ['vulkan-compute', ['-DSOKOL_VULKAN', '-DTEST_COMPUTE'], 'compute.h', compute_header],
      ]
      if glslang.found()
        vulkan_fixtures += [['vulkan-proof', ['-DSOKOL_VULKAN'], 'portable.h', portable_header]]
      endif
    endif
    if glslang.found()
      vulkan_fixtures += [
        ['generic-vulkan-proof', ['-DSOKOL_VULKAN'], 'generic.h', generic_header],
        ['generic-vulkan-compute', ['-DSOKOL_VULKAN', '-DTEST_COMPUTE'], 'generic_compute.h', generic_compute_header],
      ]
    endif
    foreach fixture : vulkan_fixtures
      proof = executable(fixture[0], test_source, fixture[3], vulkan_readback_shader,
        cpp_args: fixture[1] + ['-DSHADER_HEADER="' + fixture[2] + '"'], dependencies: [sokol_dep, vulkan_dep] + app_deps)
      test(fixture[0], proof, suite: fixture[0].startswith('generic-') ? ['core-smoke'] : [])
    endforeach
  endif
endif


storage_image_headers = []
if get_option('slang')
  storage_image_header = custom_target('storage-image-shader', input: 'tests/fixtures/storage-image/shader.slang',
    output: ['storage_image.h', 'storage_image.reflection.json'], depfile: 'storage_image.d',
    depend_files: files('tools/sokol-slang'),
    command: slang_command + ['--module', 'storage_image'])
  storage_image_headers += [['', 'storage_image.h', storage_image_header]]
endif
if glslang.found()
  generic_storage_image_spirv = custom_target('generic-storage-image-spirv', input: 'tests/fixtures/storage-image/glsl/shader.comp',
    output: 'generic_storage_image.spv', command: [glslang, '-V', '--target-env', 'vulkan1.1', '@INPUT@', '-o', '@OUTPUT@'])
  generic_storage_image_manifest = configure_file(input: 'tests/fixtures/storage-image/glsl/manifest.json', output: 'generic_storage_image_manifest.json', copy: true)
  generic_storage_image_header = custom_target('generic-storage-image-shader', input: [generic_storage_image_manifest, generic_storage_image_spirv],
    output: ['generic_storage_image.h', 'generic_storage_image.reflection.json'], depfile: 'generic_storage_image.d',
    command: core_command)
  storage_image_headers += [['generic-', 'generic_storage_image.h', generic_storage_image_header]]
  test('storage-image-contract', python, args: [files('tests/test_storage_images.py'), '--core', core.full_path(),
    '--glslang', glslang.full_path()] + (get_option('slang') ? ['--slangc', slangc.full_path()] : []),
    depends: core, timeout: 120)
endif
foreach fixture : storage_image_headers
  if host_machine.system() == 'darwin'
    proof = executable(fixture[0] + 'metal-storage-image', test_source, fixture[2],
      cpp_args: ['-x', 'objective-c++', '-DSOKOL_METAL', '-DTEST_STORAGE_IMAGE', '-DSHADER_HEADER="' + fixture[1] + '"'],
      dependencies: [sokol_dep, dependency('appleframeworks', modules: app_frameworks + ['Metal'])])
    test(fixture[0] + 'metal-storage-image', proof)
  elif host_machine.system() == 'linux' and app_available
    if egl_dep.found()
      foreach target : [['gl', gl_dep, false], ['gles', gles_dep, true]]
        if target[1].found()
          name = fixture[0] + target[0] + '-storage-image'
          proof = executable(name, test_source, fixture[2],
            cpp_args: ['-DTEST_STORAGE_IMAGE', '-DSHADER_HEADER="' + fixture[1] + '"'] + (target[2] ? ['-DSOKOL_GLES3'] : ['-DSOKOL_GLCORE']),
            dependencies: [sokol_dep, egl_dep, target[1]] + app_deps)
          test(name, proof)
        endif
      endforeach
    endif
    if vulkan_dep.found() and glslang.found()
      name = fixture[0] + 'vulkan-storage-image'
      proof = executable(name, test_source, fixture[2], vulkan_readback_shader,
        cpp_args: ['-DSOKOL_VULKAN', '-DTEST_STORAGE_IMAGE', '-DSHADER_HEADER="' + fixture[1] + '"'], dependencies: [sokol_dep, vulkan_dep] + app_deps)
      test(name, proof)
    endif
  elif host_machine.system() == 'emscripten'
    name = fixture[0] + 'webgpu-storage-image'
    proof = executable(name, test_source, fixture[2],
      name_suffix: 'html', cpp_args: ['-DSOKOL_WGPU', '-DTEST_STORAGE_IMAGE', '-fexceptions', '-DSHADER_HEADER="' + fixture[1] + '"'] + webgpu_args,
      link_args: browser_link_args + webgpu_args, dependencies: sokol_dep,
      link_depends: files('tests/browser/shell.html'))
    test(name, node, args: [files('tests/browser/run.mjs'), browser.full_path(), proof.full_path()],
      depends: proof, timeout: 90, suite: 'browser')
  elif host_machine.system() == 'windows'
    proof = executable(fixture[0] + 'd3d11-storage-image', test_source, fixture[2],
      cpp_args: ['-DSOKOL_D3D11', '-DSOKOL_WIN32_FORCE_MAIN', '-DTEST_STORAGE_IMAGE', '-DSHADER_HEADER="' + fixture[1] + '"'], dependencies: [sokol_dep, d3d11_dep] + app_deps)
    test(fixture[0] + 'd3d11-storage-image', proof)
    compiler = executable(fixture[0] + 'hlsl-storage-image-compile', 'tests/hlsl_storage_image_compile.cc', fixture[2],
      cpp_args: ['-DSHADER_HEADER="' + fixture[1] + '"'], dependencies: [sokol_dep] + d3dcompiler_deps)
    test(fixture[0] + 'hlsl-storage-image-compile', compiler)
  endif
endforeach

depth_headers = []
if get_option('slang')
  depth_header = custom_target('depth-shader', input: 'tests/fixtures/depth/shader.slang',
    output: ['depth.h', 'depth.reflection.json'], depfile: 'depth.d',
    depend_files: files('tools/sokol-slang'),
    command: slang_command + ['--module', 'depth'])
  depth_headers += [['', 'depth.h', depth_header]]
endif
if glslang.found()
  depth_spirv = []
  foreach stage : ['vert', 'frag']
    depth_spirv += custom_target('generic-depth-' + stage, input: 'tests/fixtures/depth/glsl/' + (stage == 'vert' ? 'vertex.vert' : 'fragment.frag'),
      output: 'generic_depth.' + stage + '.spv', command: [glslang, '-V', '--target-env', 'vulkan1.1', '@INPUT@', '-o', '@OUTPUT@'])
  endforeach
  generic_depth_manifest = configure_file(input: 'tests/fixtures/depth/glsl/manifest.json', output: 'generic_depth_manifest.json', copy: true)
  generic_depth_header = custom_target('generic-depth-shader', input: [generic_depth_manifest] + depth_spirv,
    output: ['generic_depth.h', 'generic_depth.reflection.json'], depfile: 'generic_depth.d',
    command: core_command)
  depth_headers += [['generic-', 'generic_depth.h', generic_depth_header]]
endif
# Reuse the portable pixel oracle with a sparse fragment interface: locations
# 0 and 2 carry UVs, while vertex outputs 1 and 3 remain unconsumed.
varyings_headers = []
if get_option('slang')
  varyings_header = custom_target('varyings-shader',
    input: 'tests/shaders/varyings.slang',
    output: ['varyings.h', 'varyings.reflection.json'], depfile: 'varyings.d',
    depend_files: files('tools/sokol-slang'),
    command: slang_command + ['--module', 'portable'])
  varyings_headers += [['', 'varyings.h', varyings_header]]
endif
if glslang.found()
  varyings_spirv = []
  foreach stage : ['vert', 'frag']
    varyings_spirv += custom_target('generic-varyings-' + stage, input: 'tests/shaders/varyings.' + stage,
      output: 'generic_varyings.' + stage + '.spv', command: [glslang, '-V', '--target-env', 'vulkan1.1', '@INPUT@', '-o', '@OUTPUT@'])
  endforeach
  varyings_manifest = configure_file(input: 'tests/shaders/varyings.manifest.json', output: 'generic_varyings_manifest.json', copy: true)
  generic_varyings_header = custom_target('generic-varyings-shader', input: [varyings_manifest] + varyings_spirv,
    output: ['generic_varyings.h', 'generic_varyings.reflection.json'], depfile: 'generic_varyings.d',
    command: core_command)
  varyings_headers += [['generic-', 'generic_varyings.h', generic_varyings_header]]
endif
foreach group : [['depth', depth_headers], ['varyings', varyings_headers]]
  foreach fixture : group[1]
    suffix = '-' + group[0]
    fixture_args = ['-DSHADER_HEADER="' + fixture[1] + '"'] + (group[0] == 'depth' ? ['-DTEST_DEPTH'] : [])
    if group[0] == 'varyings'
      test(fixture[0] + 'varyings-interface', python,
        args: [files('tests/test_varyings.py'), fixture[2][1]], depends: fixture[2])
    endif
    if host_machine.system() == 'darwin'
      name = fixture[0] + 'metal' + suffix
      proof = executable(name, test_source, fixture[2], cpp_args: ['-x', 'objective-c++', '-DSOKOL_METAL'] + fixture_args,
        dependencies: [sokol_dep, dependency('appleframeworks', modules: app_frameworks + ['Metal'])])
      test(name, proof)
      if macos_host
        name = fixture[0] + 'gl' + suffix
        proof = executable(name, test_source, fixture[2], cpp_args: ['-x', 'objective-c++', '-DSOKOL_GLCORE'] + fixture_args,
          dependencies: [sokol_dep, dependency('appleframeworks', modules: app_frameworks + ['OpenGL'])])
        test(name, proof)
      endif
    elif host_machine.system() == 'linux' and app_available
      if egl_dep.found()
        foreach backend : [['gl', gl_dep, false], ['gles', gles_dep, true]]
          if backend[1].found()
            name = fixture[0] + backend[0] + suffix
            proof = executable(name, test_source, fixture[2],
              cpp_args: fixture_args + (backend[2] ? ['-DSOKOL_GLES3'] : ['-DSOKOL_GLCORE']),
              dependencies: [sokol_dep, egl_dep, backend[1]] + app_deps)
            test(name, proof)
          endif
        endforeach
      endif
      if vulkan_dep.found() and glslang.found()
        name = fixture[0] + 'vulkan' + suffix
        proof = executable(name, test_source, fixture[2], vulkan_readback_shader,
          cpp_args: ['-DSOKOL_VULKAN'] + fixture_args, dependencies: [sokol_dep, vulkan_dep] + app_deps)
        test(name, proof)
      endif
    elif host_machine.system() == 'emscripten'
      foreach backend : ['webgl', 'webgpu']
        name = fixture[0] + backend + suffix
        is_wgpu = backend == 'webgpu'
        proof = executable(name, test_source, fixture[2],
          name_suffix: 'html', cpp_args: (is_wgpu ? ['-DSOKOL_WGPU'] : ['-DSOKOL_GLES3']) + ['-fexceptions'] + fixture_args + (is_wgpu ? webgpu_args : []),
          link_args: is_wgpu ? browser_link_args + webgpu_args : webgl_link_args, dependencies: sokol_dep,
          link_depends: files('tests/browser/shell.html'))
        test(name, node, args: [files('tests/browser/run.mjs'), browser.full_path(), proof.full_path()],
          depends: proof, timeout: 90, suite: 'browser')
      endforeach
    elif host_machine.system() == 'windows'
      name = fixture[0] + 'd3d11' + suffix
      proof = executable(name, test_source, fixture[2],
        cpp_args: ['-DSOKOL_D3D11', '-DSOKOL_WIN32_FORCE_MAIN'] + fixture_args, dependencies: [sokol_dep, d3d11_dep] + app_deps)
      test(name, proof)
      if group[0] == 'depth'
        name = fixture[0] + 'hlsl-depth-compile'
        compiler = executable(name, 'tests/hlsl_depth_compile.cc', fixture[2],
          cpp_args: fixture_args, dependencies: [sokol_dep] + d3dcompiler_deps)
        test(name, compiler)
      endif
    endif
  endforeach
endforeach

if build_machine.system() == 'darwin'
  if get_option('slang')
    test('metal-compile', python, args: [files('tests/test_metal_compile.py'),
      proof_header[0], portable_header[0], compute_header[0], storage_image_header[0], depth_header[0]], depends: [proof_header, portable_header, compute_header, storage_image_header, depth_header], timeout: 120)
  endif
  if glslang.found()
    test('generic-metal-compile', python, args: [files('tests/test_metal_compile.py'),
      generic_header[0], generic_compute_header[0], generic_storage_image_header[0], generic_depth_header[0]], depends: [generic_header, generic_compute_header, generic_storage_image_header, generic_depth_header], timeout: 120)
  endif
endif
