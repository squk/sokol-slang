"""Verify bindless texture and sampler tables across compute and raster programs."""

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
    slang = root / "tools/sokol-slang"

    with tempfile.TemporaryDirectory(prefix="sokol bindless ") as directory:
        work = Path(directory)

        # 1. Compute program combining full 32 Sokol views + ray query + bindless tables
        #    + NonUniformResourceIndex, compiled to real Metal bytecode (on Darwin) and
        #    validated Vulkan SPIR-V.
        compute_src = work / "pt_bindless.slang"
        views = [f"Texture2D<float4> view{i:02};" for i in range(31)]
        reads = [f"sum += view{i:02}.Load(int3(tid.xy, 0));" for i in range(31)]
        compute_src.write_text(
            "\n".join(views)
            + """
RWStructuredBuffer<float4> out_pixels;
RaytracingAccelerationStructure scene_tlas;
Texture2D<float4> material_textures[];
SamplerState material_samplers[];

[shader("compute")]
[numthreads(8, 4, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID) {
    float4 sum = 0.0;
"""
            + "\n".join(reads)
            + """
    RayDesc ray;
    ray.Origin = float3(0.0, 0.0, -1.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.001;
    ray.TMax = 100.0;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene_tlas, RAY_FLAG_NONE, 0xff, ray);
    q.Proceed();
    uint tex_id = tid.x + (q.CommittedStatus() != COMMITTED_NOTHING ? 1u : 0u);
    uint smp_id = tid.y & 3u;
    sum += material_textures[NonUniformResourceIndex(tex_id)].SampleLevel(
        material_samplers[NonUniformResourceIndex(smp_id)], float2(0.5, 0.5), 0.0);
    out_pixels[tid.x] = sum;
}
"""
        )
        compute_report = work / "compute.json"
        subprocess.run(
            [
                sys.executable,
                str(slang),
                str(compute_src),
                "--core",
                args.core,
                "--profile",
                "spirv_1_5",
                "--capability",
                "spvRayQueryKHR",
                "--capability",
                "spvShaderNonUniform",
                "--targets",
                "metal_macos:spirv_vk",
                "--output",
                str(work / "compute.h"),
                "--reflection",
                str(compute_report),
            ],
            check=True,
        )
        layout = json.loads(compute_report.read_text())["layout"]
        assert layout["max_bindless_slots"] == 4
        assert len(layout["textures"]) == 31
        assert len(layout["storage_buffers"]) == 1
        assert len(layout["acceleration_structures"]) == 1
        assert layout["acceleration_structures"][0]["msl_buffer_n"] == 16
        assert len(layout["bindless_textures"]) == 1
        assert len(layout["bindless_samplers"]) == 1
        btex = layout["bindless_textures"][0]
        bsmp = layout["bindless_samplers"][0]
        assert btex["stage"] == "SG_SHADERSTAGE_COMPUTE"
        assert btex["image_type"] == "SG_IMAGETYPE_2D"
        assert btex["sample_type"] == "SG_IMAGESAMPLETYPE_FLOAT"
        assert btex["msl_buffer_n"] == 23 + btex["slot"]
        assert btex["spirv_set_n"] == 2 + btex["slot"]
        assert btex["spirv_binding_n"] == 0
        assert bsmp["stage"] == "SG_SHADERSTAGE_COMPUTE"
        assert bsmp["sampler_type"] == "SG_SAMPLERTYPE_FILTERING"
        assert bsmp["msl_buffer_n"] == 23 + bsmp["slot"]
        assert bsmp["spirv_set_n"] == 2 + bsmp["slot"]
        assert bsmp["spirv_binding_n"] == 0
        assert btex["slot"] != bsmp["slot"]

        # 2. Raster program with bindless tables in BOTH vertex and fragment stages,
        #    verifying Metal vertex buffer base 19..22 vs fragment buffer base 23..26
        #    and distinct Vulkan descriptor sets 2..5 across stages.
        raster_src = work / "raster_bindless.slang"
        raster_src.write_text("""
Texture2D<float4> vs_tex[];
Texture2D<float4> fs_tex[];
SamplerState fs_smp[];

struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    nointerpolation uint mat_id : TEXCOORD1;
};

[shader("vertex")]
VsOut vertexMain(uint id : SV_VulkanVertexID) {
    VsOut o;
    float4 disp = vs_tex[NonUniformResourceIndex(id)].Load(int3(0, 0, 0));
    o.pos = float4((id == 1 ? 3.0 : -1.0) + disp.x * 1e-6, id == 2 ? 3.0 : -1.0, 0.0, 1.0);
    o.uv = float2(0.5, 0.5);
    o.mat_id = id;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VsOut i) : SV_Target0 {
    return fs_tex[NonUniformResourceIndex(i.mat_id)].Sample(
        fs_smp[NonUniformResourceIndex(i.mat_id & 1u)], i.uv);
}
""")
        raster_report = work / "raster.json"
        subprocess.run(
            [
                sys.executable,
                str(slang),
                str(raster_src),
                "--core",
                args.core,
                "--profile",
                "spirv_1_5",
                "--capability",
                "spvShaderNonUniform",
                "--targets",
                "metal_macos:metal_ios:metal_sim:spirv_vk",
                "--output",
                str(work / "raster.h"),
                "--reflection",
                str(raster_report),
            ],
            check=True,
        )
        rlayout = json.loads(raster_report.read_text())["layout"]
        assert len(rlayout["bindless_textures"]) == 2
        assert len(rlayout["bindless_samplers"]) == 1
        by_stage = {t["stage"]: t for t in rlayout["bindless_textures"]}
        vs_btex = by_stage["SG_SHADERSTAGE_VERTEX"]
        fs_btex = by_stage["SG_SHADERSTAGE_FRAGMENT"]
        fs_bsmp = rlayout["bindless_samplers"][0]
        assert vs_btex["msl_buffer_n"] == 19 + vs_btex["slot"]
        assert fs_btex["msl_buffer_n"] == 23 + fs_btex["slot"]
        assert fs_bsmp["msl_buffer_n"] == 23 + fs_bsmp["slot"]
        assert len({vs_btex["spirv_set_n"], fs_btex["spirv_set_n"], fs_bsmp["spirv_set_n"]}) == 3

        # 3. Unsupported targets (glsl430, hlsl5, wgsl) reject bindless tables cleanly.
        for bad_target in ("glsl430", "hlsl5", "wgsl"):
            completed = subprocess.run(
                [
                    sys.executable,
                    str(slang),
                    str(raster_src),
                    "--core",
                    args.core,
                    "--source",
                    "--profile",
                    "spirv_1_5",
                    "--capability",
                    "spvShaderNonUniform",
                    "--targets",
                    bad_target,
                    "--output",
                    str(work / f"bad_{bad_target}.h"),
                ],
                capture_output=True,
                text=True,
            )
            assert completed.returncode != 0
            assert "bindless tables require Metal or Vulkan" in completed.stderr


if __name__ == "__main__":
    main()
