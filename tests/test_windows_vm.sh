#!/usr/bin/env bash
# Cross-builds the tests for Windows x64 with MinGW and runs them in a Parallels Windows VM, on D3D11 and Vulkan.
#
#   tests/test_windows_vm.sh [--vm NAME] [--install-lavapipe] [d3d11] [vulkan]
#
# Needs prlctl, meson, ninja, x86_64-w64-mingw32-gcc/g++, glslangValidator, zstd and, for Vulkan, the Vulkan
# headers (brew install vulkan-headers, or set VULKAN_HEADERS to an include directory). The shaders compile on
# the host (the build fetches the macOS Slang SDK); the Windows binaries only run. The VM must share the host
# home folder (Parallels shares it as \\Mac\Home), and the repository must live under it.
#
# The binaries are x64, so on an ARM64 VM they run under emulation and need an x64 Vulkan loader: the script
# puts the Khronos loader from MSYS2's mingw64 package next to the test binaries. The VM also needs a Vulkan
# driver; --install-lavapipe installs Mesa's x64 lavapipe (mesa-dist-win) into C:\Program Files\Mesa and
# registers it under HKLM\SOFTWARE\Khronos\Vulkan\Drivers.
#
# The d3d11 backend runs every test executable that is not a Vulkan one: the D3D11 fixtures, the HLSL compile
# checks and the descriptor checks. The vulkan backend runs the *vulkan* fixtures.
set -euo pipefail

