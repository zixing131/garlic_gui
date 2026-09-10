#!/usr/bin/env python3
"""Bundle a native Garlic GUI, its matching engine and Qt runtime; smoke-test before archiving."""
import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def architecture(path, platform):
    expected = 'arm64' if platform.endswith('arm64') else 'x64'
    data = path.read_bytes()[:4096]
    if platform.startswith('windows'):
        offset = struct.unpack_from('<I', data, 0x3c)[0]
        machine = struct.unpack_from('<H', data, offset + 4)[0]
        assert machine == {'arm64': 0xaa64, 'x64': 0x8664}[expected], (path, hex(machine))
    elif platform.startswith('linux'):
        machine = struct.unpack_from('<H', data, 18)[0]
        assert machine == {'arm64': 183, 'x64': 62}[expected], (path, machine)
    else:
        archs = subprocess.check_output(['lipo', '-archs', str(path)], text=True).split()
        assert {'arm64': 'arm64', 'x64': 'x86_64'}[expected] in archs, (path, archs)


def windows_dependencies(folder, qt):
    """windeployqt does not copy every MSYS2 transitive library; inspect PE imports recursively."""
    objdump = qt / 'bin/llvm-objdump.exe'
    available = {p.name.lower(): p for p in (qt / 'bin').glob('*.dll')}
    queue = list(folder.rglob('*.dll')) + list(folder.rglob('*.exe'))
    inspected = set()
    while queue:
        binary = queue.pop()
        if binary in inspected:
            continue
        inspected.add(binary)
        output = subprocess.check_output([str(objdump), '-p', str(binary)], text=True)
        for name in re.findall(r'DLL Name:\s*(\S+)', output):
            source = available.get(name.lower())
            if source:
                destination = folder / source.name
                if not destination.exists():
                    shutil.copy2(source, destination)
                    queue.append(destination)


