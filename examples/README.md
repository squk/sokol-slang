# Examples

Working examples demonstrating how to compile and run shaders with `sokol-slang`.

| Example | Covers |
| --- | --- |
| [triangle](triangle/README.md) | Complete graphical application using `sokol_app` and `sokol_gfx` rendering a colored triangle with a Slang shader |
| [meson-invocation](meson-invocation/README.md) | Meson `custom_target` integration guide and sample build file for consuming `sokol-slang` |

## Building the Triangle App

From the repository root:

```sh
meson setup build
meson compile -C build triangle-app
./build/examples/triangle/triangle-app
```
