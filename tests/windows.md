# Windows shader checks

Requires a Windows developer shell, C++20, Windows SDK, Python, Meson, Ninja,
and `glslangValidator`.

```sh
meson setup build-windows
meson compile -C build-windows
meson test -C build-windows --no-rebuild --print-errorlogs
```

Tests cover HLSL 4/5 from Slang and GLSL: `*-compile` checks bytecode and
reflection; `*d3d11*` checks raster, depth sampling, compute, and storage images
using hardware or WARP (feature level 11.0). Compiler-only and Wine passes do
not prove native rendering.

To cross-compile with MinGW-w64:

```sh
meson setup build-windows --cross-file tests/mingw.ini
meson compile -C build-windows
```

Run the resulting executables on Windows with `d3dcompiler_47.dll` available.
