# Automatic bindings and program sets

`--auto-bindings` assigns Sokol slots from SPIR-V reflection. Metadata supplies
texture and sampler policy instead of resource kinds or slots:

```json
{
  "module": "example",
  "program": "draw",
  "vertex": "vertexMain",
  "fragment": "fragmentMain",
  "resources": {
    "color": { "sample_type": "float" },
    "linearSampler": { "sampler_type": "filtering" }
  }
}
```

Uniforms, views, and samplers have separate slot spaces shared across stages.
Storage buffers, textures, and storage images share view slots. Resource kinds,
access, and storage formats come from SPIR-V; the core validates sampling policy.

For several programs in one source, replace `module`, `program`, and stage fields
with a `programs` map. Each value has those same fields and a distinct
module/program pair. The top-level `resources` map applies to every program;
unused textures may be omitted by a program. Program sets require `--auto-bindings`.

The frontend compiles shared stage/entry pairs once. It joins generated headers
and keys reflection by program name; single-program reflection is unchanged.
Outputs are written after every program compiles successfully. Depfiles include
tools, metadata, source, and transitive includes/imports.

`--emit-spirv-dir` exports a manifest and stage binaries per program for replay
with `sokol-spirv --manifest`.
