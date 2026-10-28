"""Ensure runtime fixtures retain the unused outputs and the location gap."""
import json
import sys
from pathlib import Path

for path in sys.argv[1:]:
    for target, stages in json.loads(Path(path).read_text())['targets'].items():
        outputs = stages['vertex']['spirv']['outputs']
        inputs = stages['fragment']['spirv']['inputs']
        assert {item['location'] for item in outputs} == {0, 1, 2, 3}, (path, target, outputs)
        assert {item['location'] for item in inputs} == {0, 2}, (path, target, inputs)
print('PASS: runtime shaders retain unused middle/tail outputs and consume locations 0 and 2')
