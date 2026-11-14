"""Keep public resource names while GLSL sanitizes its own identifiers."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--core", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="sokol resource names ") as directory:
        work = Path(directory)
        header = work / "shader.h"
        reflection = work / "shader.json"
        subprocess.run(
            [
                sys.executable,
                str(root / "tools/sokol-slang"),
                str(root / "tests/shaders/resource_names.slang"),
                "--core",
                args.core,
                "--source",
                "--targets",
                "metal_macos:glsl430",
                "--output",
                str(header),
                "--reflection",
                str(reflection),
            ],
            check=True,
        )
        report = json.loads(reflection.read_text())
        layout = report["layout"]
        assert {item["name"] for item in layout["textures"]} == {"vertex__spatial_vertex_texture"}
        assert {item["name"] for item in layout["samplers"]} == {"vertex__spatial_vertex_sampler"}
        pairs = layout["texture_sampler_pairs"]
        assert len(pairs) == 1
        assert pairs[0]["view_slot"] == layout["textures"][0]["slot"]
        assert pairs[0]["sampler_slot"] == layout["samplers"][0]["slot"]
        # Pair names feed glGetUniformLocation and must name the emitted GLSL uniform.
        declaration = r"uniform\s+sampler2D\s+" + re.escape(pairs[0]["name"]) + r"\s*;"
        assert re.search(declaration, header.read_text()), pairs[0]["name"]


if __name__ == "__main__":
    main()
