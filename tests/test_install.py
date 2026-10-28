"""Stage a native install and compile copied examples outside the checkout."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(command, cwd, env=None):
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
    assert result.returncode == 0, f'{command}\n{result.stdout}{result.stderr}'
    return result.stdout


def outputs(directory):
    directory.mkdir(parents=True, exist_ok=True)
    return ['--output', str(directory / 'shader.h'),
            '--reflection', str(directory / 'shader.json'),
            '--depfile', str(directory / 'shader.d')]


def check_output(directory, stages):
    header = (directory / 'shader.h').read_bytes()
    assert b'sg_shader_desc' in header, 'missing generated descriptor'
    targets = json.loads((directory / 'shader.json').read_text())['targets']
    assert len(targets) == 8, targets.keys()
    assert all(set(target) == stages for target in targets.values()), targets
    assert (directory / 'shader.d').stat().st_size > 0, 'missing dependency file'
    return targets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--meson', required=True)
    parser.add_argument('--slangc', type=Path)
    parser.add_argument('--glslang', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    assert args.slangc or args.glslang, 'a shader compiler is required'

    with tempfile.TemporaryDirectory(prefix='sokol installed tools ') as directory:
        work = Path(directory).resolve()
        assert root not in work.parents, 'install check must run outside the checkout'
        staged = work / 'staged'
        run([args.meson, 'install', '-C', str(build), '--no-rebuild',
             '--destdir', str(staged)], work)
        core_name = 'sokol-spirv.exe' if os.name == 'nt' else 'sokol-spirv'
        installed = list(staged.rglob(core_name))
        assert len(installed) == 1, installed
        core = installed[0]
        run([str(core), '--help'], work)
        frontends = list(staged.rglob('sokol-slang'))
        if args.slangc:
            assert len(frontends) == 1, frontends
            frontend = frontends[0]
            if os.name != 'nt':
                assert os.access(frontend, os.X_OK), 'installed frontend is not executable'
                run([str(frontend), '--help'], work)
            cli = [sys.executable, str(frontend)]
        else:
            assert not frontends, 'core-only install unexpectedly includes the frontend'

        # Only these copied sources and the staged tools are used below. Slang
        # remains an explicitly supplied external compiler, not an installed dependency.
        examples = work / 'examples'
        shutil.copytree(root / 'examples', examples)
        for name in ('portable', 'compute', 'storage-image', 'depth'):
            example = examples / name
            stages = {'compute'} if name in ('compute', 'storage-image') else {'vertex', 'fragment'}
            if args.glslang:
                manifest = example / 'glsl/manifest.json'
                data = json.loads(manifest.read_text())
                for stage, item in data['stages'].items():
                    suffix = {'vertex': 'vert', 'fragment': 'frag', 'compute': 'comp'}[stage]
                    source = example / 'glsl' / ('shader.comp' if stage == 'compute' else f'{stage}.{suffix}')
                    run([str(args.glslang), '-V', '--target-env', 'vulkan1.1',
                         str(source), '-o', str(manifest.parent / item['file'])], work)
                generated = work / 'glsl' / name
                run([str(core), '--manifest', str(manifest), *outputs(generated)], work)
                check_output(generated, stages)

            if args.slangc:
                generated = work / 'slang' / name
                emitted = work / 'spirv' / name
                run([sys.executable, str(frontend), '--slangc', str(args.slangc),
                     '--core', str(core), '--input', str(example / 'shader.slang'),
                     '--metadata', str(example / 'metadata.json'),
                     '--emit-spirv-dir', str(emitted), *outputs(generated)], work)
                expected = check_output(generated, stages)
                # Find the installed sibling core and Slang on PATH; write only a header.
                wrapped = work / 'cli-slang' / name / 'shader.h'
                tool_env = dict(os.environ, PATH=str(args.slangc.parent) + os.pathsep + os.environ.get('PATH', ''))
                run([*cli, str(example / 'shader.slang'),
                     '--metadata', str(example / 'metadata.json'), '-o', str(wrapped)], work, env=tool_env)
                assert wrapped.read_bytes() == (generated / 'shader.h').read_bytes()
                assert list(wrapped.parent.iterdir()) == [wrapped], 'unexpected sidecars'

                replayed = work / 'replayed' / name
                run([str(core), '--manifest', str(emitted / 'manifest.json'),
                     *outputs(replayed)], work)
                assert check_output(replayed, stages) == expected, f'{name}: replay reflection changed'
                assert (replayed / 'shader.h').read_bytes() == (generated / 'shader.h').read_bytes(), \
                    f'{name}: replay header changed'

    print('PASS: staged install, external example compilation, all backend outputs'
          + (', and byte-identical Slang SPIR-V replay' if args.slangc else ' (core only)'))


if __name__ == '__main__':
    main()
