"""Keep fragment storage images outside the full Sokol view namespace."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--core", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="sokol fragment storage views ") as directory:
        work = Path(directory)
        source = work / "full_views.slang"
        declarations = [f"Texture2D<float4> texture{i:02};" for i in range(32)]
        declarations += [f'[format("r32ui")] RWTexture2D<uint> storage{i};' for i in range(4)]
        reads = [f"color += texture{i:02}.Load(int3(pixel, 0));" for i in range(32)]
        writes = [f"storage{i}[pixel] += asuint(color[{i}]);" for i in range(4)]
        source.write_text("\n".join(declarations) + "\n" + """
[shader("vertex")]
float4 vertexMain(uint id : SV_VulkanVertexID) : SV_Position {
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0.0, 1.0);
}
[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target0 {
    uint2 pixel = uint2(position.xy);
    float4 color = 0.0;
""" + "\n".join(reads + writes) + "\nreturn color;\n}\n")
        report = work / "shader.json"
        subprocess.run(
            [
                sys.executable,
                str(root / "tools/sokol-slang"),
                str(source),
                "--core",
                args.core,
                "--source",
                "--module",
                "full_views",
                # One program: --program NAME:ENTRIES would make a program set, whose
                # reflection is keyed by program name instead of holding "layout".
                "--vertex",
                "vertexMain",
                "--fragment",
                "fragmentMain",
                "--targets",
                "metal_macos:spirv_vk:hlsl5",
                "--output",
                str(work / "shader.h"),
                "--reflection",
                str(report),
            ],
            check=True,
        )
        layout = json.loads(report.read_text())["layout"]
        assert layout["max_view_bindslots"] == 32
        textures = layout["textures"]
        assert len(textures) == 32
        assert {item["slot"] for item in textures} == set(range(32))
        assert {item["msl_texture_n"] for item in textures} == set(range(32))
        assert layout["storage_images"] == []
        storage = layout["fragment_storage_images"]
        assert len(storage) == 4
        assert {item["slot"] for item in storage} == set(range(4))
        assert {item["msl_texture_n"] for item in storage} == set(range(32, 36))
        assert {item["access_format"] for item in storage} == {"SG_PIXELFORMAT_R32UI"}
        # D3D11 pixel shader UAVs take the top four of the output merger's eight slots.
        assert {item["hlsl_register_u_n"] for item in storage} == set(range(4, 8))
        header = (work / "shader.h").read_text()
        for register in range(4, 8):
            assert f"register(u{register})" in header, register

        # hlsl5 has no rasterizer-ordered views (shader model 5.1).
        ordered = work / "ordered.slang"
        ordered.write_text("""
[format("r32ui")] RasterizerOrderedTexture2D<uint> samples;
[shader("vertex")]
float4 vertexMain(uint id : SV_VulkanVertexID) : SV_Position {
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0.0, 1.0);
}
[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target0 {
    samples[uint2(position.xy)] += 1;
    return 0.0;
}
""")
        result = subprocess.run(
            [
                sys.executable,
                str(root / "tools/sokol-slang"),
                str(ordered),
                "--core",
                args.core,
                "--source",
                "--module",
                "ordered",
                "--vertex",
                "vertexMain",
                "--fragment",
                "fragmentMain",
                "--targets",
                "hlsl5",
                "--output",
                str(work / "ordered.h"),
                "--reflection",
                str(work / "ordered.json"),
            ],
            capture_output=True,
            text=True,
        )
        assert result.returncode != 0
        assert (
            "raster-ordered fragment storage images require Metal or Vulkan" in result.stderr
        ), result.stderr


if __name__ == "__main__":
    main()
