"""Test CLI metadata generation without invoking shader compilers."""
import copy
import contextlib
import io
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

frontend = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'tools/sokol-slang'))
allocate = frontend['allocate_bindings']
parse_args = frontend['parse_args']
select_programs = frontend['select_programs']
compile_programs = frontend['compile_programs']


def resource(name, kind, binding=0, **fields):
    if kind == 'texture': fields.setdefault('sample_type', 'float')
    if kind == 'sampler': fields.setdefault('sampler_type', 'filtering')
    return dict(name=name, kind=kind, set=0, binding=binding, **fields)


class BindingTest(unittest.TestCase):
    def test_shared_resources_receive_distinct_stage_slots(self):
        items = [resource('params', 'uniform'), resource('tex', 'texture', 1), resource('smp', 'sampler', 2)]
        stages = {stage: dict(resources=items) for stage in ('vertex', 'fragment')}
        resources, bindings = allocate(stages, {})
        for value in resources.values():
            self.assertEqual(set(value['slots']), {'vertex', 'fragment'})
            self.assertEqual(len(set(value['slots'].values())), 2)
        self.assertEqual(len(bindings), 6)
        reversed_stages = {stage: dict(resources=list(reversed(items))) for stage in ('fragment', 'vertex')}
        self.assertEqual(allocate(reversed_stages, {}), (resources, bindings))

    def test_reflection_supplies_policy_access_and_format(self):
        items = [resource('shadow', 'texture', sample_type='depth'),
                 resource('compare', 'sampler', sampler_type='comparison'),
                 resource('image', 'storage_image', access='write', format='rgba8')]
        resources, bindings = allocate(dict(fragment=dict(resources=items)), {})
        self.assertEqual(resources['shadow']['sample_type'], 'depth')
        self.assertEqual(resources['compare']['sampler_type'], 'comparison')
        self.assertEqual(resources['image']['format'], 'rgba8')
        self.assertEqual(resources['image']['access'], 'write')

    def test_policy_overrides(self):
        resources, bindings = allocate(dict(fragment=dict(resources=[resource('tex', 'texture'), resource('smp', 'sampler')])),
            dict(tex=dict(sample_type='unfilterable_float'), smp=dict(sampler_type='nonfiltering')))
        self.assertEqual(resources['tex']['sample_type'], 'unfilterable_float')
        self.assertEqual(resources['smp']['sampler_type'], 'nonfiltering')
        self.assertEqual(bindings[0]['sampler_type'], 'nonfiltering')

    def test_fixed_slots_are_reserved_before_automatic_allocation(self):
        resources, _ = allocate(dict(vertex=dict(resources=[resource('a', 'uniform')]),
                                      fragment=dict(resources=[resource('b', 'uniform'), resource('tex', 'texture')])),
                                {}, {('fragment', 'b'): 0, ('fragment', 'tex'): 0})
        self.assertEqual(resources['a']['slots']['vertex'], 1)
        self.assertEqual(resources['b']['slots']['fragment'], 0)
        self.assertEqual(resources['tex']['slots']['fragment'], 0)

    def test_invalid_slots_policies_and_duplicates(self):
        stages = dict(vertex=dict(resources=[resource('a', 'uniform'), resource('b', 'uniform')]))
        with self.assertRaisesRegex(ValueError, 'duplicate uniform slot'):
            allocate(stages, {}, {('vertex', 'a'): 0, ('vertex', 'b'): 0})
        with self.assertRaisesRegex(ValueError, 'slot must be in'):
            allocate(stages, {}, {('vertex', 'a'): 8})
        with self.assertRaisesRegex(ValueError, 'sampling policy'):
            allocate(dict(fragment=dict(resources=[resource('tex', 'texture')])), dict(tex=dict(sampler_type='filtering')))
        with self.assertRaisesRegex(ValueError, 'duplicate or conflicting'):
            allocate(dict(vertex=dict(resources=[resource('r', 'uniform'), resource('r', 'uniform')])), {})

    def test_limits_include_all_stages(self):
        for kind, limit in [('uniform', 8), ('storage_buffer', 32), ('sampler', 12)]:
            stages = {stage: dict(resources=[resource(f'r_{i}', kind, i) for i in indices])
                      for stage, indices in [('vertex', range(limit//2)), ('fragment', range(limit//2, limit+1))]}
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, 'exceeds Sokol'):
                allocate(stages, {})


class CliTest(unittest.TestCase):
    def test_shader_only_defaults_and_discovered_entries(self):
        args = parse_args(['triangle.slang'])
        self.assertEqual(args.output, Path('triangle.h'))
        programs, batch = select_programs(args, [dict(name='vs', stage='vertex'), dict(name='fs', stage='fragment')])
        self.assertFalse(batch)
        self.assertEqual(programs['draw'], dict(module='triangle', program='draw', vertex='vs', fragment='fs'))
        programs, batch = select_programs(args, [dict(name='cs', stage='compute')])
        self.assertEqual(programs['run']['compute'], 'cs')

    def test_numeric_filename_has_a_valid_default_module(self):
        programs, _ = select_programs(parse_args(['01-shader.slang', '--compute', 'cs']), [])
        self.assertEqual(programs['run']['module'], '_01_shader')

    def test_program_sets_and_direct_entries(self):
        args = parse_args(['shader.slang', '--module', 'example', '--program', 'base:vs:fs', '--program', 'work:cs'])
        programs, batch = select_programs(args, [])
        self.assertTrue(batch)
        self.assertEqual(programs['work'], dict(module='example', program='work', compute='cs'))
        args = parse_args(['shader.slang', '--vertex', 'vs', '--fragment', 'fs', '--program', 'custom'])
        programs, batch = select_programs(args, [])
        self.assertFalse(batch)
        self.assertEqual(programs['custom']['vertex'], 'vs')

    def test_ambiguous_entries_and_duplicate_programs_fail(self):
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            select_programs(parse_args(['shader.slang']), [dict(name='a', stage='compute'), dict(name='b', stage='compute')])
        with self.assertRaisesRegex(ValueError, 'duplicate program'):
            select_programs(parse_args(['shader.slang', '--program', 'draw:vs:fs', '--program', 'draw:vs:other']), [])
        with self.assertRaisesRegex(ValueError, 'cannot mix'):
            select_programs(parse_args(['shader.slang', '--program', 'draw:vs:fs', '--vertex', 'vs']), [])

    def test_removed_json_flags_and_source_overwrite_fail(self):
        for flags in (['--metadata', 'metadata.json'], ['--auto-bindings'], ['-o', 'shader.slang']):
            with self.subTest(flags=flags), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                parse_args(['shader.slang', *flags])

    def test_settings_reject_duplicates_and_bad_values(self):
        with self.assertRaisesRegex(ValueError, 'duplicate sample type'):
            frontend['parse_settings'](['tex=float', 'tex=depth'], frontend['SAMPLE_TYPES'], 'sample type')
        with self.assertRaisesRegex(ValueError, 'expects NAME=TYPE'):
            frontend['parse_settings'](['tex=bad'], frontend['SAMPLE_TYPES'], 'sample type')
        with self.assertRaisesRegex(ValueError, 'binding expects'):
            frontend['parse_slots'](['tex=2'])


class FrontendTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='frontend test ')
        self.addCleanup(self.directory.cleanup)
        self.work = Path(self.directory.name)
        self.args = parse_args([str(self.work/'shader.slang'), '--module', 'example',
                               '--program', 'base:vs:fsBase', '--program', 'full:vs:fsFull'])
        self.args.reflection = self.work/'shader.reflection.json'
        self.args.depfile = self.work/'shader.d'
        self.args.slangc = self.work/'slangc'
        self.args.core = self.work/'sokol-spirv'
        self.args.targets = 'metal_macos:wgsl'
        self.args.include_dir = [self.work/'include files']
        self.args.input.write_text('// Shader compiler calls are mocked.\n')
        self.calls, self.manifests = [], []
        self.fail_program = None
        self.entries = [dict(name='vs', stage='vertex'), dict(name='fs', stage='fragment')]
        self.items = {stage: [] for stage in frontend['STAGES']}

    def run_tool(self, args, **kwargs):
        self.calls.append(args)
        if '-target' in args:
            binary = Path(args[args.index('-o')+1])
            binary.write_text(args[args.index('-stage')+1] if '-stage' in args else 'discovery')
            if '-depfile' in args:
                Path(args[args.index('-depfile')+1]).write_text(str(binary)+': '+str(self.args.input)+' include\\ files/shared.slang\n')
        else:
            manifest = json.loads(Path(args[args.index('--manifest')+1]).read_text())
            self.manifests.append(manifest)
            if self.fail_program and manifest['program'] == self.fail_program:
                raise subprocess.CalledProcessError(1, args)
            Path(args[args.index('--output')+1]).write_text(manifest['module']+'_'+manifest['program'])
            Path(args[args.index('--reflection')+1]).write_text(json.dumps(dict(targets={})))

    def inspect(self, args, **kwargs):
        if '-version' in args: return '2026.20\n'
        stage = Path(args[-1]).read_text()
        if stage == 'discovery': return json.dumps(dict(entries=self.entries, resources=[]))
        return json.dumps(dict(entries=[dict(name='main', stage=stage)], resources=self.items[stage]))

    def compile(self):
        with patch('subprocess.run', side_effect=self.run_tool), patch('subprocess.check_output', side_effect=self.inspect):
            compile_programs(self.args)

    def test_batch_reuses_stages_and_preserves_dependencies(self):
        self.compile()
        stages = [args for args in self.calls if '-entry' in args]
        self.assertEqual(len(stages), 3)
        self.assertEqual(sum(args[args.index('-entry')+1] == 'vs' for args in stages), 1)
        self.assertTrue(all(str((self.work/'include files').resolve()) in args for args in stages))
        self.assertEqual(set(json.loads(self.args.reflection.read_text())), {'base', 'full'})
        self.assertEqual(self.args.output.read_text(), 'example_base\nexample_full')
        self.assertIn('include\\ files/shared.slang', self.args.depfile.read_text())
        self.assertNotIn('metadata.json', self.args.depfile.read_text())

    def test_shader_only_discovers_once_and_builds_metadata(self):
        self.args.program = []
        self.items['fragment'] = [resource('tex', 'texture'), resource('smp', 'sampler')]
        self.compile()
        self.assertEqual(len([cmd for cmd in self.calls if '-target' in cmd and '-entry' not in cmd]), 1)
        self.assertEqual(self.manifests[0]['program'], 'draw')
        bindings = self.manifests[0]['resources']
        self.assertEqual([b['kind'] for b in bindings], ['sampler', 'texture'])
        self.assertEqual(bindings[1]['sample_type'], 'float')
        self.assertEqual(self.args.output.read_text(), 'example_draw')
        self.assertIn('targets', json.loads(self.args.reflection.read_text()))

    def test_shader_only_main_writes_only_the_header(self):
        main = frontend['main']
        with patch.dict(main.__globals__, find_tool=lambda name, override=None: self.work/name), \
                patch('subprocess.run', side_effect=self.run_tool), \
                patch('subprocess.check_output', side_effect=self.inspect):
            main([str(self.args.input)])
        self.assertEqual(self.args.output.read_text(), 'shader_draw')
        self.assertEqual({p.name for p in self.work.iterdir()}, {'shader.slang', 'shader.h'})

    def test_cli_policy_and_slot_overrides_reach_manifest(self):
        self.items['fragment'] = [resource('tex', 'texture'), resource('smp', 'sampler')]
        self.args.sample_type = ['tex=unfilterable_float']
        self.args.sampler_type = ['smp=nonfiltering']
        self.args.binding = ['fragment:tex=9']
        self.compile()
        bindings = {b['name']: b for b in self.manifests[0]['resources']}
        self.assertEqual(bindings['fragment_tex']['slot'], 9)
        self.assertEqual(bindings['fragment_tex']['sample_type'], 'unfilterable_float')
        self.assertEqual(bindings['fragment_smp']['sampler_type'], 'nonfiltering')

    def test_failed_later_program_keeps_previous_outputs(self):
        self.compile()
        paths = (self.args.output, self.args.reflection, self.args.depfile)
        before = {path: path.read_bytes() for path in paths}
        self.fail_program = 'full'
        with self.assertRaises(subprocess.CalledProcessError): self.compile()
        for path in paths: self.assertEqual(path.read_bytes(), before[path])

    def test_unused_binding_preserves_outputs(self):
        self.compile()
        before = self.args.output.read_bytes()
        self.args.binding = ['fragment:missing=1']
        with self.assertRaisesRegex(ValueError, 'no reflected resource'): self.compile()
        self.assertEqual(self.args.output.read_bytes(), before)

    def test_exported_manifests_have_portable_paths(self):
        self.args.emit_spirv_dir = self.work/'exported'
        self.compile()
        for name in ('base', 'full'):
            folder = self.args.emit_spirv_dir/name
            for info in json.loads((folder/'manifest.json').read_text())['stages'].values():
                self.assertFalse(Path(info['file']).is_absolute())
                self.assertTrue((folder/info['file']).is_file())

    def test_compute_discovery_selects_compute_targets(self):
        self.args.program = []
        self.args.targets = None
        self.entries = [dict(name='cs', stage='compute')]
        self.compile()
        self.assertEqual(set(self.manifests[0]['stages']), {'compute'})
        targets = json.loads(self.args.reflection.read_text())['compiler']['targets']
        self.assertIn('glsl430', targets)
        self.assertIn('glsl310es', targets)
        self.assertNotIn('glsl410', targets)

    def test_storage_resources_select_modern_gl_defaults(self):
        self.args.targets = None
        self.items['vertex'] = [resource('records', 'storage_buffer', access='read')]
        self.compile()
        for report in json.loads(self.args.reflection.read_text()).values():
            self.assertIn('glsl430', report['compiler']['targets'])
            self.assertNotIn('glsl410', report['compiler']['targets'])


if __name__ == '__main__':
    unittest.main()
