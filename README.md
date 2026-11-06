# sokol-slang

Compile Slang into a Sokol header with backend shaders, buffer structs, and
binding constants. Applications need only the header and Sokol.

[![Textured quad rendered with Sokol and Slang](examples/textured-quad/screenshot.png)](examples/README.md#textured-quad)

Alpha: APIs may change. Windows/D3D11 and iOS execution are unverified.

## Build

Requires C++20, Python 3, Meson 1.1+, and Ninja. Tests also use `glslangValidator`.

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

## Compile

```sh
python3 tools/sokol-slang examples/triangle/shader.slang \
  --module triangle -o build/triangle.h
```

A shader alone is enough: `sokol-slang shader.slang` writes `shader.h`.
Entry points, resource types, and bindings come from reflection.

The frontend discovers `sokol-spirv` automatically; override with `--core`.
Slang source compilation runs through its linked compiler library. Select the
Slang SDK at Meson setup with `-Dslangc=/path/to/slangc`.
Use `--targets` to select backends and `-I` for imports/includes.

[Examples](examples/README.md) · [Docker tests](docs/reference.md#docker) · [Reference](docs/reference.md)

## Formatting

Install `clang-format` and Black (`python3 -m pip install black`), then install the
pre-commit hook from the repository root:

```sh
install -m 755 .githooks/pre-commit "$(git rev-parse --git-path hooks)/pre-commit"
```

The hook checks staged C/C++/Objective-C sources with `clang-format` and Python
sources (including `tools/sokol-slang`) with Black. It excludes vendored code in
`deps/` and `subprojects/` and prints formatting commands when changes are needed.
It leaves files and staging untouched and preserves other installed hooks.
