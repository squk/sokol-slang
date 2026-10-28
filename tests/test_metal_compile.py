"""Compile the generated MSL with Apple's compiler for each Metal platform."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_support import decode_source


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--metal', required=True)
    parser.add_argument('headers', nargs='+')
    args = parser.parse_args()
    targets = {
        'metal_macos': 'air64-apple-macos10.14',
        'metal_ios': 'air64-apple-ios12.0',
        'metal_sim': 'air64-apple-ios13.0-simulator',
    }
    total = 0
    with tempfile.TemporaryDirectory(prefix='sokol metal ') as directory:
        work = Path(directory)
        for header in args.headers:
            compiled = set()
            for stage, target, data in re.findall(
                r'\w+_(vs|fs|cs)_source_(metal_macos|metal_ios|metal_sim)\[\d+\] = \{(.*?)\};',
                Path(header).read_text(), re.S
            ):
                key = (target, stage)
                assert key not in compiled, f'duplicate Metal stage: {key}'
                source = work / f'{target}_{stage}.metal'
                source.write_bytes(decode_source(data))
                output = source.with_suffix('.air')
                standard = 'macos-metal2.1' if target == 'metal_macos' else 'ios-metal2.1'
                result = subprocess.run([args.metal, '-target', targets[target], '-std=' + standard,
                    '-c', str(source), '-o', str(output)], capture_output=True, text=True)
                assert result.returncode == 0, f'{header}: {target}/{stage}\n{result.stdout}{result.stderr}'
                assert output.is_file() and output.stat().st_size > 0, 'missing Metal object'
                compiled.add(key)
            stages = {stage for _, stage in compiled}
            assert stages in ({'vs', 'fs'}, {'cs'}), f'missing or mixed stages in {header}: {stages}'
            assert compiled == {(target, stage) for target in targets for stage in stages}, compiled
            total += len(compiled)
    print(f'PASS: {total} Metal stage compilations for macOS, iOS and simulator (MSL 2.1)')


if __name__ == '__main__':
    main()
