"""Make QtTest diagnostics visible to CTest when Windows GUI stdout is unavailable."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='garlic-qttest-') as folder:
    log = Path(folder) / 'results.txt'
    try:
        result = subprocess.run([sys.argv[1], '-o', str(log) + ',txt'],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=110)
        if result.stdout:
            print(result.stdout.decode('utf-8', errors='replace'))
        code = result.returncode
    except subprocess.TimeoutExpired:
        print('QtTest timed out after 110 seconds')
        code = 1
    if log.exists():
        print(log.read_text(encoding='utf-8', errors='replace'))
    else:
        print('QtTest did not produce its result log')
        code = code or 1
    sys.exit(1 if code else 0)
