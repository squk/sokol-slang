"""Verify compute translation and access contracts using independent GLSL SPIR-V."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import tempfile
from test_support import run, rewrite_spirv, check_translation

TARGETS = ('glsl430', 'glsl310es', 'hlsl5', 'metal_macos', 'metal_ios', 'metal_sim', 'wgsl', 'spirv_vk')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--core', required=True)
    parser.add_argument('--glslang', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    original = json.loads((root / 'tests/fixtures/compute/glsl/manifest.json').read_text())
    with tempfile.TemporaryDirectory(prefix='sokol compute ') as directory:
        work = Path(directory)
        binary = work / 'generic_compute.spv'
        run([args.glslang, '-V', '--target-env', 'vulkan1.1', str(root / 'tests/fixtures/compute/glsl/shader.comp'), '-o', str(binary)])
        spirv = binary.read_bytes()
        inspected = json.loads(run([args.core, '--inspect', str(binary)]))
        assert inspected['entries'] == [{'name': 'main', 'stage': 'compute'}]
        assert {r['access'] for r in inspected['resources'] if r['kind'] == 'storage_buffer'} == {'read', 'read_write'}
        manifest = work / 'manifest.json'
        header = work / 'compute.h'
        report = work / 'compute.json'
        depfile = work / 'compute.d'

        def generate(data=original, target=None, error=None):
            manifest.write_text(json.dumps(data))
            old = header.read_bytes() if header.exists() else None
            command = [args.core, '--manifest', str(manifest), '--output', str(header),
                       '--reflection', str(report), '--depfile', str(depfile)]
            result = subprocess.run(command + (['--targets', target] if target else []), capture_output=True, text=True)
            if error:
                assert result.returncode != 0 and error in result.stderr, result.stderr
                assert header.read_bytes() == old, 'failed compute generation replaced header'
            else:
                assert result.returncode == 0, result.stderr

        generate()
        first = header.read_bytes()
        generate()
        assert header.read_bytes() == first
        assert set(json.loads(report.read_text())['targets']) == set(TARGETS)
        assert 'generic_compute.spv' in depfile.read_text()
        for target in TARGETS:
            generate(target=target)
            reflected = json.loads(report.read_text())['targets'][target]['compute']
            assert reflected['workgroup_size'] == [32, 2, 1]
            buffers = {r['name']: r for r in reflected['bindings'] if r['kind'] == 'storage_buffer'}
            assert buffers['compute_sourceRecords']['access'] == 'read'
            assert buffers['compute_resultRecords']['access'] == 'read_write'
            if target.startswith(('glsl', 'hlsl')):
                check_translation(args.glslang, header, 'compute_cs', target, 'comp', work)

        def bad(change, error):
            data = copy.deepcopy(original)
            change(data)
            generate(data, error=error)

        bad(lambda d: d['stages'].update(vertex=dict(file='missing.spv', entry='main')), 'vertex and fragment stages, or a compute stage')
        bad(lambda d: d['stages'].clear(), 'vertex and fragment stages, or a compute stage')
        bad(lambda d: d['resources'][1].update(access='read_write'), 'access does not match')
        bad(lambda d: d['resources'][2].pop('access'), 'access does not match')
        bad(lambda d: d['resources'][0].update(access='read'), 'access is only valid')
        bad(lambda d: d['resources'][1].update(stage='fragment'), 'resource stage is not in')
        for target in ('glsl410', 'glsl300es', 'hlsl4'):
            generate(target=target, error='compute requires')
        for workgroup in ((0x80000000, 1, 1), (65536, 32768, 1), (65536, 65537, 1), (32, 2, 0x40000001)):
            binary.write_bytes(rewrite_spirv(spirv, workgroup=workgroup))
            generate(error='compute workgroup size exceeds')
        renamed = copy.deepcopy(original)
        renamed['stages']['compute']['entry'] = 'dispatch_entry'
        binary.write_bytes(rewrite_spirv(spirv, strip_names=True, entry='dispatch_entry'))
        generate(renamed)
        assert 'member_0' in header.read_text()
        print('PASS: compute stages, eight targets, workgroups, access contracts, stripped names and diagnostics')


if __name__ == '__main__':
    main()
