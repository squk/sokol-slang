"""Check storage-image access/format contracts and backend binding allocation."""

import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from test_support import run, rewrite_spirv, check_translation


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--core", required=True)
    parser.add_argument("--glslang", required=True)
    parser.add_argument("--slangc")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    original = json.loads((root / "tests/fixtures/storage-image/glsl/manifest.json").read_text())
    with tempfile.TemporaryDirectory(prefix="sokol storage images ") as directory:
        work = Path(directory)
        binary = work / "generic_storage_image.spv"
        run(
            [
                args.glslang,
                "-V",
                "--target-env",
                "vulkan1.1",
                str(root / "tests/fixtures/storage-image/glsl/shader.comp"),
                "-o",
                str(binary),
            ]
        )
        spirv = binary.read_bytes()
        manifest = work / "manifest.json"
        header, report = work / "shader.h", work / "shader.json"
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

        def generate(data=original, error=None):
            manifest.write_text(json.dumps(data))
            old = header.read_bytes() if header.exists() else None
            result = subprocess.run(command, capture_output=True, text=True)
            if error:
                assert result.returncode != 0 and error in result.stderr, result.stderr
                assert header.read_bytes() == old, "failed generation replaced header"
            else:
                assert result.returncode == 0, result.stderr

        def verify(auto_slots=False):
            targets = json.loads(report.read_text())["targets"]
            assert len(targets) == 8
            for target, stages in targets.items():
                assert stages["compute"]["workgroup_size"] == [8, 4, 1]
                bindings = {b["name"]: b for b in stages["compute"]["bindings"]}
                for name, slot, access, fmt in [
                    ("colors", 5, "write", "rgba8"),
                    ("counters", 9, "read_write", "r32ui"),
                ]:
                    image = bindings["compute_" + name]
                    assert (image["kind"], image["access"], image["format"]) == (
                        "storage_image",
                        access,
                        fmt,
                    )
                    if not auto_slots:
                        assert image["slot"] == slot
                view_slots = [b["slot"] for b in bindings.values() if b["kind"] != "uniform"]
                assert len(set(view_slots)) == len(view_slots)
                # Images and writable buffers share UAV registers, but GL image units
                # and Metal texture indices have separate namespaces from buffers.
                names = (
                    ("colors", "resultRecords", "counters")
                    if target == "hlsl5"
                    else ("colors", "counters")
                )
                native = [bindings["compute_" + name]["binding"] for name in names]
                assert len(set(native)) == len(native), f"{target}: native binding collision"
                if target.startswith(("glsl", "hlsl")):
                    check_translation(
                        args.glslang, header, "storage_image_cs", target, "comp", work
                    )

        generate()
        verify()
        first = header.read_bytes()
        generate()
        assert header.read_bytes() == first
        inspected = json.loads(run([args.core, "--inspect", str(binary)]))["resources"]
        assert {(r["format"], r["access"]) for r in inspected if r["kind"] == "storage_image"} == {
            ("rgba8", "write"),
            ("r32ui", "read_write"),
        }

        def bad(change, error):
            data = copy.deepcopy(original)
            change(data)
            generate(data, error)

        bad(lambda d: d["resources"][1].pop("access"), "access must be explicit")
        bad(lambda d: d["resources"][1].pop("format"), "format is required")
        bad(lambda d: d["resources"][1].update(access="read"), "access must be explicit")
        bad(lambda d: d["resources"][1].update(access="read_write"), "access does not match")
        bad(lambda d: d["resources"][2].update(access="write"), "access does not match")
        bad(lambda d: d["resources"][1].update(format="r32ui"), "format does not match")
        bad(lambda d: d["resources"][1].update(format="depth"), "allowed enum")
        bad(lambda d: d["resources"][0].update(format="r32ui"), "format is required only")
        bad(lambda d: d["resources"][3].update(access="write"), "storage buffer access")
        bad(lambda d: d["resources"][1].update(stage="fragment"), "only in compute")
        bad(lambda d: d["resources"][1].update(slot=7), "duplicate view slot")
        binary.write_bytes(rewrite_spirv(spirv, strip_names=True))
        generate()
        verify()
        binary.write_bytes(spirv)

        # Exercise the complete Sokol storage-format set through each translator.
        formats = [
            "rgba8",
            "rgba8_snorm",
            "rgba8ui",
            "rgba8i",
            "rgba16ui",
            "rgba16i",
            "rgba16f",
            "r32ui",
            "r32i",
            "r32f",
            "rg32ui",
            "rg32i",
            "rg32f",
            "rgba32ui",
            "rgba32i",
            "rgba32f",
        ]
        simple = copy.deepcopy(original)
        simple["resources"] = [copy.deepcopy(original["resources"][1])]
        source = work / "format.comp"
        for fmt in formats:
            prefix = "u" if fmt.endswith("ui") else "i" if fmt.endswith("i") else ""
            vector = prefix + "vec4"
            simple["resources"][0]["format"] = fmt
            source.write_text(
                "#version 450\nlayout(local_size_x=32) in;\n"
                f"layout(set=2,binding=6,{fmt}) uniform writeonly {prefix}image2D image;\n"
                f"void main() {{ imageStore(image, ivec2(gl_GlobalInvocationID.xy), {vector}(1)); }}\n"
            )
            run([args.glslang, "-V", str(source), "-o", str(binary)])
            generate(simple)
            for stages in json.loads(report.read_text())["targets"].values():
                assert stages["compute"]["bindings"][0]["format"] == fmt
            expected = fmt.upper().replace("_SNORM", "SN")
            if expected.endswith("I") and not expected.endswith("UI"):
                expected = expected[:-1] + "SI"
            assert (
                header.read_text().count(
                    "storage_image.access_format = SG_PIXELFORMAT_" + expected + ";"
                )
                == 8
            )
        binary.write_bytes(spirv)

        if args.slangc:
            frontend = [
                sys.executable,
                str(root / "tools/sokol-slang"),
                "--source",
                "--core",
                args.core,
                "--input",
                str(root / "tests/fixtures/storage-image/shader.slang"),
                "--module",
                "storage_image",
                "--output",
                str(header),
                "--reflection",
                str(report),
                "--depfile",
                str(work / "shader.d"),
            ]
            run(frontend)
            verify(auto_slots=True)
    print(
        "PASS: storage-image formats/access, eight targets, mixed UAV bindings, stripped names and diagnostics"
    )


if __name__ == "__main__":
    main()
