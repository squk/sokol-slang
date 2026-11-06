# Invoking sokol-slang with Meson

This example demonstrates how to integrate `sokol-slang` into any Meson build system using `custom_target`.

## 1. Overview

`sokol-slang` converts a Slang shader and a JSON metadata file into:
1. A C header (`.h`) containing pre-compiled backend shaders for OpenGL, GLES, Metal, Direct3D 11, WebGPU, and Vulkan, along with layout constants and shader descriptor functions.
2. An optional reflection file (`.reflection.json`) detailing bindings and uniforms.
3. An optional depfile (`.d`) that tells Ninja all files included or imported by the shader for precise incremental recompilation.

## 2. Integration Approaches

### Approach A: As a Meson subproject (Recommended)

When adding `sokol-slang` to your project's `subprojects/` directory (or as a wrap file `subprojects/sokol-slang.wrap`), `sokol-slang` exports three variables:
- `core`: The native `sokol-spirv` compiler executable.
- `slang_frontend`: The Python driver script (`tools/sokol-slang`).
- `slangc`: The pinned build-machine Slang compiler binary.

In your `meson.build`:

```meson
python = import('python').find_installation('python3')
sokol_slang = subproject('sokol-slang')

core = sokol_slang.get_variable('core')
slang_frontend = sokol_slang.get_variable('slang_frontend')
slangc = sokol_slang.get_variable('slangc')

shader_header = custom_target('my-shader',
  input: 'shader.slang',
  output: ['shader.h', 'shader.reflection.json'],
  depfile: 'shader.d',
  command: [
    python, slang_frontend,
    '--slangc', slangc,
    '--core', core,
    '--input', '@INPUT0@',
    '--module', 'meson_example',
    '--output', '@OUTPUT0@',
    '--reflection', '@OUTPUT1@',
    '--depfile', '@DEPFILE@'
  ]
)
```

Pass `shader_header` as a source file to any `executable()` or `library()` target to ensure the shader header is compiled before C/C++ compilation begins.

### Approach B: Using an Installed CLI

If `sokol-slang` is installed on your system `PATH`:

```meson
sokol_slang = find_program('sokol-slang')

shader_header = custom_target('my-shader',
  input: 'shader.slang',
  output: ['shader.h', 'shader.reflection.json'],
  depfile: 'shader.d',
  command: [
    sokol_slang,
    '--input', '@INPUT0@',
    '--module', 'meson_example',
    '--output', '@OUTPUT0@',
    '--reflection', '@OUTPUT1@',
    '--depfile', '@DEPFILE@'
  ]
)
```

## 3. Depfile Support

By providing `--depfile @DEPFILE@` and `depfile: 'shader.d'`, Ninja automatically tracks dependencies from Slang `import` and `#include` directives. Whenever an imported shader module is modified, Ninja will automatically re-run `sokol-slang`.
