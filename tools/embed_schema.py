"""Embed the checked-in manifest schema in the standalone executable."""
import json
from pathlib import Path
import sys

schema = json.loads(Path(sys.argv[1]).read_text())
Path(sys.argv[2]).write_text('inline constexpr const char* manifest_schema = ' +
                           json.dumps(json.dumps(schema)) + ';\n')
