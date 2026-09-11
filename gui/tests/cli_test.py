#!/usr/bin/env python3
"""Integration contract for the GUI's CLI index and single-class requests."""
from contextlib import contextmanager
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

os.environ["GARLIC_SAFE_SOURCE_PATHS"] = "0"  # Legacy paths tested explicitly below.

engine, fixtures = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
files = [fixtures / 'demo.jar', fixtures / 'demo.zip', fixtures / 'Main.class', fixtures / 'unknown.jar']
files += [p for p in (fixtures / 'classes.dex', fixtures / '示例 app.apk', fixtures / 'nested.apks') if p.exists()]
def run(*args, **kwargs):
    try:
        return subprocess.run(*args, **kwargs)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        details = [f"Command failed: {error.cmd!r}"]
        for label, output in (("stdout", error.stdout), ("stderr", error.stderr)):
            if output:
                if isinstance(output, bytes):
                    output = output.decode("utf-8", errors="replace")
                details.append(f"{label}:\n{output}")
        diagnostic = "\n".join(details)
        print(diagnostic, file=sys.stderr)
        (root / "command-failure.txt").write_text(diagnostic, encoding="utf-8")
        raise


@contextmanager
def test_workspace():
    with tempfile.TemporaryDirectory(prefix="garlic-cli-test-") as temp:
        path = Path(temp)
        try:
            yield path
        except Exception:
            saved = fixtures.parent / "cli-test-failures" / path.name
            saved.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(path, saved)
            print(f"Failed CLI test artifacts: {saved}", file=sys.stderr)
            raise


