"""Stage a native install and compile copied examples outside the checkout."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(command, cwd, env=None):
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
    assert result.returncode == 0, f"{command}\n{result.stdout}{result.stderr}"
    return result.stdout


def outputs(directory):
    directory.mkdir(parents=True, exist_ok=True)
    return [
        "--output",
        str(directory / "shader.h"),
        "--reflection",
        str(directory / "shader.json"),
        "--depfile",
        str(directory / "shader.d"),
    ]


def check_output(directory, stages):
    header = (directory / "shader.h").read_bytes()
    assert b"sg_shader_desc" in header, "missing generated descriptor"
    targets = json.loads((directory / "shader.json").read_text())["targets"]
    assert targets, "missing backend descriptors"
    assert all(set(target) == stages for target in targets.values()), targets
    assert (directory / "shader.d").stat().st_size > 0, "missing dependency file"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--meson", required=True)
    parser.add_argument("--slangc", type=Path)
    parser.add_argument("--glslang", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    assert args.slangc or args.glslang, "a shader compiler is required"

    with tempfile.TemporaryDirectory(prefix="sokol installed tools ") as directory:
        work = Path(directory).resolve()
        assert root not in work.parents, "install check must run outside the checkout"
        staged = work / "staged"
        run(
            [args.meson, "install", "-C", str(build), "--no-rebuild", "--destdir", str(staged)],
            work,
        )
        core_name = "sokol-spirv.exe" if os.name == "nt" else "sokol-spirv"
        installed = list(staged.rglob(core_name))
        assert len(installed) == 1, installed
        core = installed[0]
        run([str(core), "--help"], work)
        frontends = list(staged.rglob("sokol-slang"))
        if args.slangc:
            assert len(frontends) == 1, frontends
            frontend = frontends[0]
            if os.name != "nt":
                assert os.access(frontend, os.X_OK), "installed frontend is not executable"
                run([str(frontend), "--help"], work)
            cli = [sys.executable, str(frontend)]
        else:
            assert not frontends, "core-only install unexpectedly includes the frontend"

        # Packaging smoke checks use one raster and one compute input. Feature
        # contracts, native bytecode and SPIR-V replay have their own tests.
        tool_env = dict(os.environ)
        if args.slangc:
            tool_env["PATH"] = str(args.slangc.parent) + os.pathsep + tool_env.get("PATH", "")
        for name, stages in [("portable", {"vertex", "fragment"}), ("compute", {"compute"})]:
            example = work / "fixtures" / name
            shutil.copytree(root / "tests/fixtures" / name, example)
            generated = work / "generated" / name
            if args.slangc:
                # Find the installed sibling core and the external Slang compiler
                # through the same discovery path a normal invocation uses.
                command = [*cli, str(example / "shader.slang")]
            else:
                manifest = example / "glsl/manifest.json"
                for stage, item in json.loads(manifest.read_text())["stages"].items():
                    suffix = {"vertex": "vert", "fragment": "frag", "compute": "comp"}[stage]
                    source = (
                        example
                        / "glsl"
                        / ("shader.comp" if stage == "compute" else f"{stage}.{suffix}")
                    )
                    run(
                        [
                            str(args.glslang),
                            "-V",
                            "--target-env",
                            "vulkan1.1",
                            str(source),
                            "-o",
                            str(manifest.parent / item["file"]),
                        ],
                        work,
                    )
                command = [str(core), "--manifest", str(manifest)]
            run([*command, *outputs(generated)], work, env=tool_env)
            check_output(generated, stages)

    print("PASS: installed tools compile raster and compute inputs outside the checkout")


if __name__ == "__main__":
    main()
