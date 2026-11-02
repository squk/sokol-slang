"""Check typed texture operations and metadata against every output dialect."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from test_support import TARGETS, run, check_translation


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--core', required=True)
    parser.add_argument('--glslang', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    manifest = json.loads((root / 'tests/fixtures/portable/glsl/manifest.json').read_text())
    original = (root / 'tests/fixtures/portable/glsl/fragment.frag').read_text()
    cases = [
        ('uint', 'nonfiltering', 'utexture2D', 'vec4(texelFetch(usampler2D(tex, smp), ivec2(varying_uv * 4.0), 0))'),
        ('sint', 'nonfiltering', 'itexture2D', 'vec4(texelFetch(isampler2D(tex, smp), ivec2(varying_uv * 4.0), 0))'),
        ('depth', 'comparison', 'texture2D', 'vec4(texture(sampler2DShadow(tex, smp), vec3(varying_uv, 0.5)))'),
        ('depth', 'comparison', 'texture2D', 'vec4(textureLod(sampler2DShadow(tex, smp), vec3(varying_uv, 0.5), 0))'),
        ('unfilterable_float', 'nonfiltering', 'texture2D', 'texture(sampler2D(tex, smp), varying_uv)'),
    ]
    with tempfile.TemporaryDirectory(prefix='sokol texture types ') as directory:
        work = Path(directory)
        run([args.glslang, '-V', '--target-env', 'vulkan1.1', str(root / 'tests/fixtures/portable/glsl/vertex.vert'),
             '-o', str(work / 'generic_vertex.spv')])
        header = work / 'shader.h'
        command = [args.core, '--manifest', str(work / 'manifest.json'), '--output', str(header),
                   '--reflection', str(work / 'reflection.json'), '--depfile', str(work / 'shader.d')]

        def generate(target, error=None):
            (work / 'manifest.json').write_text(json.dumps(manifest))
            before = header.read_bytes() if header.exists() else None
            result = subprocess.run(command + ['--targets', target], capture_output=True, text=True)
            if error:
                assert result.returncode != 0 and error in result.stderr, result.stderr
                assert header.read_bytes() == before, 'failed generation replaced header'
            else:
                assert result.returncode == 0, result.stderr

        for sample, sampler, texture, expression in cases:
            source = original.replace('uniform texture2D', 'uniform ' + texture)
            source = source.replace('texture(sampler2D(tex, smp), varying_uv)', expression)
            (work / 'input.frag').write_text(source)
            run([args.glslang, '-V', '--target-env', 'vulkan1.1', str(work / 'input.frag'),
                 '-o', str(work / 'generic_fragment.spv')])
            manifest['resources'][2]['sample_type'] = sample
            manifest['resources'][3]['sampler_type'] = sampler
            for target in TARGETS:
                generate(target)
                text = header.read_text()
                assert 'SG_IMAGESAMPLETYPE_' + sample.upper() in text
                assert 'SG_SAMPLERTYPE_' + sampler.upper() in text
                if target.startswith(('glsl', 'hlsl')):
                    check_translation(args.glslang, header, 'portable_fs', target, 'frag', work)
            manifest['resources'][2]['sample_type'] = 'uint' if sample == 'unfilterable_float' else 'float'
            generate('metal_macos', 'sample_type does not match')
            manifest['resources'][2]['sample_type'] = sample
            manifest['resources'][3]['sampler_type'] = 'filtering' if sampler == 'comparison' else 'comparison'
            generate('metal_macos', 'sampler_type does not match')
            if sample != 'depth':
                manifest['resources'][3]['sampler_type'] = 'filtering'
                generate('metal_macos', 'require a nonfiltering sampler')
    print('PASS: integer loads, depth comparisons, unfilterable sampling, 11 dialects and metadata diagnostics')


if __name__ == '__main__':
    main()
