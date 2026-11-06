# Triangle Example (sokol_app + Slang)

A complete graphical application using `sokol_app` and `sokol_gfx` that renders a colored triangle to the screen using a shader authored in Slang.

## How it works

1. **Slang Shader (`shader.slang`)**:
   Defines `VertexInput`, `VertexOutput`, `vertexMain`, and `fragmentMain`. Vertex colors and 2D positions are passed through to the rasterizer.

2. **Compilation with `sokol-slang`**:
   `sokol-slang` discovers entry points and generates resource metadata from the
   shader. The example selects `--module triangle` and produces `triangle.h`, which contains:
   - Backend shader code for Metal, D3D11, OpenGL, GLES, WebGPU, and Vulkan.
   - `triangle_draw_shader_desc(sg_query_backend())` to retrieve the active backend's shader descriptor.
   - Attribute indices `ATTR_triangle_draw_input_position` and `ATTR_triangle_draw_input_color`.

3. **Host Application (`main.c`)**:
   Uses `sokol_app.h` to open a window, `sokol_gfx.h` to manage GPU buffers and pipelines, and draws the 3-vertex triangle in the per-frame callback.

## Building and Running

From the repository root:

```sh
# Setup build directory (if not already done)
meson setup build

# Compile the triangle application
meson compile -C build triangle-app

# Run the application
./build/examples/triangle/triangle-app
```
