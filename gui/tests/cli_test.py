#!/usr/bin/env python3
"""Integration contract for the GUI's CLI index and single-class requests."""
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile

engine, fixtures = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
files = [fixtures / 'demo.jar', fixtures / 'demo.zip', fixtures / 'Main.class', fixtures / 'unknown.jar']
files += [p for p in (fixtures / 'classes.dex', fixtures / '示例 app.apk', fixtures / 'nested.apks') if p.exists()]
with tempfile.TemporaryDirectory(prefix='garlic-cli-test-') as temp:
    root = Path(temp)
    # Owned smali fixture: constant dispatcher specialization must preserve
    # results, including sparse negative keys and switch default paths.
    flattened = root / 'unflattened'
    env = dict(os.environ, GARLIC_UNFLATTEN='1')
    subprocess.run([str(engine), str(fixtures / 'flattened.dex'), '-o', str(flattened), '-t', '1'],
                   check=True, capture_output=True, timeout=20, env=env)
    source = flattened / 'demo/Flattened.java'
    code = source.read_text(encoding='utf-8')
    assert code.count('switch(') == 1, code  # Only the unknown parameter dispatcher remains.
    runner = flattened / 'Check.java'
    runner.write_text('''import demo.Flattened;
public class Check {
    public static void main(String[] args) {
        if (Flattened.run() != 7 || Flattened.sparse() != 5 || Flattened.fallback() != 3
            || Flattened.unknown(0) != 5 || Flattened.unknown(8) != 3)
            throw new AssertionError("Dispatcher specialization changed behavior");
    }
}''', encoding='utf-8')
    subprocess.run(['javac', '-d', str(flattened), str(source), str(runner)], check=True,
                   capture_output=True, timeout=30)
    subprocess.run(['java', '-cp', str(flattened), 'Check'], check=True,
                   capture_output=True, timeout=20)
    # Use non-ASCII input, output, index and environment paths on every runner,
    # including Windows ARM64 where an ANSI argv previously disagreed with miniz.
    unicode_root = root / ('中文路径 测试' * 10)
    unicode_root.mkdir()
    originals = list(files)
    for source in originals:
        destination = unicode_root / ('输入 文件' + source.name)
        shutil.copyfile(source, destination)
        files.append(destination)
    for number, source in enumerate(files):
        for threads in (1, 2):
            target = unicode_root / f'输出-{number}-{threads}'
            target.mkdir()
            index = target / '索引.jsonl'
            out = target / '源码'
            args = [str(engine), str(source), '-o', str(out), '-t', str(threads)]
            subprocess.run([*args, '-I', str(index)], check=True, capture_output=True, timeout=20)
            names = [json.loads(line)['name'] for line in index.read_text(encoding='utf-8').splitlines()]
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
    # Exercise concurrent worker startup and full export, beyond the single-class path.
    for attempt in range(12):
        target = unicode_root / f'并行-{attempt}'
        env = os.environ.copy()
        env['GARLIC_SOURCE_MAP_DIR'] = str(target)
        result = subprocess.run([str(engine), str(files[0]), '-o', str(target), '-t', '8'],
                                capture_output=True, timeout=20, env=env)
        assert result.returncode == 0, result.stdout + result.stderr
        for name in ('Main', 'Extra', 'Use', 'DupJoin'):
            assert (target / f'demo/{name}.java').is_file()
            assert json.loads((target / f'demo/{name}.map.json').read_text(encoding='utf-8'))
print('Index / single-class / no-match contract passed for', len(files), 'formats at 1 and 2 threads')
