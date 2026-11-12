import json
import sys
from pathlib import Path

print(json.loads(Path('build/release/source-manifest.json').read_text())['worktree'])
