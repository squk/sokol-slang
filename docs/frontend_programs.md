# Shader inputs and program sets

## Write a shader

Mark each entry point with its stage. A raster program needs a vertex entry and
a fragment entry; a compute program needs one compute entry. For example,
`shader.slang` can contain:

```hlsl
struct Varyings {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
Varyings vertexMain(float2 position : POSITION, float4 color : COLOR0) {
    Varyings output;
    output.position = float4(position, 0.0, 1.0);
    output.color = color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(Varyings input) : SV_Target0 {
    return input.color;
}
```

Use zero-to-one clip depth; the compiler adjusts GL output. Resource kinds,
vertex inputs and buffer layouts come from reflection. See the
[triangle](../examples/triangle/) for the matching C application and the
[textured quad](../examples/textured-quad/) for textures, samplers and uniforms.
The [reference](reference.md#validation-and-limits) lists supported buffer types
and backend limits.

## Compile and select programs

The frontend discovers `[shader(...)]` entry points through Slang, compiles the
selected stages to SPIR-V, and submits one request to `sokol-spirv`:

```sh
sokol-slang shader.slang
```

This writes `shader.h`. The filename stem supplies the module prefix; the
program name defaults to `draw` for vertex/fragment shaders or `run` for compute.
Use `--module`, `--program`, and `-o` to choose names and the output path.

For ambiguous entry points, select `--vertex ENTRY --fragment ENTRY` or
`--compute ENTRY`. A program set uses repeatable selections:

```sh
sokol-slang shader.slang --module example \
  --program base:vertexMain:fragmentMain \
  --program depth:vertexMain:depthFragment \
  --reflection shader.reflection.json
```

Compute selections use `--program NAME:COMPUTE`. The core generates one combined
header and keys reflection by program name. Shared Slang stage/entry pairs
compile once.

## Resource metadata

The core inspects SPIR-V for resource names, kinds, access, storage formats, texture scalar
types, and comparison-sampler use. Float textures default to `float`; ordinary
samplers default to `filtering`. Override policies that depend on runtime formats:

```sh
sokol-slang shader.slang \
  --sample-type positionTexture=unfilterable_float \
  --sampler-type pointSampler=nonfiltering
```

Texture types are `float`, `unfilterable_float`, `depth`, `sint`, and `uint`.
Sampler types are `filtering`, `nonfiltering`, and `comparison`. The core checks
that these policies agree with the compiled shader.

Slots are assigned automatically across all stages in a program. Uniforms,
views, and samplers have separate spaces; textures, storage buffers and storage
images share view slots. Reserve slots with repeatable
`--binding STAGE:NAME=SLOT` options when a host interface needs fixed indices.

The frontend writes a compact manifest internally: program selections, stage
files, and optional caller constraints. The core owns resource discovery, slot
allocation, backend choices, and header generation. Direct SPIR-V callers use
the same contract without Slang. `--reflection` writes compiled metadata;
`--emit-spirv-dir` exports stage binaries and one `manifest.json` for replay with
`sokol-spirv --manifest`, including the complete program set and shared stages.

Headers and sidecars are published after every program succeeds. Depfiles include
tools, shader inputs, and transitive includes/imports. Default GL targets use
GLSL 4.30/GLES 3.10 when storage resources or compute require them, and
GLSL 4.10/GLES 3.00 otherwise.
