"""Exercise compiler failures and dependency tracking with real Slang output."""
import argparse
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
    options = ['--module', 'proof']
    with tempfile.TemporaryDirectory(prefix='sokol slang ') as directory:
        work = Path(directory)
        shader = work / 'shader.slang'
        output = work / 'shader.h'
        reflection = work / 'shader.reflection.json'
        depfile = work / 'shader.d'
        command = [sys.executable, str(root / 'tools/sokol-slang'), '--slangc', args.slangc,
            '--core', args.core, '--targets', 'metal_macos:metal_ios:metal_sim', '--input', str(shader),
            '--output', str(output), '--reflection', str(reflection), '--depfile', str(depfile)]

        def run(text, flags, error=None, extra_args=()):
            shader.write_text(text)
            old = output.read_bytes() if output.exists() else None
            result = subprocess.run(command + list(flags) + list(extra_args), cwd=work, text=True, capture_output=True)
            if error:
                assert result.returncode != 0, 'invalid shader succeeded'
                assert error in result.stderr, result.stderr
                assert old == (output.read_bytes() if output.exists() else None), 'failed compile replaced header'
            else:
                assert result.returncode == 0, result.stderr
            return result

        run(source, options)
        report = json.loads(reflection.read_text())['targets']['metal_macos']
        bindings = {b['name']: b for stage in ('vertex', 'fragment') for b in report[stage]['bindings']}
        assert bindings['vertex_records']['slot'] != bindings['fragment_records']['slot']
        assert bindings['vertex_records']['binding'] == 8
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
        run(unused_varyings, options)
        stages = json.loads(reflection.read_text())['targets']['metal_macos']
        assert len(stages['vertex']['spirv']['outputs']) == 4
        assert len(stages['fragment']['spirv']['inputs']) == 2
        # Native texture loads must not expose an unused helper sampler.
        # Keep this check independent of the layout fixture's BaseInstance
        # builtin, which the pinned WGSL reader does not support.
        loads = (root / 'tests/fixtures/portable/shader.slang').read_text().replace(
            'tex.Sample(smp, input.uv)', 'tex.Load(int3(0, 0, 0))')
        load_options = options + ['--sample-type', 'tex=unfilterable_float']
        run(loads, load_options, extra_args=['--targets', 'metal_macos:wgsl:spirv_vk'])
        load_report = json.loads(reflection.read_text())
        assert not any(resource['kind'] == 'sampler' for resource in load_report['resources'].values())
        for stages in load_report['targets'].values():
            assert not any(binding['kind'] == 'sampler' for stage in stages.values()
                           for binding in stage['bindings'])
        assert 'desc.samplers[' not in output.read_text()
        run(source, options)
        run(source, options + ['--binding', 'vertex:records=0', '--binding', 'fragment:tex=0'], 'duplicate view slot')
        run(source, options + ['--binding', 'vertex:params=8'], 'slot must be in')
        run(source, options + ['--sample-type', 'tex=depth'], 'sample_type does not match')
        run(source.replace('float scale;', 'float scale; float2x2 unsupported;'), options, 'uniforms support')
        run(source.replace('float2 position : POSITION;', '[[vk::location(16)]] float2 position : POSITION;').replace('uint recordIndex : TEXCOORD0;', '[[vk::location(1)]] uint recordIndex : TEXCOORD0;'),
            options, 'out-of-range stage location')
        run(source.replace('return tex.Sample', 'syntax_error return tex.Sample'), options, 'error')
        (work / 'helper.slang').write_text('float proofScale() { return 1.0; }\n')
        imported = 'import helper;\n' + source.replace('input.position * params.scale', 'input.position * params.scale * proofScale()')
        run(imported, options)
        assert 'helper.slang' in depfile.read_text(), depfile.read_text()
        assert '\\ ' in depfile.read_text(), 'dependency paths with spaces must be escaped'
        first = output.read_bytes()
        (work / 'helper.slang').write_text('float proofScale() { return 0.5; }\n')
        run(imported, options)
        assert first != output.read_bytes(), 'import edit did not change generated shader'

        # Relative textual includes also preserve macros and transitive dependencies.
        (work / 'constants.slang').write_text('#define PROOF_SCALE 1.0\n')
        (work / 'common.slang').write_text('#include "constants.slang"\n')
        included = '#include "common.slang"\n' + source.replace(
            'input.position * params.scale', 'input.position * params.scale * PROOF_SCALE')
        run(included, options)
        assert 'common.slang' in depfile.read_text()
        assert 'constants.slang' in depfile.read_text()
        first = output.read_bytes()
        (work / 'constants.slang').write_text('#define PROOF_SCALE 0.5\n')
        run(included, options)
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
        run(combined, options, error='common.slang')
        search_args = ['-I', includes.name, '--include-dir', str(modules)]
        run(combined, options, extra_args=search_args)
        dependencies = depfile.read_text()
        for path in (includes / 'common.slang', includes / 'constants.slang', modules / 'helper.slang'):
            escaped = str(path).replace('\\', '\\\\').replace(':', '\\:').replace(' ', '\\ ')
            assert escaped in dependencies, dependencies
        # The joined short form is accepted too; missing dependencies preserve the output.
        run(combined, options, extra_args=['-I' + str(includes), '-I' + str(modules)])
        (includes / 'constants.slang').unlink()
        run(combined, options, error='constants.slang', extra_args=search_args)

        compute_source = (root / 'tests/fixtures/compute/shader.slang').read_text()
        compute_options = ['--module', 'compute']
        target_index = command.index('--targets')
        del command[target_index:target_index + 2]
        emitted = work / 'compute-spirv'
        command += ['--emit-spirv-dir', str(emitted)]
        run(compute_source, compute_options)
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
        run(compute_source, compute_options, 'choose vertex and fragment entries, or a compute entry',
            extra_args=['--vertex', 'vertexMain', '--compute', 'computeMain'])
        run(compute_source.replace('numthreads(32, 2, 1)', 'numthreads(8, 1, 1)'), compute_options, 'multiple of 32')
        # No JSON, entry flags, naming flags or policy flags: Slang owns discovery.
        simple = (root / 'tests/fixtures/portable/shader.slang').read_text()
        run(simple, [])
        simple_report = json.loads(reflection.read_text())
        assert simple_report['resources']['tex']['sample_type'] == 'float'
        assert simple_report['resources']['smp']['sampler_type'] == 'filtering'
        assert 'shader_draw_shader_desc' in output.read_text()
        assert not (work / 'metadata.json').exists()
        depth_source = (root / 'tests/fixtures/depth/shader.slang').read_text()
        run(depth_source, [])
        inferred = json.loads(reflection.read_text())['resources']
        assert inferred['shadow']['sample_type'] == 'depth'
        assert inferred['lessSmp']['sampler_type'] == 'comparison'
        assert inferred['greaterSmp']['sampler_type'] == 'comparison'
        run(compute_source, [])
        assert set(json.loads(reflection.read_text())['targets']['metal_macos']) == {'compute'}
    print('PASS: raster/compute bindings, invalid CLI settings, layouts, diagnostics, includes/imports and reusable compute SPIR-V')


if __name__ == '__main__':
    main()
