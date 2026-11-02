"""Exercise compiler failures and dependency tracking with real Slang output."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--slangc', required=True)
    parser.add_argument('--core', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'tests/shaders/proof.slang').read_text()
    metadata = json.loads((root / 'tests/shaders/proof.json').read_text())
    with tempfile.TemporaryDirectory(prefix='sokol slang ') as directory:
        work = Path(directory)
        shader = work / 'shader.slang'
        meta = work / 'shader.json'
        output = work / 'shader.h'
        reflection = work / 'shader.reflection.json'
        depfile = work / 'shader.d'
        command = [sys.executable, str(root / 'tools/sokol-slang'), '--slangc', args.slangc,
            '--core', args.core, '--targets', 'metal_macos:metal_ios:metal_sim', '--input', str(shader), '--metadata', str(meta),
            '--output', str(output), '--reflection', str(reflection), '--depfile', str(depfile)]

        def run(text, data, error=None, extra_args=()):
            shader.write_text(text)
            meta.write_text(json.dumps(data))
            old = output.read_bytes() if output.exists() else None
            result = subprocess.run(command + list(extra_args), cwd=work, text=True, capture_output=True)
            if error:
                assert result.returncode != 0, 'invalid shader succeeded'
                assert error in result.stderr, result.stderr
                assert old == (output.read_bytes() if output.exists() else None), 'failed compile replaced header'
            else:
                assert result.returncode == 0, result.stderr
            return result

        run(source, metadata)
        report = json.loads(reflection.read_text())['targets']['metal_macos']
        bindings = {b['name']: b for stage in ('vertex', 'fragment') for b in report[stage]['bindings']}
        assert bindings['vertex_records']['slot'] == 0
        assert bindings['vertex_records']['binding'] == 8
        assert bindings['fragment_records']['slot'] == 2
        assert bindings['fragment_records']['binding'] == 8
        assert bindings['fragment_tex']['binding'] == 0
        assert report['vertex']['entry'] == 'main0'
        # Slang removes unused fragment inputs from a shared stage struct while
        # retaining the corresponding vertex outputs.
        unused_varyings = source.replace(
            'nointerpolation uint recordIndex : TEXCOORD1;',
            'float2 unusedMiddle : TEXCOORD1;\n'
            '    nointerpolation uint recordIndex : TEXCOORD2;\n'
            '    float2 unusedTail : TEXCOORD3;').replace(
                'output.uv = input.position * 0.5 + 0.5;',
                'output.uv = input.position * 0.5 + 0.5;\n'
                '    output.unusedMiddle = input.position;\n'
                '    output.unusedTail = input.position;')
        run(unused_varyings, metadata)
        stages = json.loads(reflection.read_text())['targets']['metal_macos']
        assert len(stages['vertex']['spirv']['outputs']) == 4
        assert len(stages['fragment']['spirv']['inputs']) == 2
        broken = copy.deepcopy(metadata)
        broken['resources']['tex']['slots']['fragment'] = 0
        run(source, broken, 'duplicate view slot')
        broken = copy.deepcopy(metadata)
        broken['resources']['params']['slots']['vertex'] = 8
        run(source, broken, 'slot must be in')
        broken = copy.deepcopy(metadata)
        del broken['resources']['tex']
        run(source, broken, 'missing or wrong metadata')
        run(source.replace('float scale;', 'float scale; float2x2 unsupported;'), metadata, 'uniforms support')
        run(source.replace('float2 position : POSITION;', '[[vk::location(16)]] float2 position : POSITION;').replace('uint recordIndex : TEXCOORD0;', '[[vk::location(1)]] uint recordIndex : TEXCOORD0;'),
            metadata, 'out-of-range stage location')
        run(source.replace('return tex.Sample', 'syntax_error return tex.Sample'), metadata, 'error')
        (work / 'helper.slang').write_text('float proofScale() { return 1.0; }\n')
        imported = 'import helper;\n' + source.replace('input.position * params.scale', 'input.position * params.scale * proofScale()')
        run(imported, metadata)
        assert 'helper.slang' in depfile.read_text(), depfile.read_text()
        assert '\\ ' in depfile.read_text(), 'dependency paths with spaces must be escaped'
        first = output.read_bytes()
        (work / 'helper.slang').write_text('float proofScale() { return 0.5; }\n')
        run(imported, metadata)
        assert first != output.read_bytes(), 'import edit did not change generated shader'

        # Relative textual includes also preserve macros and transitive dependencies.
        (work / 'constants.slang').write_text('#define PROOF_SCALE 1.0\n')
        (work / 'common.slang').write_text('#include "constants.slang"\n')
        included = '#include "common.slang"\n' + source.replace(
            'input.position * params.scale', 'input.position * params.scale * PROOF_SCALE')
        run(included, metadata)
        assert 'common.slang' in depfile.read_text()
        assert 'constants.slang' in depfile.read_text()
        first = output.read_bytes()
        (work / 'constants.slang').write_text('#define PROOF_SCALE 0.5\n')
        run(included, metadata)
        assert first != output.read_bytes(), 'include edit did not change generated shader'

        # Resolve both includes and imports through repeatable search directories.
        includes = work / 'include files'
        modules = work / 'module files'
        includes.mkdir()
        modules.mkdir()
        (work / 'common.slang').rename(includes / 'common.slang')
        (work / 'constants.slang').rename(includes / 'constants.slang')
        (work / 'helper.slang').rename(modules / 'helper.slang')
        combined = 'import helper;\n' + included.replace('PROOF_SCALE', 'PROOF_SCALE * proofScale()')
        run(combined, metadata, error='common.slang')
        search_args = ['-I', includes.name, '--include-dir', str(modules)]
        run(combined, metadata, extra_args=search_args)
        dependencies = depfile.read_text()
        for path in (includes / 'common.slang', includes / 'constants.slang', modules / 'helper.slang'):
            escaped = str(path).replace('\\', '\\\\').replace(':', '\\:').replace(' ', '\\ ')
            assert escaped in dependencies, dependencies
        # The joined short form is accepted too; missing dependencies preserve the output.
        run(combined, metadata, extra_args=['-I' + str(includes), '-I' + str(modules)])
        (includes / 'constants.slang').unlink()
        run(combined, metadata, error='constants.slang', extra_args=search_args)

        compute_source = (root / 'tests/fixtures/compute/shader.slang').read_text()
        compute_meta = json.loads((root / 'tests/fixtures/compute/metadata.json').read_text())
        target_index = command.index('--targets')
        del command[target_index:target_index + 2]
        emitted = work / 'compute-spirv'
        command += ['--emit-spirv-dir', str(emitted)]
        run(compute_source, compute_meta)
        targets = json.loads(reflection.read_text())['targets']
        assert set(targets) == {'glsl430', 'glsl310es', 'hlsl5', 'metal_macos', 'metal_ios', 'metal_sim', 'wgsl', 'spirv_vk'}
        assert all(t['compute']['workgroup_size'] == [32, 2, 1] for t in targets.values())
        assert set(json.loads((emitted / 'manifest.json').read_text())['stages']) == {'compute'}
        reused = work / 'reused'
        result = subprocess.run([args.core, '--manifest', str(emitted / 'manifest.json'),
            '--output', str(reused / 'shader.h'), '--reflection', str(reused / 'shader.json'),
            '--depfile', str(reused / 'shader.d')], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert (reused / 'shader.h').read_bytes() == output.read_bytes(), 'reused compute SPIR-V changed header'
        broken = copy.deepcopy(compute_meta)
        broken['resources']['resultRecords'].pop('access')
        run(compute_source, broken, 'access does not match')
        broken = dict(compute_meta, vertex='vertexMain')
        run(compute_source, broken, 'choose vertex and fragment entries, or a compute entry')
        run(compute_source.replace('numthreads(32, 2, 1)', 'numthreads(8, 1, 1)'), compute_meta, 'multiple of 32')
    print('PASS: raster/compute bindings, invalid metadata, layouts, diagnostics, includes/imports and reusable compute SPIR-V')


if __name__ == '__main__':
    main()
