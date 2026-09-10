#!/usr/bin/env python3
"""Integration contract for the GUI's CLI index and single-class requests."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

engine, fixtures = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
files = [fixtures / 'demo.jar', fixtures / 'Main.class']
files += [p for p in (fixtures / 'classes.dex', fixtures / '示例 app.apk') if p.exists()]
with tempfile.TemporaryDirectory(prefix='garlic-cli-test-') as temp:
    root = Path(temp)
    for number, source in enumerate(files):
        for threads in (1, 2):
            target = root / f'{number}-{threads}'
            target.mkdir()
            index = target / 'index.jsonl'
            out = target / 'output'
            args = [str(engine), str(source), '-o', str(out), '-t', str(threads)]
            subprocess.run([*args, '-I', str(index)], check=True, capture_output=True, timeout=20)
            names = [json.loads(line)['name'] for line in index.read_text().splitlines()]
            assert 'demo/Main' in names, names
            if source.suffix != '.class':
                assert 'demo/Main$Details' in names
            assert not list(out.rglob('*.java')), 'Indexing must not decompile Java'
            assert not list(out.rglob('*.smali')), 'Indexing must not decompile Smali'
            result = subprocess.run([*args, '-c', 'demo/Main'], check=True, capture_output=True, timeout=20)
            if source.suffix == '.class':
                assert b'class Main' in result.stdout
            else:
                assert (out / 'demo/Main.java').is_file()
                assert not (out / 'demo/Extra.java').exists()
            missing = subprocess.run([*args, '-c', 'demo/Missing'], capture_output=True, timeout=20)
            assert missing.returncode != 0
    bad = subprocess.run([str(engine), str(files[0]), '-I'], capture_output=True, timeout=20)
    assert bad.returncode != 0, 'Incomplete index flag must fail, never batch decompile'
print('Index / single-class / no-match contract passed for', len(files), 'formats at 1 and 2 threads')
