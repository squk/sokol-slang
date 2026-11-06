"""Exercise the SPIR-V contract using GLSL, with no Slang dependency."""

import argparse
import copy
import json
from pathlib import Path
import subprocess
import tempfile

from test_support import TARGETS, run, rewrite_spirv, check_translation


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--core", required=True)
    parser.add_argument("--glslang", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    original = json.loads((root / "tests/fixtures/portable/glsl/manifest.json").read_text())
    with tempfile.TemporaryDirectory(prefix="sokol spirv ") as directory:
        work = Path(directory)
        binaries = {}
        for stage, suffix in (("vertex", "vert"), ("fragment", "frag")):
            output = work / original["stages"][stage]["file"]
            run(
                [
                    args.glslang,
                    "-V",
                    "--target-env",
                    "vulkan1.1",
                    str(root / f"tests/fixtures/portable/glsl/{stage}.{suffix}"),
                    "-o",
                    str(output),
                ]
            )
            binaries[stage] = output.read_bytes()
        manifest = work / "manifest.json"
        header = work / "shader.h"
        report = work / "shader.json"
        command = [
            args.core,
            "--source",
            "--manifest",
            str(manifest),
            "--output",
            str(header),
            "--reflection",
            str(report),
            "--depfile",
            str(work / "shader.d"),
        ]

        def generate(data=original, targets=None, error=None, text=None):
            manifest.write_text(json.dumps(data) if text is None else text)
            old = header.read_bytes() if header.exists() else None
            result = subprocess.run(
                command + (["--targets", targets] if targets is not None else []),
                capture_output=True,
                text=True,
            )
            if error:
                assert result.returncode != 0, "invalid input succeeded"
                assert error in result.stderr, result.stderr
                assert header.read_bytes() == old, "failed generation replaced the header"
            else:
                assert result.returncode == 0, result.stdout + result.stderr
            return result

        generate()
        first = header.read_bytes()
        generate()
        assert header.read_bytes() == first, "output is not reproducible"
        assert "\\ " in (work / "shader.d").read_text()
        assert len(json.loads(report.read_text())["targets"]) == 8
        for target in TARGETS:
            generate(targets=target)
            stages = json.loads(report.read_text())["targets"][target]
            for stage in ("vertex", "fragment"):
                bindings = {r["name"]: r for r in stages[stage]["bindings"]}
                assert bindings[stage + "_params"]["slot"] == (2 if stage == "vertex" else 6)
            # Validate generated GLSL and HLSL with a second compiler. Tint
            # validates WGSL during translation; the core validates Vulkan SPIR-V.
            if target.startswith(("glsl", "hlsl")):
                for snippet, suffix in (("vs", "vert"), ("fs", "frag")):
                    check_translation(
                        args.glslang, header, "portable_" + snippet, target, suffix, work
                    )

        for stage in ("vertex", "fragment"):
            (work / original["stages"][stage]["file"]).write_bytes(
                rewrite_spirv(binaries[stage], strip_names=True)
            )
        generate()
        assert "member_0" in header.read_text(), "stripped members need stable fallback names"
        # A shared vertex entry may emit varyings unused by this fragment entry.
        # Exercise a hole between consumed locations as well as a trailing output.
        vertex_source = (root / "tests/fixtures/portable/glsl/vertex.vert").read_text()
        fragment_source = (root / "tests/fixtures/portable/glsl/fragment.frag").read_text()
        extra_vertex = vertex_source.replace(
            "void main()",
            "layout(location=1) out vec2 unused_middle;\n"
            "layout(location=2) out vec2 consumed;\n"
            "layout(location=3) out vec2 unused_tail;\nvoid main()",
        ).replace(
            "varying_uv = uv;",
            "varying_uv = uv; unused_middle = position; consumed = uv; unused_tail = position;",
        )
        extra_fragment = fragment_source.replace(
            "void main()", "layout(location=2) in vec2 consumed;\nvoid main()"
        ).replace("* params.tint", "* params.tint * consumed.x")

        def compile_stage(stage, suffix, text):
            source = work / ("varyings." + suffix)
            source.write_text(text)
            run(
                [
                    args.glslang,
                    "-V",
                    "--target-env",
                    "vulkan1.1",
                    str(source),
                    "-o",
                    str(work / original["stages"][stage]["file"]),
                ]
            )

        # Sparse vertex inputs must agree with Sokol's contiguous pipeline slots.
        # Keep the stage link at location zero while moving only mesh inputs.
        sparse_vertex = vertex_source.replace("location=0) in", "location=3) in").replace(
            "location=1) in", "location=7) in"
        )
        compile_stage("vertex", "vert", sparse_vertex)
        compile_stage("fragment", "frag", fragment_source)
        for target in TARGETS:
            generate(targets=target)
            stages = json.loads(report.read_text())["targets"][target]
            attributes = stages["vertex"]["spirv"]["inputs"]
            assert {item["name"]: item["location"] for item in attributes} == {
                "position": 0,
                "uv": 1,
            }
            assert {item["location"] for item in stages["vertex"]["spirv"]["outputs"]} == {0}
            assert {item["location"] for item in stages["fragment"]["spirv"]["inputs"]} == {0}

        compile_stage("vertex", "vert", extra_vertex)
        compile_stage("fragment", "frag", extra_fragment)
        for target in TARGETS:
            generate(targets=target)
            stages = json.loads(report.read_text())["targets"][target]
            assert {item["location"] for item in stages["vertex"]["spirv"]["outputs"]} == {
                0,
                1,
                2,
                3,
            }
            assert {item["location"] for item in stages["fragment"]["spirv"]["inputs"]} == {0, 2}
        compile_stage("fragment", "frag", extra_fragment.replace("location=2", "location=4"))
        generate(error="varying type mismatch at location 4")
        compile_stage(
            "fragment", "frag", extra_fragment.replace("in vec2 consumed", "in vec3 consumed")
        )
        generate(error="varying type mismatch at location 2")
        for stage in ("vertex", "fragment"):
            (work / original["stages"][stage]["file"]).write_bytes(binaries[stage])

        renamed = copy.deepcopy(original)
        for stage in ("vertex", "fragment"):
            renamed["stages"][stage]["entry"] = "custom_entry"
            (work / original["stages"][stage]["file"]).write_bytes(
                rewrite_spirv(binaries[stage], entry="custom_entry")
            )
        generate(renamed)
        generate(error="entry point matching")
        for stage in ("vertex", "fragment"):
            (work / original["stages"][stage]["file"]).write_bytes(binaries[stage])

        def bad(change, error):
            data = copy.deepcopy(original)
            change(data)
            generate(data, error=error)

        bad(lambda d: d.update(unknown=True), "unknown field")
        bad(lambda d: d.update(version=2), "constant")
        bad(lambda d: d.update(version=1.0), "expected integer")
        bad(lambda d: d.update(module="bad-name"), "invalid identifier")
        bad(lambda d: d["stages"]["vertex"].update(unknown=1), "unknown field")
        bad(lambda d: d["resources"][0].update(slot=8), "slot must be")
        bad(lambda d: d["resources"][0].update(slot=-1), "below minimum")
        bad(lambda d: d["resources"][0].update(slot=True), "expected integer")
        bad(lambda d: d["resources"][0].update(binding=4294967296), "above maximum")
        bad(lambda d: d["resources"][1].update(slot=2), "duplicate uniform slot")
        bad(lambda d: d["resources"][2].update(binding=9), "duplicate SPIR-V descriptor")
        bad(lambda d: d["resources"][2].pop("sample_type"), "sample_type is required")
        for sample_type in ("sint", "uint", "depth"):
            bad(
                lambda d: d["resources"][2].update(sample_type=sample_type),
                "sample_type does not match",
            )
        bad(
            lambda d: d["resources"][3].update(sampler_type="comparison"),
            "sampler_type does not match",
        )
        bad(lambda d: d["resources"][2].update(kind="storage_buffer"), "sample_type is required")
        bad(lambda d: d["resources"][0].update(kind="storage_buffer"), "missing or wrong metadata")
        bad(lambda d: d["resources"].pop(), "missing or wrong metadata")
        bad(
            lambda d: d["resources"].append(
                dict(stage="vertex", set=99, binding=9, name="unused", kind="uniform", slot=7)
            ),
            "metadata resource is unused",
        )
        generate(
            error="duplicate JSON key",
            text=json.dumps(original).replace('"version": 1', '"version": 1, "version": 1'),
        )
        generate(targets="glsl410:glsl430", error="one target version")
        generate(targets="not_a_target", error="unknown target")
        generate(targets="glsl410:", error="empty target name")
        (work / original["stages"]["vertex"]["file"]).write_bytes(b"bad")
        generate(error="invalid SPIR-V size")
    print(
        "PASS: GLSL input, 11 targets, stripped names, custom entries, determinism and manifest failures"
    )


if __name__ == "__main__":
    main()