vm="Windows 11"
install_lavapipe=0
backends=()
while [ $# -gt 0 ]; do
    case "$1" in
        --vm) vm="$2"; shift 2 ;;
        --install-lavapipe) install_lavapipe=1; shift ;;
        d3d11|vulkan) backends+=("$1"); shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ ${#backends[@]} -eq 0 ] && backends=(d3d11 vulkan)
with_vulkan=0
for backend in "${backends[@]}"; do [ "$backend" = vulkan ] && with_vulkan=1; done

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/tests/build"
# One build directory per Meson configuration: Vulkan adds header and loader paths.
dir="$build/mingw_d3d11"
[ "$with_vulkan" = 1 ] && dir="$build/mingw_vulkan"
case "$root" in
    "$HOME"/*) ;;
    *) echo "the repository must be under $HOME, which the VM shares as \\\\Mac\\Home" >&2; exit 1 ;;
esac
# A host path under $HOME as the VM sees it.
vm_path() { local rel="${1#"$HOME"/}"; echo "\\\\Mac\\Home\\${rel//\//\\}"; }

vm_ps() { prlctl exec "$vm" --current-user powershell -NoProfile -Command "$1"; }
vm_admin_ps() { prlctl exec "$vm" powershell -NoProfile -Command "$1"; }

if ! prlctl list "$vm" 2>/dev/null | grep -q running; then
    echo "starting VM '$vm'"
    prlctl start "$vm"
    until prlctl exec "$vm" --current-user cmd /c echo ready 2>/dev/null | grep -q ready; do sleep 5; done
fi

if [ "$install_lavapipe" = 1 ]; then
    release=$(curl -s https://api.github.com/repos/pal1000/mesa-dist-win/releases/latest |
              python3 -c "import json,sys; print(json.load(sys.stdin)['tag_name'])")
    url="https://github.com/pal1000/mesa-dist-win/releases/download/$release/mesa3d-$release-release-msvc.7z"
    echo "installing lavapipe from mesa-dist-win $release"
    vm_admin_ps "
        \$ErrorActionPreference = 'Stop'
        \$archive = Join-Path \$env:TEMP 'mesa-dist-win.7z'
        curl.exe -sSL -o \$archive '$url'
        New-Item -ItemType Directory -Force -Path 'C:\Program Files\Mesa' | Out-Null
        tar.exe -xf \$archive -C 'C:\Program Files\Mesa' x64/vulkan_lvp.dll x64/lvp_icd.x86_64.json
        Remove-Item \$archive
        New-Item -Force -Path 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers' | Out-Null
        New-ItemProperty -Force -Path 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers' \`
            -Name 'C:\Program Files\Mesa\x64\lvp_icd.x86_64.json' -PropertyType DWord -Value 0 | Out-Null"
fi

# MinGW cross build. Meson wraps the Slang SDK and the sokol-tools dependencies as subprojects.
cross=(--cross-file "$root/tests/mingw.ini")
if [ "$with_vulkan" = 1 ]; then
    headers="${VULKAN_HEADERS:-$(brew --prefix vulkan-headers 2>/dev/null)/include}"
    [ -f "$headers/vulkan/vulkan.h" ] || { echo "no Vulkan headers at $headers; set VULKAN_HEADERS" >&2; exit 1; }
    loader="$build/msys2-vulkan-loader"
    if [ ! -f "$loader/mingw64/bin/vulkan-1.dll" ]; then
        package=$(curl -s https://repo.msys2.org/mingw/mingw64/ |
                  grep -o 'mingw-w64-x86_64-vulkan-loader-[^"]*\.pkg\.tar\.zst"' | tr -d '"' | sort -V | tail -1)
        echo "fetching $package"
        rm -rf "$loader" && mkdir -p "$loader"
        curl -sSL "https://repo.msys2.org/mingw/mingw64/$package" | zstd -dq | tar -xf - -C "$loader"
    fi
    # mingw.ini links with -static, and ld then skips *.dll.a import libraries; give it a .a name.
    if [ ! -f "$loader/mingw64/lib/libvulkan-1.a" ]; then
        cp "$loader/mingw64/lib/libvulkan.dll.a" "$loader/mingw64/lib/libvulkan-1.a"
    fi
    # NDEBUG: sokol debug builds ask Vulkan for the validation layer, which the VM lacks. b_ndebug is a
    # combo option, so its value is the string 'true', not a cross-file boolean.
    # A later cross file replaces an earlier one's option, so repeat mingw.ini's -static.
    mkdir -p "$build"
    ini="$(cat <<EOF
[built-in options]
b_ndebug = 'true'
c_args = ['-I$headers']
cpp_args = ['-I$headers']
cpp_link_args = ['-static', '-L$loader/mingw64/lib']
EOF
)"
    # Meson keeps the cross files it was first given, so a changed one needs a fresh build directory.
    if [ ! -f "$build/vulkan.ini" ] || [ "$ini" != "$(cat "$build/vulkan.ini")" ]; then
        rm -rf "$dir"
        printf '%s\n' "$ini" > "$build/vulkan.ini"
    fi
    cross+=(--cross-file "$build/vulkan.ini")
fi
setup=(meson setup)
[ -d "$dir/meson-private" ] && setup+=(--reconfigure)
"${setup[@]}" "$dir" "${cross[@]}" > /dev/null
meson compile -j"$(sysctl -n hw.ncpu)" -C "$dir"
[ "$with_vulkan" = 1 ] && cp "$loader/mingw64/bin/vulkan-1.dll" "$dir/"

status=0
passed=0
failed=0
for backend in "${backends[@]}"; do
    tests=$(cd "$dir" && ls *.exe | sed 's/\.exe$//')
    if [ "$backend" = vulkan ]; then
        tests=$(echo "$tests" | grep vulkan || true)
    else
        tests=$(echo "$tests" | grep -v vulkan || true)
    fi
    [ -n "$tests" ] || { echo "no $backend test executables were built" >&2; status=1; continue; }
    # Lavapipe compiles shaders to x64 code at run time for the CPU features the VM reports. Windows' x64
    # emulation on an ARM64 VM reports AVX-class features but faults (0xC000001D, illegal instruction) in
    # lavapipe's generated raster code; compute alone survives. SSE4.1 code runs.
    env_setup=""
    [ "$backend" = vulkan ] && env_setup="\$env:GALLIUM_OVERRIDE_CPU_CAPS = 'sse4.1'"
    for test in $tests; do
        echo "== $backend $test"
        # Output goes to files in the shared build directory: prlctl does not relay a redirected
        # process's console, and a crash would lose buffered stdout. A hung test is killed after
        # two minutes.
        out="$dir/$test.out.txt"
        err="$dir/$test.err.txt"
        rm -f "$out" "$err"
        # prlctl sometimes rejects a call with "Invalid argument"; retry it.
        code=""
        for attempt in 1 2 3; do
            code=$(vm_ps "
                Set-Location '$(vm_path "$dir")'
                $env_setup
                \$p = Start-Process -FilePath '.\\$test.exe' -NoNewWindow -PassThru \`
                    -RedirectStandardOutput '$(vm_path "$out")' -RedirectStandardError '$(vm_path "$err")'
                \$null = \$p.Handle
                if (\$p.WaitForExit(120000)) { \$p.ExitCode } else { \$p.Kill(); 'timeout' }" | tr -d '\r' | tail -1) && break
            code="prlctl error"
            sleep 2
        done
        cat "$out" "$err" 2>/dev/null || true
        if [ "$code" != 0 ]; then
            echo "$backend $test failed with exit code $code" >&2
            status=1
            failed=$((failed + 1))
        else
            passed=$((passed + 1))
        fi
    done
done
echo "Windows VM: $passed passed, $failed failed"
exit $status
