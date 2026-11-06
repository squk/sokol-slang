"""Test CLI metadata generation without invoking shader compilers."""

import contextlib
import io
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

frontend = runpy.run_path(str(Path(__file__).resolve().parents[1] / "tools/sokol-slang"))
parse_args = frontend["parse_args"]
select_programs = frontend["select_programs"]
compile_programs = frontend["compile_programs"]


def resource(name, kind, binding=0, **fields):
    if kind == "texture":
        fields.setdefault("sample_type", "float")
    if kind == "sampler":
        fields.setdefault("sampler_type", "filtering")
    return dict(name=name, kind=kind, set=0, binding=binding, **fields)


class CliTest(unittest.TestCase):
    def test_shader_only_defaults_and_discovered_entries(self):
        args = parse_args(["triangle.slang"])
        self.assertEqual(args.output, Path("triangle.h"))
        programs, batch = select_programs(
            args, [dict(name="vs", stage="vertex"), dict(name="fs", stage="fragment")]
        )
        self.assertFalse(batch)
        self.assertEqual(
            programs["draw"], dict(module="triangle", program="draw", vertex="vs", fragment="fs")
        )
        programs, batch = select_programs(args, [dict(name="cs", stage="compute")])
        self.assertEqual(programs["run"]["compute"], "cs")

    def test_numeric_filename_has_a_valid_default_module(self):
        programs, _ = select_programs(parse_args(["01-shader.slang", "--compute", "cs"]), [])
        self.assertEqual(programs["run"]["module"], "_01_shader")

    def test_program_sets_and_direct_entries(self):
        args = parse_args(
            [
                "shader.slang",
                "--module",
                "example",
                "--program",
                "base:vs:fs",
                "--program",
                "work:cs",
            ]
        )
        programs, batch = select_programs(args, [])
        self.assertTrue(batch)
        self.assertEqual(programs["work"], dict(module="example", program="work", compute="cs"))
        args = parse_args(
            ["shader.slang", "--vertex", "vs", "--fragment", "fs", "--program", "custom"]
        )
        programs, batch = select_programs(args, [])
        self.assertFalse(batch)
        self.assertEqual(programs["custom"]["vertex"], "vs")

    def test_ambiguous_entries_and_duplicate_programs_fail(self):
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            select_programs(
                parse_args(["shader.slang"]),
                [dict(name="a", stage="compute"), dict(name="b", stage="compute")],
            )
        with self.assertRaisesRegex(ValueError, "duplicate program"):
            select_programs(
                parse_args(
                    ["shader.slang", "--program", "draw:vs:fs", "--program", "draw:vs:other"]
                ),
                [],
            )
        with self.assertRaisesRegex(ValueError, "cannot mix"):
            select_programs(
                parse_args(["shader.slang", "--program", "draw:vs:fs", "--vertex", "vs"]), []
            )

    def test_removed_json_flags_and_source_overwrite_fail(self):
        for flags in (["--metadata", "metadata.json"], ["--auto-bindings"], ["-o", "shader.slang"]):
            with (
                self.subTest(flags=flags),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit),
            ):
                parse_args(["shader.slang", *flags])

    def test_settings_reject_duplicates_and_bad_values(self):
        with self.assertRaisesRegex(ValueError, "duplicate sample type"):
            frontend["parse_settings"](
                ["tex=float", "tex=depth"], frontend["SAMPLE_TYPES"], "sample type"
            )
        with self.assertRaisesRegex(ValueError, "expects NAME=TYPE"):
            frontend["parse_settings"](["tex=bad"], frontend["SAMPLE_TYPES"], "sample type")
        with self.assertRaisesRegex(ValueError, "binding expects"):
            frontend["parse_slots"](["tex=2"])


class FrontendTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="frontend test ")
        self.addCleanup(self.directory.cleanup)
        self.work = Path(self.directory.name)
        self.args = parse_args(
            [
                str(self.work / "shader.slang"),
                "--module",
                "example",
                "--program",
                "base:vs:fsBase",
                "--program",
                "full:vs:fsFull",
            ]
        )
        self.args.reflection = self.work / "shader.reflection.json"
        self.args.depfile = self.work / "shader.d"
        self.args.slangc = self.work / "slangc"
        self.args.core = self.work / "sokol-spirv"
        self.args.targets = "metal_macos:wgsl"
        self.args.include_dir = [self.work / "include files"]
        self.args.input.write_text("// Shader compiler calls are mocked.\n")
        self.calls, self.manifests = [], []
        self.fail_core = False
        self.entries = [dict(name="vs", stage="vertex"), dict(name="fs", stage="fragment")]

    def run_tool(self, args, **kwargs):
        self.calls.append(args)
        if "-target" in args:
            binary = Path(args[args.index("-o") + 1])
            binary.write_text(args[args.index("-stage") + 1] if "-stage" in args else "discovery")
            if "-depfile" in args:
                Path(args[args.index("-depfile") + 1]).write_text(
                    str(binary) + ": " + str(self.args.input) + " include\\ files/shared.slang\n"
                )
        else:
            manifest = json.loads(Path(args[args.index("--manifest") + 1]).read_text())
            self.manifests.append(manifest)
            if self.fail_core:
                raise subprocess.CalledProcessError(1, args)
            Path(args[args.index("--output") + 1]).write_text("generated by core\n")
            report = dict(targets={"mock": {}}, resources={})
            reports = (
                {p["program"]: report for p in manifest["programs"]}
                if "programs" in manifest
                else report
            )
            Path(args[args.index("--reflection") + 1]).write_text(json.dumps(reports))

    def inspect(self, args, **kwargs):
        if "-version" in args:
            return "2026.20\n"
        stage = Path(args[-1]).read_text()
        if stage == "discovery":
            return json.dumps(dict(entries=self.entries))
        return json.dumps(dict(entries=[dict(name="main", stage=stage)]))

    def compile(self):
        with (
            patch("subprocess.run", side_effect=self.run_tool),
            patch("subprocess.check_output", side_effect=self.inspect),
        ):
            compile_programs(self.args)

    def test_batch_compiles_shared_stages_once_and_calls_core_once(self):
        self.compile()
        stages = [args for args in self.calls if "-entry" in args]
        self.assertEqual(len(stages), 3)
        self.assertEqual(sum(args[args.index("-entry") + 1] == "vs" for args in stages), 1)
        self.assertTrue(
            all(str((self.work / "include files").resolve()) in args for args in stages)
        )
        self.assertEqual(len(self.manifests), 1)
        self.assertEqual([p["program"] for p in self.manifests[0]["programs"]], ["base", "full"])
        self.assertTrue(all("resources" not in p for p in self.manifests[0]["programs"]))
        self.assertEqual(self.args.output.read_text(), "generated by core\n")
        self.assertIn("include\\ files/shared.slang", self.args.depfile.read_text())
        self.assertNotIn("manifest.json", self.args.depfile.read_text())

    def test_shader_only_discovers_entries_without_backend_policy(self):
        self.args.program = []
        self.args.targets = None
        self.compile()
        self.assertEqual(set(self.manifests[0]["stages"]), {"vertex", "fragment"})
        self.assertEqual(self.manifests[0]["program"], "draw")
        self.assertNotIn("resources", self.manifests[0])
        core_call = next(c for c in self.calls if "--manifest" in c)
        self.assertNotIn("--targets", core_call)

    def test_constraints_and_source_option_are_forwarded_to_core(self):
        self.args.sample_type = ["tex=unfilterable_float"]
        self.args.sampler_type = ["smp=nonfiltering"]
        self.args.binding = ["fragment:tex=9"]
        self.args.source_output = True
        self.compile()
        manifest = self.manifests[0]
        self.assertEqual(
            manifest["policies"],
            {"tex": {"sample_type": "unfilterable_float"}, "smp": {"sampler_type": "nonfiltering"}},
        )
        self.assertEqual(manifest["bindings"], [dict(stage="fragment", name="tex", slot=9)])
        core_call = next(c for c in self.calls if "--manifest" in c)
        self.assertIn("--source", core_call)
        self.assertEqual(core_call[core_call.index("--targets") + 1], self.args.targets)

    def test_core_failure_preserves_frontend_outputs(self):
        self.compile()
        paths = [self.args.output, self.args.reflection, self.args.depfile]
        before = {p: p.read_bytes() for p in paths}
        self.fail_core = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.compile()
        for path in paths:
            self.assertEqual(path.read_bytes(), before[path])

    def test_exported_set_has_one_replay_manifest_and_relative_shared_stages(self):
        self.args.emit_spirv_dir = self.work / "exported"
        self.compile()
        portable = json.loads((self.args.emit_spirv_dir / "manifest.json").read_text())
        files = [info["file"] for p in portable["programs"] for info in p["stages"].values()]
        self.assertEqual(len(set(files)), 3)
        for filename in files:
            self.assertFalse(Path(filename).is_absolute())
            self.assertTrue((self.args.emit_spirv_dir / filename).is_file())

    def test_shader_only_main_writes_only_the_header(self):
        main = frontend["main"]
        with (
            patch.dict(main.__globals__, find_tool=lambda name, override=None: self.work / name),
            patch("subprocess.run", side_effect=self.run_tool),
            patch("subprocess.check_output", side_effect=self.inspect),
        ):
            main([str(self.args.input)])
        self.assertEqual(self.args.output.read_text(), "generated by core\n")
        self.assertEqual({p.name for p in self.work.iterdir()}, {"shader.slang", "shader.h"})


if __name__ == "__main__":
    unittest.main()