with test_workspace() as root:
    # Constructors with a Signature attribute must never acquire a void return type.
    generic = root / 'GenericConstructor.java'
    generic.write_text('package demo; public class GenericConstructor { public <T> GenericConstructor(T value) {} }', encoding='utf-8')
    run(['javac', '-d', str(root), str(generic)], check=True, capture_output=True, timeout=20)
    result = run([str(engine), str(root / 'demo/GenericConstructor.class')],
                 check=True, capture_output=True, timeout=20)
    code = result.stdout.decode('utf-8')
    assert 'void GenericConstructor(' not in code and 'GenericConstructor(' in code, code
    generic.write_text(code, encoding='utf-8')
    run(['javac', '-d', str(root / 'generic-check'), str(generic)], check=True, capture_output=True, timeout=20)

    # Encoded paths remain distinct after Windows case folding.
    for mode in (['explicit', 'windows-default'] if os.name == 'nt' else ['explicit']):
        safe = root / ('safe-output-' + mode)
        environment = dict(os.environ, GARLIC_SAFE_SOURCE_PATHS='1')
        if mode == 'windows-default': environment.pop('GARLIC_SAFE_SOURCE_PATHS')
        run([str(engine), str(fixtures / 'cases.dex'), '-o', str(safe)],
            env=environment, check=True, capture_output=True, timeout=20)
        paths = list(safe.rglob('*.java'))
        assert len(paths) == len({str(p).casefold() for p in paths})
        sources = [p.read_text(encoding='utf-8') for p in paths]
        assert any('class Foo' in text for text in sources)
        assert any('class foo' in text for text in sources)

    # Default compact v2 and explicit legacy output retain the same class records.
    indexes = []
    for mode in (None, '0'):
        environment = dict(os.environ)
        environment.pop('GARLIC_COMPACT_INDEX', None)
        if mode is not None: environment['GARLIC_COMPACT_INDEX'] = mode
        target = root / ('format-' + str(mode) + '.jsonl')
        run([str(engine), str(fixtures / 'cases.dex'), '-I', str(target), '-o', str(root / 'format-output')],
            env=environment, check=True, capture_output=True, timeout=20)
        indexes.append([json.loads(line) for line in target.read_text().splitlines()])
    assert all(isinstance(row['refs'], dict) and 'ref_targets' in row for row in indexes[0])
    assert all(isinstance(row['refs'], list) for row in indexes[1])
    assert sorted(row['name'] for row in indexes[0]) == sorted(row['name'] for row in indexes[1])

    # Fast directory mode must preserve archive order, case and class kinds.
    archive_path = root / 'directory.apk'
    with zipfile.ZipFile(archive_path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(fixtures / 'cases.dex', 'classes.dex')
        archive.write(fixtures / 'flattened.dex', 'classes2.dex')
    # Selecting the indexed DEX must preserve single-class output exactly.
    outputs = []
    for entry in ('', 'classes.dex'):
        destination = root / ('selected-dex' if entry else 'all-dex')
        environment = dict(os.environ)
        environment.pop('GARLIC_DEX_ENTRY', None)
        if entry: environment['GARLIC_DEX_ENTRY'] = entry
        run([str(engine), str(archive_path), '-c', 'demo/cases/Foo', '-o', str(destination)],
            env=environment, check=True, capture_output=True, timeout=20)
        outputs.append((destination / 'demo/cases/Foo.java').read_bytes())
    assert outputs[0] == outputs[1]
    listings = []
    for mode, threads in [('full', '1'), ('names', '1'), ('names', '2')]:
        index = root / ('directory-' + mode + threads + '.jsonl')
        environment = dict(os.environ, GARLIC_COMPACT_INDEX='2')
        if mode == 'names': environment['GARLIC_DIRECTORY_INDEX'] = 'names'
        run([str(engine), str(archive_path), '-I', str(index), '-o', str(root / 'directory-out'), '-t', threads],
            env=environment, check=True, capture_output=True, timeout=20)
        listings.append([{key: row.get(key) for key in ('name', 'flags', 'kind', 'inner', 'origin')}
                         for row in map(json.loads, index.read_text(encoding='utf-8').splitlines())])
    assert listings[0] == listings[1] == listings[2], listings
    with zipfile.ZipFile(root / 'broken.apk', 'w') as archive:
        archive.writestr('classes.dex', b'dex\n035\0' + b'\xff' * 104)
    result = run([str(engine), str(root / 'broken.apk'), '-I', str(root / 'broken.jsonl'),
                  '-o', str(root / 'broken-out'), '-t', '2'],
                 env=dict(os.environ, GARLIC_DIRECTORY_INDEX='names'), capture_output=True, timeout=20)
    assert result.returncode != 0, 'Malformed DEX directory accepted'
    # Raw control-flow simulation must propagate values introduced on a backedge.
    raw_output = root / 'flattened-without-deobfuscation'
    for threads in ('1', '4'):
        run([str(engine), str(fixtures / 'flattened.dex'), '-o', str(raw_output), '-t', threads],
            check=True, capture_output=True, timeout=20,
            env=dict(os.environ, GARLIC_UNFLATTEN='0', GARLIC_DEOBFUSCATE='0'))
        raw_code = (raw_output / 'demo/Flattened.java').read_text(encoding='utf-8')
        assert 'compareDispatcher()' in raw_code and 'return i3;' in raw_code

    # Owned smali fixture: constant dispatcher specialization must preserve
    # results, including sparse negative keys and switch default paths.
    flattened = root / 'unflattened'
    env = dict(os.environ, GARLIC_UNFLATTEN='1', GARLIC_DEOBFUSCATE='1')
    run([str(engine), str(fixtures / 'flattened.dex'), '-o', str(flattened), '-t', '1'],
                   check=True, capture_output=True, timeout=20, env=env)
    source = flattened / 'demo/Flattened.java'
    code = source.read_text(encoding='utf-8')
    assert code.count('switch(') == 3, code  # Unknown input joins and custom helpers remain.
    assert 'return -2147483572;' in code, code
    runner = flattened / 'Check.java'
    runner.write_text('''import demo.Flattened;
public class Check {
    public static void main(String[] args) {
        if (Flattened.literal() != -2147483572 || Flattened.run() != 7 || Flattened.sparse() != 5 || Flattened.fallback() != 3
            || Flattened.unknown(0) != 5 || Flattened.unknown(8) != 3
            || Flattened.joinedState(true) != 61 || Flattened.joinedState(false) != 61
            || Flattened.compareDispatcher() != 37
            || Flattened.hashLoop() != 42 || Flattened.movedState() != 23
            || Flattened.negativeState() != 31 || Flattened.customHash() != 5
            || Flattened.branchHash(true) != 42 || Flattened.branchHash(false) != 23 || Flattened.unicodeHash() != 19
            || Flattened.sharedInput(1) != 5 || Flattened.sharedInput(8) != 3)
            throw new AssertionError("Dispatcher specialization changed behavior");
    }
}''', encoding='utf-8')
    run(['javac', '-encoding', 'UTF-8', '-d', str(flattened), str(source), str(runner)], check=True,
                   capture_output=True, timeout=30)
    run(['java', '-cp', str(flattened), 'Check'], check=True,
                   capture_output=True, timeout=20)
    arrays = flattened / 'demo/PrimitiveArrays.java'
    array_code = arrays.read_text(encoding='utf-8')
    assert 'new short[][]' not in array_code and '(null)' not in array_code, array_code
    runner.write_text('''import demo.PrimitiveArrays;
import java.util.Arrays;
public class Check {
    public static void main(String[] args) {
        if (!Arrays.equals(PrimitiveArrays.shorts(), new short[]{-32768,-1,0,32767})) throw new AssertionError("Primitive array payload changed: shorts");
        if (!Arrays.equals(PrimitiveArrays.bytes(), new byte[]{-128,-1,127})) throw new AssertionError("Primitive array payload changed: bytes");
        if (!Arrays.equals(PrimitiveArrays.chars(), new char[]{0,65,65535})) throw new AssertionError("Primitive array payload changed: chars");
        if (!Arrays.equals(PrimitiveArrays.ints(), new int[]{Integer.MIN_VALUE,0,Integer.MAX_VALUE})) throw new AssertionError("Primitive array payload changed: ints");
        if (!Arrays.equals(PrimitiveArrays.longs(), new long[]{Long.MIN_VALUE,-1,Long.MAX_VALUE})) throw new AssertionError("Primitive array payload changed: longs");
        if (!Arrays.equals(PrimitiveArrays.floats(), new float[]{1F,-2.5F})) throw new AssertionError("Primitive array payload changed: floats");
        if (!Arrays.equals(PrimitiveArrays.doubles(), new double[]{1D,-2.5D})) throw new AssertionError("Primitive array payload changed: doubles");
        if (!Arrays.equals(PrimitiveArrays.bools(), new boolean[]{false,true})) throw new AssertionError("Primitive array payload changed: bools");
        if (!Arrays.equals(PrimitiveArrays.filled(), new int[]{3,-2})) throw new AssertionError("Primitive array payload changed: filled");
        if (!Arrays.equals(PrimitiveArrays.filledRange(), new int[]{3,-2})) throw new AssertionError("Primitive array payload changed: filledRange");
        if (PrimitiveArrays.sized(7).length != 7 || PrimitiveArrays.empty().length != 0)
            throw new AssertionError("Primitive array length changed");
        short[] shared = PrimitiveArrays.shared();
        shared[0] = 91;
        if (PrimitiveArrays.shared() != shared || PrimitiveArrays.shared()[0] != 91)
            throw new AssertionError("Static arrays must retain identity and mutation");
        float[] fb = PrimitiveArrays.floatBits();
        double[] db = PrimitiveArrays.doubleBits();
        int[] fi = {0x80000000, 1, 0x7f800000, 0x7fc00123};
        long[] di = {0x8000000000000000L, 1L, 0x7ff0000000000000L, 0x7ff8000000000123L};
        for (int i = 0; i < 4; ++i)
            if (Float.floatToRawIntBits(fb[i]) != fi[i] || Double.doubleToRawLongBits(db[i]) != di[i])
                throw new AssertionError("Floating array bits changed");
        short[] target = {0,0,99};
        if (PrimitiveArrays.fill(target) != target || !Arrays.equals(target,new short[]{4,-7,99}))
            throw new AssertionError("fill-array-data must preserve aliases and remaining elements");
        try { PrimitiveArrays.sized(-1); throw new AssertionError(); }
        catch (NegativeArraySizeException expected) { }
        try { PrimitiveArrays.fill(null); throw new AssertionError(); }
        catch (NullPointerException expected) { }
        try { PrimitiveArrays.fill(new short[1]); throw new AssertionError(); }
        catch (IndexOutOfBoundsException expected) { }
    }
}''', encoding='utf-8')
    run(['javac', '-encoding', 'UTF-8', '-d', str(flattened), str(arrays), str(runner)], check=True,
                   capture_output=True, timeout=30)
    run(['java', '-cp', str(flattened), 'Check'], check=True,
                   capture_output=True, timeout=20)
    run([str(engine), str(fixtures / 'demo.jar'), '-o', str(flattened), '-c', 'demo/Folded', '-t', '1'],
                   check=True, capture_output=True, timeout=20, env=env)
    folded = flattened / 'demo/Folded.java'
    code = folded.read_text(encoding='utf-8')
    assert code.count('decoded: "secret"') >= 2, code
    assert '.substring(20)' in code and 'x / 0' in code, code
    disabled = root / 'fold-disabled'
    run([str(engine), str(fixtures / 'demo.jar'), '-o', str(disabled), '-c', 'demo/Folded', '-t', '1'],
                   check=True, capture_output=True, timeout=20, env=dict(env, GARLIC_DEOBFUSCATE='0'))
    assert 'decoded:' not in (disabled / 'demo/Folded.java').read_text(encoding='utf-8')
    # Decoding comments must not change identity or let hostile strings escape a comment.
    runner.write_text('''import demo.Folded;
public class Check {
    public static void main(String[] args) {
        if (!Folded.text().equals("secret") || !Folded.array().equals("secret")
            || Folded.array() == "secret" || Folded.identity(99) != 0 || Folded.boolIdentity(true))
            throw new AssertionError("String semantics changed");
        try { Folded.unsafe(); throw new AssertionError(); }
        catch (StringIndexOutOfBoundsException expected) { }
        try { Folded.unknown(7); throw new AssertionError(); }
        catch (ArithmeticException expected) { }
    }
}''', encoding='utf-8')
    run(['javac', '-encoding', 'UTF-8', '-d', str(flattened), str(folded), str(runner)], check=True,
                   capture_output=True, timeout=30)
    run(['java', '-cp', str(flattened), 'Check'], check=True,
                   capture_output=True, timeout=20)
    # Round-trip JVM long constants too: Windows C long is only 32 bits.
    wide_input = root / 'wide-input'
    wide_input.mkdir()
    wide_source = wide_input / 'WideConstants.java'
    wide_source.write_text('''package demo;
public class WideConstants {
    public static long low() { return Long.MIN_VALUE; }
    public static long high() { return Long.MAX_VALUE; }
    public static long mixed() { return 0x1234567887654321L; }
}
''', encoding='utf-8')
    run(['javac', '--release', '8', '-encoding', 'UTF-8', '-d', str(wide_input), str(wide_source)],
        check=True, capture_output=True, timeout=30)
    wide_archive = wide_input / 'wide.jar'
    with zipfile.ZipFile(wide_archive, 'w') as archive:
        archive.write(wide_input / 'demo/WideConstants.class', 'demo/WideConstants.class')
    wide_output = root / 'wide-output'
    run([str(engine), str(wide_archive), '-o', str(wide_output), '-t', '1'],
        check=True, capture_output=True, timeout=20)
    runner.write_text('''import demo.WideConstants;
public class Check {
    public static void main(String[] args) {
        if (WideConstants.low() != Long.MIN_VALUE || WideConstants.high() != Long.MAX_VALUE
            || WideConstants.mixed() != 0x1234567887654321L)
            throw new AssertionError("JVM 64-bit constants changed");
    }
}
''', encoding='utf-8')
    run(['javac', '-encoding', 'UTF-8', '-d', str(wide_output),
         str(wide_output / 'demo/WideConstants.java'), str(runner)],
        check=True, capture_output=True, timeout=30)
    run(['java', '-cp', str(wide_output), 'Check'], check=True, capture_output=True, timeout=20)
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
            run([*args, '-I', str(index)], check=True, capture_output=True, timeout=20)
            names = [json.loads(line)['name'] for line in index.read_text(encoding='utf-8').splitlines()]
            assert 'demo/Main' in names, names
            if source.suffix != '.class':
                assert 'demo/Main$Details' in names
            assert not list(out.rglob('*.java')), 'Indexing must not decompile Java'
            assert not list(out.rglob('*.smali')), 'Indexing must not decompile Smali'
            result = run([*args, '-c', 'demo/Main'], check=True, capture_output=True, timeout=20)
            if source.suffix == '.class':
                assert b'class Main' in result.stdout
            else:
                assert (out / 'demo/Main.java').is_file()
                assert not (out / 'demo/Extra.java').exists()
            missing = run([*args, '-c', 'demo/Missing'], capture_output=True, timeout=20)
            assert missing.returncode != 0
    bad = run([str(engine), str(files[0]), '-I'], capture_output=True, timeout=20)
    assert bad.returncode != 0, 'Incomplete index flag must fail, never batch decompile'
    # Exercise concurrent worker startup and full export, beyond the single-class path.
    for attempt in range(12):
        target = unicode_root / f'并行-{attempt}'
        env = os.environ.copy()
        env['GARLIC_SOURCE_MAP_DIR'] = str(target)
        result = run([str(engine), str(files[0]), '-o', str(target), '-t', '8'],
                                capture_output=True, timeout=20, env=env)
        assert result.returncode == 0, result.stdout + result.stderr
        for name in ('Main', 'Extra', 'Use', 'DupJoin'):
            assert (target / f'demo/{name}.java').is_file()
            assert json.loads((target / f'demo/{name}.map.json').read_text(encoding='utf-8'))
print('Index / single-class / no-match contract passed for', len(files), 'formats at 1 and 2 threads')
