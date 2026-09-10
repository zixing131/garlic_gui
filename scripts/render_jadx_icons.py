#!/usr/bin/env python3
"""Rebuild embedded HiDPI node icons from the unmodified jadx SVGs using librsvg."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1] / 'gui/icons/jadx'
entries = []
for source in sorted(root.glob('*.svg')):
    for scale in range(1, 5):
        target = root / 'png' / f'{source.stem}@{scale}x.png'
        target.parent.mkdir(exist_ok=True)
        subprocess.run(['rsvg-convert', '-w', str(16 * scale), '-h', str(16 * scale),
                        '-o', str(target), str(source)], check=True)
        entries.append(f'    <file>{target.relative_to(root).as_posix()}</file>')
(root / 'jadx_icons.qrc').write_text(
    '<RCC>\n  <qresource prefix="/jadx">\n' + '\n'.join(entries) +
    '\n  </qresource>\n</RCC>\n', encoding='utf-8')
