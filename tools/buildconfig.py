#!/usr/bin/env python3
"""Update a dependency stamp only when flags, input lists or tools change."""
import argparse
import json
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path)
    ap.add_argument('--value', action='append', default=[])
    ap.add_argument('--tool', action='append', default=[])
    a = ap.parse_args()
    tools = []
    for command in a.tool:
        argv = shlex.split(command)
        version = subprocess.check_output([*argv, '--version'], text=True)
        path = Path(shutil.which(argv[0]) or argv[0]).resolve()
        tools.append((command, str(path), path.stat().st_mtime_ns, version))
    text = json.dumps({'values': a.value, 'tools': tools}, sort_keys=True) + '\n'
    if a.output.exists() and a.output.read_text() == text: return
    a.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(dir=a.output.parent)
    try:
        with os.fdopen(fd, 'w') as f: f.write(text)
        os.replace(temp, a.output)
    finally:
        if os.path.exists(temp): os.unlink(temp)

if __name__ == '__main__': main()