def linux_dependencies(folder):
    # The host supplies glibc, its loader and GPU drivers. Everything else resolved by ldd is bundled.
    excluded = re.compile(r'^(ld-linux|lib(c|m|pthread|dl|rt|resolv|util)\.so|lib(GL|GLX|GLdispatch|EGL|drm|gbm))')
    libs = folder / 'lib'
    libs.mkdir(exist_ok=True)
    queue = list((folder / 'bin').iterdir()) + list((folder / 'plugins').rglob('*.so'))
    seen = set()
    while queue:
        binary = queue.pop()
        if binary in seen:
            continue
        seen.add(binary)
        with binary.open('rb') as candidate:
            if candidate.read(4) != b'\x7fELF':
                continue  # qt.conf and launcher scripts have no ELF dependencies.
        output = subprocess.check_output(['ldd', str(binary)], text=True)
        if 'not found' in output:
            raise RuntimeError(f'Unresolved dependency for {binary}:\n{output}')
        for name, source in re.findall(r'\s*(\S+)\s+=>\s+(/\S+)', output):
            if excluded.match(name):
                continue
            destination = libs / name
            if not destination.exists():
                shutil.copy2(source, destination)
                queue.append(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--platform', choices=['macos-arm64', 'macos-x64', 'windows-arm64', 'windows-x64', 'linux-arm64', 'linux-x64'], required=True)
    parser.add_argument('--qt', type=Path, required=True)
    args = parser.parse_args()
    build, qt, output = args.build.resolve(), args.qt.resolve(), args.output.resolve()
    folder = output / ('Garlic-' + args.platform)
    if folder.exists():
        raise SystemExit(f'Refusing to overwrite an existing package directory: {folder}')
    folder.mkdir(parents=True)
    root = Path(__file__).resolve().parents[1]
    shutil.copy2(root / 'LICENSE', folder / 'LICENSE')
    notices = folder / 'ThirdParty' / 'jadx-icons'
    notices.mkdir(parents=True, exist_ok=True)
    for name in ('LICENSE', 'NOTICE', 'README.md'):
        shutil.copy2(root / 'gui/icons/jadx' / name, notices / name)
    shutil.copy2(root / 'docs/garlic-gui.md', folder / 'README.md')
    for candidate in [qt / 'Licenses', qt / 'licenses', qt / 'share/licenses/qt6-base']:
        if candidate.is_dir():
            shutil.copytree(candidate, folder / 'Qt-Licenses', dirs_exist_ok=True)
    env = os.environ.copy()
    if args.platform.startswith('macos'):
        app = folder / 'Garlic.app'
        shutil.copytree(build / 'gui/garlic-gui.app', app, symlinks=True)
        run(qt / 'bin/macdeployqt', app, '-always-overwrite')
        run('codesign', '--force', '--deep', '--sign', '-', app)
        run('codesign', '--verify', '--deep', '--strict', app)
        gui, engine = app / 'Contents/MacOS/garlic-gui', app / 'Contents/MacOS/garlic'
        env.pop('QT_QPA_PLATFORM', None)
    elif args.platform.startswith('windows'):
        gui, engine = folder / 'garlic-gui.exe', folder / 'garlic.exe'
        shutil.copy2(build / 'gui/garlic-gui.exe', gui)
        shutil.copy2(build / 'garlic.exe', engine)
        deploy = next((p for p in [qt / 'bin/windeployqt6.exe', qt / 'bin/windeployqt.exe'] if p.exists()), None)
        if not deploy:
            raise RuntimeError('windeployqt executable not found')
        run(deploy, '--release', '--no-translations', '--compiler-runtime', gui)
        windows_dependencies(folder, qt)
        # Detect missing runtime DLLs rather than accidentally resolving them from the SDK PATH.
        env['PATH'] = os.pathsep.join([str(folder), str(Path(os.environ['SystemRoot']) / 'System32'), os.environ['SystemRoot']])
    else:
        (folder / 'bin').mkdir()
        gui, engine = folder / 'bin/garlic-gui', folder / 'bin/garlic'
        shutil.copy2(build / 'gui/garlic-gui', gui)
        shutil.copy2(build / 'garlic', engine)
        plugins = folder / 'plugins'
        for name in ['platforms', 'platforminputcontexts', 'imageformats', 'xcbglintegrations', 'tls']:
            source = qt / 'plugins' / name
            if source.exists():
                shutil.copytree(source, plugins / name)
        (folder / 'bin/qt.conf').write_text('[Paths]\nPlugins=../plugins\nLibraries=../lib\n')
        linux_dependencies(folder)
        launcher = folder / 'Garlic'
        launcher.write_text('#!/bin/sh\nHERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexport LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\nexec "$HERE/bin/garlic-gui" "$@"\n')
        launcher.chmod(0o755)
        env['LD_LIBRARY_PATH'] = str(folder / 'lib')
        env['QT_QPA_PLATFORM'] = 'offscreen'
    architecture(gui, args.platform)
    architecture(engine, args.platform)
    if args.platform.startswith('windows'):
        for binary in folder.rglob('*.dll'):
            architecture(binary, args.platform)
    run(gui, '--version', env=env, timeout=30)
    run(gui, '--smoke-test', env=env, timeout=30)
    smoke = output / ('smoke-' + args.platform)
    smoke.mkdir()
    run(engine, build / 'fixtures/demo.jar', '-I', smoke / 'classes.jsonl', '-o', smoke, '-t', '1', env=env, timeout=30, stdout=subprocess.DEVNULL)
    assert (smoke / 'classes.jsonl').stat().st_size > 0
    if args.platform.startswith('macos'):
        archive = str(output / ('Garlic-' + args.platform + '.zip'))
        run('ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', folder, archive)
    else:
        archive = shutil.make_archive(str(output / ('Garlic-' + args.platform)), 'zip', output, folder.name)
    print(archive)


if __name__ == '__main__':
    main()
