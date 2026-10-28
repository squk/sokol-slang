"""Test slot allocation and compiler orchestration without invoking a compiler."""
import argparse
import copy
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

FRONTEND = Path(__file__).resolve().parents[1] / 'tools/sokol-slang'
frontend = runpy.run_path(str(FRONTEND))
allocate = frontend['allocate_bindings']
compile_programs = frontend['compile_programs']


def resource(name, kind, binding=0, **fields):
    return dict(name=name, kind=kind, set=0, binding=binding, **fields)


class BindingTest(unittest.TestCase):
    def test_shared_resources_receive_distinct_stage_slots(self):
        items = [resource('params', 'uniform'), resource('tex', 'texture', 1), resource('smp', 'sampler', 2)]
        stages = {stage: dict(resources=items) for stage in ('vertex', 'fragment')}
        policies = dict(tex=dict(sample_type='float'), smp=dict(sampler_type='filtering'))
        resources, bindings = allocate(stages, policies)
        for value in resources.values():
            self.assertEqual(set(value['slots']), {'vertex', 'fragment'})
            self.assertEqual(len(set(value['slots'].values())), 2)
        self.assertEqual(len(bindings), 6)
        reversed_stages = {stage: dict(resources=list(reversed(items))) for stage in ('fragment', 'vertex')}
        self.assertEqual(allocate(reversed_stages, policies), (resources, bindings))

    def test_view_and_sampler_spaces_are_separate(self):
        resources, bindings = allocate(dict(fragment=dict(resources=[
            resource('a_tex', 'texture'), resource('b_smp', 'sampler', 1),
            resource('c_buffer', 'storage_buffer', 2, access='read')])),
            dict(a_tex=dict(sample_type='depth'), b_smp=dict(sampler_type='comparison')))
        self.assertEqual(resources['a_tex']['slots']['fragment'], 0)
        self.assertEqual(resources['b_smp']['slots']['fragment'], 0)
        self.assertEqual(resources['c_buffer']['slots']['fragment'], 1)
        self.assertEqual(bindings[-1]['access'], 'read')

    def test_limits_include_all_stages(self):
        for kind, limit in [('uniform', 8), ('storage_buffer', 32), ('sampler', 12)]:
            stages = {stage: dict(resources=[resource(f'r_{i}', kind, i) for i in indices])
                      for stage, indices in [('vertex', range(limit//2)), ('fragment', range(limit//2, limit+1))]}
            policies = {f'r_{i}': dict(sampler_type='filtering') for i in range(limit+1)} if kind == 'sampler' else {}
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, 'exceeds Sokol'):
                allocate(stages, policies)

    def test_invalid_policies_and_duplicate_descriptors_fail(self):
        for policy in ({}, {'tex': dict(sample_type='bad')}, {'tex': dict(sampler_type='filtering')}):
            with self.subTest(policy=policy), self.assertRaisesRegex(ValueError, 'sampling policy'):
                allocate(dict(fragment=dict(resources=[resource('tex', 'texture')])), policy)
        with self.assertRaisesRegex(ValueError, 'duplicate or conflicting'):
            allocate(dict(vertex=dict(resources=[resource('r', 'uniform'), resource('r', 'uniform', 1)])), {})


class FrontendTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='frontend test ')
        self.addCleanup(self.directory.cleanup)
        self.work = Path(self.directory.name)
        self.args = argparse.Namespace(input=self.work/'shader.slang', metadata=self.work/'shader.json',
            output=self.work/'shader.h', reflection=self.work/'shader.reflection.json', depfile=self.work/'shader.d',
            slangc=self.work/'slangc', core=self.work/'sokol-spirv', targets='metal_macos:wgsl',
            auto_bindings=True, emit_spirv_dir=None, include_dir=[self.work/'include files'])
        self.args.input.write_text('// Compiler calls are mocked in this test.\n')
        self.meta = dict(programs={name: dict(module='example_'+name, program='draw', vertex='vs', fragment=fs)
                                  for name, fs in [('base', 'fsBase'), ('full', 'fsFull')]}, resources={})
        self.calls = []
        self.fail_program = None

    def run_tool(self, args, **kwargs):
        self.calls.append(args)
        if '-entry' in args:
            binary = Path(args[args.index('-o')+1])
            binary.write_text(args[args.index('-stage')+1])
            Path(args[args.index('-depfile')+1]).write_text(str(binary)+': '+str(self.args.input)+' include\\ files/shared.slang\n')
        else:
            manifest = json.loads(Path(args[args.index('--manifest')+1]).read_text())
            if self.fail_program and manifest['module'] == 'example_'+self.fail_program:
                raise subprocess.CalledProcessError(1, args)
            Path(args[args.index('--output')+1]).write_text(manifest['module'])
            Path(args[args.index('--reflection')+1]).write_text(json.dumps(dict(targets={})))

    def inspect(self, args, **kwargs):
        if '-version' in args:
            return '2026.20\n'
        stage = Path(args[-1]).read_text()
        return json.dumps(dict(entries=[dict(name='main', stage=stage)], resources=[]))

    def compile(self):
        self.args.metadata.write_text(json.dumps(self.meta))
        with patch('subprocess.run', side_effect=self.run_tool), \
                patch('subprocess.check_output', side_effect=self.inspect):
            compile_programs(self.args)

    def test_batch_reuses_stages_and_preserves_dependencies(self):
        self.compile()
        stages = [args for args in self.calls if '-entry' in args]
        self.assertEqual(len(stages), 3)
        self.assertEqual(sum(args[args.index('-entry')+1] == 'vs' for args in stages), 1)
        self.assertTrue(all(str((self.work/'include files').resolve()) in args for args in stages))
        reports = json.loads(self.args.reflection.read_text())
        self.assertEqual(set(reports), {'base', 'full'})
        self.assertEqual(self.args.output.read_text(), 'example_base\nexample_full')
        deps = self.args.depfile.read_text()
        self.assertIn('include\\ files/shared.slang', deps)
        self.assertIn('frontend\\ test\\ ', deps)

    def test_failed_later_program_keeps_all_previous_outputs(self):
        self.compile()
        before = {path: path.read_bytes() for path in (self.args.output, self.args.reflection, self.args.depfile)}
        self.fail_program = 'full'
        with self.assertRaises(subprocess.CalledProcessError):
            self.compile()
        for path, data in before.items():
            self.assertEqual(path.read_bytes(), data)

    def test_exported_programs_have_portable_manifest_paths(self):
        self.args.emit_spirv_dir = self.work/'exported'
        self.compile()
        for name in self.meta['programs']:
            folder = self.args.emit_spirv_dir/name
            manifest = json.loads((folder/'manifest.json').read_text())
            for info in manifest['stages'].values():
                self.assertFalse(Path(info['file']).is_absolute())
                self.assertTrue((folder/info['file']).is_file())

    def test_explicit_single_program_still_uses_requested_metadata(self):
        self.args.auto_bindings = False
        self.meta = dict(module='single', program='draw', vertex='vs', fragment='fs', resources={})
        self.compile()
        self.assertEqual(self.args.output.read_text(), 'single')
        self.assertIn('targets', json.loads(self.args.reflection.read_text()))
        self.assertNotIn('draw', json.loads(self.args.reflection.read_text()))

    def test_compute_uses_compute_targets_without_raster_stages(self):
        self.args.targets = None
        self.meta = dict(module='compute', program='dispatch', compute='cs', resources={})
        self.compile()
        stages = [args for args in self.calls if '-entry' in args]
        self.assertEqual(len(stages), 1)
        self.assertEqual(stages[0][stages[0].index('-stage')+1], 'compute')
        targets = json.loads(self.args.reflection.read_text())['compiler']['targets']
        self.assertIn('glsl430', targets)
        self.assertIn('glsl310es', targets)
        self.assertNotIn('glsl410', targets)

    def test_conflicting_prefixes_fail_before_compiling(self):
        self.meta['programs']['full'] = copy.deepcopy(self.meta['programs']['base'])
        with self.assertRaisesRegex(ValueError, 'distinct module/program'):
            self.compile()
        self.assertEqual(self.calls, [])


if __name__ == '__main__':
    unittest.main()
