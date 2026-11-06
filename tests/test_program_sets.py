"""Exercise shared binding allocation and program-set generation through the core."""

import argparse
import copy
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()
    original = json.loads(args.manifest.read_text())
    for program in original["programs"]:
        for stage in program["stages"].values():
            stage["file"] = str((args.manifest.parent / stage["file"]).resolve())
    with tempfile.TemporaryDirectory(prefix="sokol program sets ") as directory:
        work = Path(directory)
        manifest = work / "programs.json"
        header, report, depfile = (work / name for name in ("shader.h", "shader.json", "shader.d"))
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
            str(depfile),
        ]

        def generate(document, error=None):
            manifest.write_text(json.dumps(document))
            before = {p: p.read_bytes() for p in (header, report, depfile) if p.exists()}
            result = subprocess.run(command, capture_output=True, text=True)
            if error:
                assert result.returncode and error in result.stderr, result.stderr
                assert all(
                    path.read_bytes() == data for path, data in before.items()
                ), "failed set replaced outputs"
            else:
                assert result.returncode == 0, result.stderr
            return json.loads(report.read_text()) if report.exists() else None

        reports = generate(original)
        first = header.read_bytes()
        assert set(reports) == {"base", "full", "work"}
        assert "glsl410" in reports["base"]["targets"]
        assert "glsl430" in reports["work"]["targets"]
        for name in ("base", "full"):
            resources = reports[name]["resources"]
            assert (
                resources["params"]["slots"]["vertex"] != resources["params"]["slots"]["fragment"]
            )
            assert resources["tex"]["sample_type"] == "float"
            assert resources["smp"]["sampler_type"] == "filtering"
        assert reports["work"]["resources"]["resultRecords"]["access"] == "read_write"
        for program in original["programs"]:
            for stage in program["stages"].values():
                assert Path(stage["file"]).name in depfile.read_text()
        generate(original)
        assert header.read_bytes() == first, "program-set header is not deterministic"

        single = dict(original["programs"][0], version=1, module="programset")
        generate(single)
        expected = header.read_bytes()
        direct = work / "direct" / "shader.h"
        direct_command = [
            args.core,
            "--source",
            "--vertex",
            single["stages"]["vertex"]["file"],
            "--fragment",
            single["stages"]["fragment"]["file"],
            "--module",
            "programset",
            "--program",
            "base",
            "-o",
            str(direct),
        ]
        result = subprocess.run(direct_command, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert direct.read_bytes() == expected, "CLI files and manifest generated different headers"
        assert list(direct.parent.iterdir()) == [direct], "CLI wrote unrequested sidecars"
        compute = work / "123-compute.h"
        result = subprocess.run(
            [
                args.core,
                "--source",
                "--compute",
                original["programs"][2]["stages"]["compute"]["file"],
                "-o",
                str(compute),
            ],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, result.stderr
        assert "_123_compute_run_shader_desc" in compute.read_text()
        result = subprocess.run(
            direct_command + ["--manifest", str(manifest)], capture_output=True, text=True
        )
        assert result.returncode and "cannot be combined" in result.stderr

        constrained = copy.deepcopy(original)
        constrained["bindings"] = [dict(stage="vertex", name="params", slot=0)]
        constrained["policies"] = {
            "tex": {"sample_type": "unfilterable_float"},
            "smp": {"sampler_type": "nonfiltering"},
        }
        reports = generate(constrained)
        for name in ("base", "full"):
            resources = reports[name]["resources"]
            assert resources["params"]["slots"] == {"vertex": 0, "fragment": 1}
            assert resources["tex"]["sample_type"] == "unfilterable_float"
            assert resources["smp"]["sampler_type"] == "nonfiltering"
        result = subprocess.run(
            direct_command
            + [
                "--binding",
                "vertex:params=0",
                "--sample-type",
                "tex=unfilterable_float",
                "--sampler-type",
                "smp=nonfiltering",
                "--reflection",
                str(direct.with_suffix(".json")),
            ],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, result.stderr
        assert (
            json.loads(direct.with_suffix(".json").read_text())["resources"]
            == reports["base"]["resources"]
        )
        bad = copy.deepcopy(constrained)
        bad["bindings"].append(dict(stage="fragment", name="params", slot=0))
        generate(bad, "duplicate uniform slot")
        bad = copy.deepcopy(constrained)
        bad["bindings"][0]["slot"] = 8
        generate(bad, "slot must be")
        bad = copy.deepcopy(constrained)
        bad["bindings"].append(dict(stage="fragment", name="missing", slot=7))
        generate(bad, "no reflected resource")
        bad = copy.deepcopy(original)
        bad["programs"][1]["stages"]["fragment"]["entry"] = "missing"
        generate(bad, "entry")
        bad = copy.deepcopy(original)
        bad["programs"][1]["program"] = "base"
        generate(bad, "duplicate program")
        bad = copy.deepcopy(original)
        bad["programs"] = []
        generate(bad, "too few items")
        bad = copy.deepcopy(original)
        bad["programs"][0]["surprise"] = 1
        generate(bad, "unknown field")
        assert not list(work.glob("shader.h.*.tmp*")), "temporary program-set outputs leaked"
    print(
        "PASS: shared core allocation, policies, slot reservation, mixed programs and failure preservation"
    )


if __name__ == "__main__":
    main()
