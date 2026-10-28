# Examples

Each directory has Slang source, metadata, and equivalent GLSL in `glsl/`.

| Example | Covers |
| --- | --- |
| [portable](portable/shader.slang) | Transforms, textures, tint, clip depth |
| [compute](compute/shader.slang) | Structured buffers and workgroups |
| [storage-image](storage-image/shader.slang) | RGBA8 writes and R32UI read/write |
| [depth](depth/shader.slang) | Depth comparison and fullscreen triangle |

After [setup](../README.md#build), build the portable Slang and GLSL examples:

```sh
meson compile -C build portable-shader generic-shader
```

GLSL needs `glslangValidator`. Outputs stay in the build directory.
See [usage and limits](../docs/reference.md).
