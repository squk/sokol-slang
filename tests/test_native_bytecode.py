"""Check native bytecode defaults, source opt-out, and failed-build preservation."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True)
    parser.add_argument("--manifest", required=True)
    args = parser.parse_args()
    target = "metal_macos" if sys.platform == "darwin" else "hlsl5"
    with tempfile.TemporaryDirectory(prefix="sokol native bytecode ") as directory:
        work = Path(directory)
        output = work / "shader.h"
        command = [
            args.core,
            "--manifest",
            args.manifest,
            "--targets",
            target,
            "--output",
            str(output),
            "--reflection",
            str(work / "shader.json"),
            "--depfile",
            str(work / "shader.d"),
        ]

        def generate(extra=(), env=None):
            return subprocess.run(command + list(extra), env=env, capture_output=True, text=True)

        result = generate()
        assert result.returncode == 0, result.stderr
        native = output.read_bytes()
        assert ("_bytecode_" + target).encode() in native
        assert ("_source_" + target).encode() not in native
        assert b".source =" not in native
        result = generate()
        assert result.returncode == 0, result.stderr
        assert output.read_bytes() == native, "native bytecode generation is not deterministic"

        result = generate(["--source"])
        assert result.returncode == 0, result.stderr
        source = output.read_bytes()
        assert ("_source_" + target).encode() in source
        assert ("_bytecode_" + target).encode() not in source

        if sys.platform == "darwin":
            # Toolchain failure must not quietly enable runtime compilation.
            env = dict(os.environ, DEVELOPER_DIR=str(work / "missing-xcode"))
            result = generate(env=env)
            assert (
                result.returncode != 0 and "Metal compiler unavailable" in result.stderr
            ), result.stderr
            assert output.read_bytes() == source, "failed native compile replaced the header"
        assert not list(work.glob("shader.h.*.tmp*")), "native compiler left temporary files"
    print("PASS: native bytecode, deterministic output, source opt-out and failure preservation")


if __name__ == "__main__":
    main()
