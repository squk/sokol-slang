# Toolchain reference

Commands run from the repository root. See [quick start](../README.md#build)
and [examples](../examples/README.md).

## Build

`./test.sh` configures, builds, and tests in `build-all`. Arguments go to Meson
setup, e.g. `./test.sh --wrap-mode=nodownload`.

Core-only build, without Slang:

```sh
meson setup build-core -Dslang=false
meson compile -C build-core
meson test -C build-core --print-errorlogs
```

Browser tests require Emscripten (tested: 6.0.2), Node.js 22+, and Chromium with
working WebGL2/WebGPU devices:

```sh
meson setup build-web --cross-file tests/emscripten.ini -Dbrowser=/path/to/chromium
meson test -C build-web --suite browser --print-errorlogs
```

The runner uses a temporary profile and local HTTP server. Offline builds need
both Meson's package cache and Emscripten's `emdawnwebgpu` cache populated.

For an Apple Silicon simulator, boot a device in Xcode:

```sh
meson setup build-ios --cross-file tests/ios-simulator.ini --cross-file tests/ios-simulator-run.ini
meson test -C build-ios --print-errorlogs
```

Omit the run file for SDK-only compilation. `tests/ios-device.ini` targets physical
devices; installation is manual. Base-vertex/base-instance shaders require A9+.
See [Windows checks](../tests/windows.md) for MSVC and MinGW commands.

## Docker

```sh
docker build -t sokol-slang-tests .
docker run --rm --shm-size=1g -v "$PWD:/work" sokol-slang-tests
```

The image runs Linux GL/GLES/Vulkan tests (Mesa software drivers), browser
WebGL2/WebGPU tests, and a MinGW Windows cross-build. Pass `linux`, `browser`,
or `windows` to run one group; set `-e JOBS=8` to change build parallelism.
Build `--target linux` for the smaller Linux-only image. Source and build outputs
stay in the mounted checkout; dependencies are downloaded on the first run.
Chromium runs without its sandbox inside the container.

Windows cross-builds do not execute D3D11 tests. Metal/iOS and native Windows
execution require their respective hosts.

## Meson consumers

`subproject('sokol-slang')` exports `core` (the native compiler target),
`slang_frontend` (the Python script), and `slangc` (the pinned compiler).
Pass `core` in generation commands so Meson tracks it as a build dependency.
Subproject builds omit examples, tests, and installation.

## Install

```sh
meson install -C build
meson test -C build installed-tools --print-errorlogs
```

Installs `sokol-spirv` and, when enabled, the `sokol-slang` CLI. The schema is
embedded; examples are not installed. The frontend requires Python 3 and an
external Slang 2026.20 compiler.

`installed-tools` stages a temporary install and compiles copied examples outside
the checkout. Slang SPIR-V replay must produce identical headers and reflection.
It also runs in native core-only builds when `glslangValidator` is available.

## Dependencies

Meson downloads pinned, hash-verified archives from `subprojects/*.wrap`.
Overlays live in `subprojects/packagefiles/`. Offline builds require
`subprojects/packagecache/` and `--wrap-mode=nodownload`.

Slang packages target the build machine: macOS, Linux (glibc 2.28+), or Windows,
x86-64/ARM64. Override selection with `-Dslangc=/path/to/slangc` (version 2026.20).

## SPIR-V interface

```sh
build/sokol-spirv --manifest program.json \
  --output build/shader.h --reflection build/shader.json --depfile build/shader.d
build/sokol-spirv --inspect shader.spv
```

The [version 1 schema](../schema/manifest.schema.json) defines the contract;
see [raster](../examples/portable/glsl/manifest.json) and
[compute](../examples/compute/glsl/manifest.json) manifests. Unknown/duplicate
fields, invalid types, missing resources, and binding collisions are errors.

- A program has vertex/fragment stages or one compute stage. Each SPIR-V module
  has one entry point; paths are relative to the manifest. SPIR-V through 1.3 is
  validated, and entry names are normalized to `main`.
- Resources match by stage, descriptor set, and binding. Public names come from
  metadata. Stripped members/attributes receive `member_N`/`input_N` names.
- Uniform, sampler, and view slots are separate global namespaces. Textures,
  storage buffers, and storage images share view slots. Each stage use needs its
  own Sokol slot; source descriptor numbers need not match native bindings.
- Raster manifests require `clip_depth: "zero_to_one"`; GL output converts to
  negative-one-to-one depth. Use `SV_VulkanVertexID` for raw vertex indices on
  GLES; Slang's `SV_VertexID` requires unsupported `BaseVertex` handling there.

## Slang frontend

See the [compilation command](../README.md#compile). Input may be positional
or supplied with `--input`; `-o`/`--output` selects the header. `--reflection` and
`--depfile` are optional. Tools resolve beside the CLI, then on `PATH`; the
checkout also checks `build/`, `build-all/`, and the pinned Slang package.
Use `--core` or `--slangc` to override discovery.

Metadata names entry points and resources, with a `slots` map for stage bindings:

- Textures require `sample_type`; samplers require `sampler_type`. Types must
  match SPIR-V. Integer/unfilterable textures require nonfiltering samplers;
  use texel loads for portable integer textures.
- Compute uses a `compute` entry and slots. Storage images require `format` and
  explicit `access` (`write` or `read_write`); metadata cannot change shader access.
- Generated resource aliases have stage prefixes: `vertex_`, `fragment_`, `compute_`.

Compilation uses SPIR-V 1.3, column-major matrices, and GL buffer layout.
Repeatable `-I path`/`--include-dir path` resolves from the working directory;
depfiles include transitive includes/imports. Failed compilation preserves the
previous header.

For automatic slot allocation and multiple programs per source, see
[automatic bindings and program sets](frontend_programs.md).

`--emit-spirv-dir path` exports stage binaries and `manifest.json` for reuse with
the core, without Slang.

## Using generated headers

Use the Sokol revision in [sokol.wrap](../subprojects/sokol.wrap); generated
headers track that API. Include `sokol_gfx.h` first, then the generated header.
After `sg_setup()`, the portable example creates its shader with:

```c
sg_shader shader = sg_make_shader(portable_draw_shader_desc(sg_query_backend()));
```

Use the generated `ATTR_*`, `UB_*`, `VIEW_*`, and `SMP_*` constants for bindings.
The [portable fixture](../tests/portable_fixture.h) shows pipeline setup and uniforms.

## Backend outputs

Defaults include all eight backend variants. Both tools accept colon-separated
`--targets`, choosing at most one dialect per backend:

| Backend | Default | Alternative |
| --- | --- | --- |
| OpenGL | `glsl410` | `glsl430` |
| GLES / WebGL2 | `glsl300es` | `glsl310es` |
| D3D11 | `hlsl5` | `hlsl4` |
| Metal macOS | `metal_macos` | |
| Metal iOS | `metal_ios` | |
| Metal simulator | `metal_sim` | |
| WebGPU | `wgsl` | |
| Vulkan | `spirv_vk` | |

Compute defaults use `glsl430`/`glsl310es`; WebGL2 and macOS OpenGL cannot dispatch
compute. Dialect selection cannot add runtime capabilities. Pinned Sokol has no
D3D12 backend; its Vulkan backend is experimental.

SPIRV-Cross emits GLSL/HLSL/MSL, Tint emits WGSL, and Vulkan embeds remapped SPIR-V
validated for Vulkan 1.1. Metal/D3D11 compile generated source at runtime.
JSON reports separate source-layout `spirv` reflection from native `bindings` and
`entry`. Use native binding records for runtime slots.

## Validation and limits

Compiler tests cover eleven dialects, strict metadata, stripped names, custom
entries, dependencies, deterministic output, and failed-compile preservation.
Runtime fixtures check pixel/buffer readback from Slang and GLSL on macOS
Metal/OpenGL, Linux Mesa GL/GLES/Vulkan, and Chromium WebGL2/WebGPU. Compute runs
on the capable backends; missing GPU access fails rather than skips native tests.

`*varyings*` fixtures verify unused vertex outputs and consumed locations 0/2.
`*-depth` fixtures verify comparison sampling and attachment transitions.
Apple compiler and Wine/DXBC checks do not establish device execution:
**native Windows/D3D11 and iOS/simulator execution remain unverified**.

Tests use `sokol_app`/`sokol_glue` on every backend. Headless Linux runs under
`xvfb-run -a` with `LIBGL_ALWAYS_SOFTWARE=1`. Native helpers only handle GPU readback.
Vulkan fixtures require Vulkan 1.3, `VK_EXT_descriptor_buffer`, and Khronos
validation layers; readback uses private Sokol handle queries. The
[Docker test image](../Dockerfile) provides these dependencies.

Shader limits:

- One raster or compute program per invocation; fixed compute workgroups with
  total thread count divisible by 32.
- No resource arrays, push constants, or multiple buffers sharing one block type.
- Uniforms: 32-bit float/int scalars/vectors, float4x4, and arrays of float4,
  int4, or float4x4. Single-member array wrappers work; other nested structs do not.
- Storage buffers: one unsized record array; unsupported on `glsl410`,
  `glsl300es`, and `hlsl4`. Vertex-stage access must be read-only.
- Storage images: non-multisampled 2D, 2D array, or 3D, with a schema-listed
  format. Use textures for read-only access and check `sg_query_pixelformat()`
  for device support.
