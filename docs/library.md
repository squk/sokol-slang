# C++ integration

Use the libraries when an editor or build tool needs owned shader data rather
than a generated C header. Calls run synchronously; the caller owns scheduling,
caching, GPU object creation and pipeline readiness.

## Link from Meson

Consume the project as a subproject with `slang` and `compiler_library` enabled
(both default to true):

```meson
compiler_project = subproject('sokol-slang')
program_compiler_dep = compiler_project.get_variable('program_compiler_dep')
shader_tool = executable('shader-tool', 'shader_tool.cc',
  dependencies: program_compiler_dep, native: true)
```

These libraries run on the build machine. Cross-built players consume their
outputs. The CLI installation does not install a C++ SDK.

The interface headers live directly in `src/`; `src/internal/` is private:

| Header | Meson dependency | Use |
| --- | --- | --- |
| [program_compile.h](../src/program_compile.h) | `program_compiler_dep` | Compile named programs from Slang or reuse a source result. |
| [slang_compile.h](../src/slang_compile.h) | `source_compiler_dep` | Compile Slang to SPIR-V and source reflection. |
| [backend_compile.h](../src/backend_compile.h) | `backend_compiler_dep` | Translate SPIR-V into backend code and native layout. |
| [manifest.h](../src/manifest.h) | `backend_compiler_dep` | Validate manifest policy and resolve stage files. |

## Compile a program

This example supplies the shader bytes directly and selects the entries from the
[shader authoring guide](frontend_programs.md#write-a-shader):

```cpp
#include "program_compile.h"
#include <iostream>
#include <string>

bool compile_shader(const std::string &shader_source) {
    sokol_slang::ProgramRequest request;
    request.source.module_name = "example";
    request.source.source_path = "shaders/example.slang";
    request.source.source = shader_source;
    request.targets = "glsl410:wgsl:spirv_vk";
    request.programs["draw"].entries = {
        {"vertex", "vertexMain"}, {"fragment", "fragmentMain"}};
    request.programs["draw"].policy = {{"clip_depth", "zero_to_one"}};

    auto result = sokol_slang::compile_programs(request);
    for (const auto &diagnostic : result.source.diagnostics)
        std::cerr << diagnostic.phase << ": " << diagnostic.text << '\n';
    if (!result.succeeded)
        return false;

    // The caller decides where to store or upload this owned artifact.
    const auto document = result.programs.at("draw").to_json();
    std::cout << document.dump(2) << '\n';
    return true;
}
```

Compute selections use `{{"compute", "computeMain"}}` and omit `clip_depth`.
Targets must be explicit and use the same colon-separated names as
[the CLI](reference.md#backend-outputs). Program policy uses the
[manifest schema](../schema/manifest.schema.json); module, program and stage
data are supplied by the call. Shared stage/entry pairs compile once.

## Supply captured source

`source_path` identifies the root source; `source` holds its contents. Put other
includes and imports in `SourceRequest::files`, keyed by paths, and provide
`search_paths` for include/import lookup. Do not repeat the root in `files`.
Paths are lexically normalized, and duplicate normalized paths are rejected.
The compiler reads only captured bytes and never falls back to live source files.
Keep the request unchanged until the call returns.

`defines` maps macro names to replacement strings for the root, includes and
imports. `profile` defaults to `spirv_1_3`; `capabilities` adds Slang capability
names. The optional `progress` callback runs on the compiling thread and must
not throw. Each source call creates a fresh request session and reuses that
thread's global Slang session for standard-library startup.

## Handle results and failures

Check `ProgramResult::succeeded` before using its program map. Any program failure
clears the entire map; diagnostics in `result.source.diagnostics` identify the
failing phase or program. `result.source.succeeded` describes source compilation
alone, so it may be true when backend compilation fails.

`SourceResult` owns SPIR-V words, Slang reflection, declaration metadata, dependency
paths, compiler identity and diagnostics after the Slang session ends. Declaration
metadata includes unreferenced declarations exposed by module reflection; it is
not a report of the data each optimized entry reads.

To reuse source compilation, pass a `SourceResult` to the other
`compile_programs` overload with selections, targets and a module name. Every
selected stage/entry pair must already exist in that result. `compile_source`
can also be called directly: an empty `entry_points` list discovers marked
entries; an explicit list compiles only those names.

For SPIR-V callers, `compile_backend` takes owned stage words and manifest policy.
It throws on invalid inputs or backend compilation errors. Translation runs on a
private 8 MiB stack and joins before returning; thread setup failures throw
`std::system_error`. Bound concurrent calls in the caller.

Library requests compile native Metal bytecode on Apple hosts and DXBC on Windows
by default, matching the CLI. Set `BackendRequest::source_only` or
`ProgramRequest::source_only` to keep source instead; the source-result overload
also accepts this flag. `BackendRequest::metal_args` passes arguments in order to
the host Metal compiler. Native tools use a private temporary directory removed
on success or failure. These calls perform no GPU work.

## Read an artifact

`ProgramArtifact::layout` exports merged attributes, uniform blocks and members,
texture/sampler pairs, storage resources, fragment outputs and compute workgroup
sizes. It uses the C-header generator's enum mappings and includes native binding
indices and uniform flattening. Use native layout bindings for runtime slots.

`ProgramArtifact::to_json()` writes format version 2 with layout, reflection,
stage source/bytecode, entries and native profiles. Attribute, uniform-block,
texture, sampler and storage-resource entries use `source_name` for source lookup;
`name` retains the backend binding name. Attribute source names join SPIR-V
reflection by remapped location; resource names join original stage descriptors
by set and binding. Names must agree across backend targets. Fragment outputs
and texture/sampler pairs retain their existing native names.

The CLI reflection report exposes the native layout too. Source-request and
manifest schemas remain version 1; their version is independent of the artifact
format. See [backend outputs](reference.md#backend-outputs) for host tool
requirements and source-versus-bytecode behavior.
