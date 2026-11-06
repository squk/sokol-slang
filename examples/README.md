# Examples

- [triangle](triangle/): Draw a colored triangle with `sokol_app` and Slang.
- [meson-invocation](meson-invocation/meson.build): Compile shaders from Meson.

Build and run the triangle from the repository root:

```sh
meson setup build
meson compile -C build triangle-app
./build/examples/triangle/triangle-app
```
