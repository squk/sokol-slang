"""Check build-time Metal libraries for every stage and Apple platform."""

import argparse
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("headers", nargs="+")
    args = parser.parse_args()
    targets = {"metal_macos", "metal_ios", "metal_sim"}
    total = 0
    for header in args.headers:
        compiled = set()
        text = Path(header).read_text()
        assert not re.search(
            r"_source_metal_(macos|ios|sim)\[", text
        ), "unexpected Metal source fallback"
        for symbol, stage, target, size, data in re.findall(
            r"(\w+_(vs|fs|cs)_bytecode_(metal_macos|metal_ios|metal_sim))\[(\d+)\] = \{(.*?)\};",
            text,
            re.S,
        ):
            key = (target, stage)
            assert key not in compiled, f"duplicate Metal stage: {key}"
            binary = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", data))
            assert binary.startswith(b"MTLB") and len(binary) == int(
                size
            ), f"invalid metallib: {key}"
            function = {"vs": "vertex_func", "fs": "fragment_func", "cs": "compute_func"}[stage]
            assert f"desc.{function}.bytecode.ptr = {symbol};" in text
            compiled.add(key)
        stages = {stage for _, stage in compiled}
        assert stages in ({"vs", "fs"}, {"cs"}), f"missing or mixed stages in {header}: {stages}"
        assert compiled == {(target, stage) for target in targets for stage in stages}, compiled
        total += len(compiled)
    print(
        f"PASS: {total} embedded Metal libraries for macOS, iOS and simulator; no source fallback"
    )


if __name__ == "__main__":
    main()
