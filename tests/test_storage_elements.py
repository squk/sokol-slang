"""Compile storage buffers whose elements are scalars or vectors instead of structs."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from test_support import check_translation, decode_source

TARGETS = ("metal_macos", "glsl430", "hlsl5", "wgsl", "spirv_vk")

# Resource -> (stage, record struct, record size, read-only)
COMPUTE = {
    "colors": ("compute", "sokol_float4_record", 16, True),
    "offsets": ("compute", "sokol_float3_record", 16, True),
    "weights": ("compute", "sokol_float_record", 4, True),
    "pairs": ("compute", "sokol_int2_record", 8, True),
    "rawInput": ("compute", "sokol_uint_record", 4, True),
    "rawOutput": ("compute", "sokol_uint_record", 4, False),
    "counts": ("compute", "sokol_uint_record", 4, False),
}
RASTER = {
    "offsets": ("vertex", "sokol_float4_record", 16, True),
    "scales": ("vertex", "sokol_float_record", 4, True),
    "colors": ("fragment", "sokol_float4_record", 16, True),
}
# Record -> (C alignment, C struct members)
C_RECORDS = {
    "sokol_float4_record": (16, ["float value[4];"]),
    "sokol_float3_record": (16, ["float value[3];", "uint8_t _pad_12[4];"]),
    "sokol_float_record": (4, ["float value;"]),
    "sokol_int2_record": (8, ["int32_t value[2];"]),
    "sokol_uint_record": (4, ["uint32_t value;"]),
}
GLSLANG_STAGES = {"cs": "comp", "vs": "vert", "fs": "frag"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--core", required=True)
    parser.add_argument("--glslang")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="sokol storage elements ") as directory:
        work = Path(directory)

        def compile_shader(source, module, error=None):
            shader = work / (module + ".slang")
            header = work / (module + ".h")
            report = work / (module + ".json")
            shader.write_text(source)
            result = subprocess.run(
                [
                    sys.executable,
                    str(root / "tools/sokol-slang"),
                    "--source",
                    "--core",
                    args.core,
                    "--targets",
                    ":".join(TARGETS),
                    "--module",
                    module,
                    "--input",
                    str(shader),
                    "--output",
                    str(header),
                    "--reflection",
                    str(report),
                ],
                cwd=work,
                text=True,
                capture_output=True,
            )
            if error:
                assert result.returncode != 0, f"{module}: invalid shader succeeded"
                assert error in result.stderr, result.stderr
                return None, None
            assert result.returncode == 0, result.stderr
            return header, json.loads(report.read_text())

        def check(module, expected, stages):
            source = (root / "tests/shaders" / (module + ".slang")).read_text()
            header, report = compile_shader(source, module)
            text = header.read_text()
            assert set(report["targets"]) == set(TARGETS)

            # Layout: every buffer is an array of the shared wrapper record.
            buffers = {}
            for buffer in report["layout"]["storage_buffers"]:
                stage, name = buffer["name"].split("_", 1)
                buffers[name] = (
                    stage,
                    buffer["struct_name"],
                    buffer["record_size"],
                    buffer["readonly"],
                )
            assert buffers == expected, buffers

            # C header: one typedef per record, with std430 padding.
            for record in {item[1] for item in expected.values()}:
                typedefs = re.findall(
                    r"SOKOL_SHDC_ALIGN\((\d+)\) typedef struct "
                    + module
                    + "_"
                    + record
                    + r"_t \{(.*?)\}",
                    text,
                    re.S,
                )
                assert len(typedefs) == 1, f"{record}: {len(typedefs)} C typedefs"
                align, body = typedefs[0]
                members = [line.strip() for line in body.strip().splitlines()]
                assert (int(align), members) == C_RECORDS[record], (record, align, members)

            for target in TARGETS:
                stage_reports = report["targets"][target]
                assert set(stage_reports) == set(stages)
                for stage in stages:
                    stage_report = stage_reports[stage]
                    count = sum(
                        1 for b in stage_report["bindings"] if b["kind"] == "storage_buffer"
                    )
                    assert count == sum(1 for item in expected.values() if item[0] == stage)
                    # SPIR-V reflection sees the wrapper records with member value.
                    records = {
                        t["name"]: t
                        for t in stage_report["spirv"]["types"].values()
                        if "members" in t
                    }
                    for name, item in expected.items():
                        if item[0] == stage:
                            members = records[item[1]]["members"]
                            assert [m["name"] for m in members] == ["value"], members
                            assert members[0]["offset"] == 0

            # Backend sources declare the record structs.
            for target in ("metal_macos", "glsl430", "hlsl5", "wgsl"):
                found = re.findall(r"(\w+)_source_" + target + r"\[\d+\] = \{(.*?)\};", text, re.S)
                assert len(found) == len(stages), f"{target}: {len(found)} sources"
                for symbol, data in found:
                    snippet = symbol.rsplit("_", 1)[1]
                    code = decode_source(data).decode()
                    stage = {"cs": "compute", "vs": "vertex", "fs": "fragment"}[snippet]
                    for item in expected.values():
                        if item[0] == stage and target in ("metal_macos", "glsl430"):
                            assert item[1] in code, f"{target} {stage}: missing {item[1]}"
                    if args.glslang and target in ("glsl430", "hlsl5"):
                        check_translation(
                            args.glslang, header, symbol, target, GLSLANG_STAGES[snippet], work
                        )

        check("storage_elements", COMPUTE, ["compute"])
        check("storage_elements_raster", RASTER, ["vertex", "fragment"])

        unsupported = """
StructuredBuffer<{element}> values;
RWStructuredBuffer<float> results;
[shader("compute")]
[numthreads(32, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {{ results[id.x] = {value}; }}
"""
        compile_shader(
            unsupported.format(element="float4x4", value="values[id.x][1][2]"),
            "matrix_elements",
            "matrix elements are not supported",
        )
        compile_shader(
            unsupported.format(element="double", value="float(values[id.x])"),
            "double_elements",
            "only 32-bit int, uint and float elements are supported",
        )
    print(
        "PASS: scalar/vector storage buffer records, ByteAddressBuffer, vertex/fragment/compute "
        "stages, C layouts and unsupported element diagnostics"
    )


if __name__ == "__main__":
    main()
