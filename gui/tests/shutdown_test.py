"""Exercise actual GUI process exit, including detached workspace cleanup."""
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time

exe, engine, fixtures = map(Path, sys.argv[1:4])
env = dict(os.environ, QT_QPA_PLATFORM='offscreen')

def probe(apk, delay):
    lines = queue.Queue()
    process = subprocess.Popen([str(exe), '--engine', str(engine), '--shutdown-probe', str(delay), str(apk)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    def reader():
        for line in process.stdout:
            lines.put((time.monotonic(), line.rstrip()))
    threading.Thread(target=reader, daemon=True).start()
    workspace = None
    try:
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            try:
                stamp, line = lines.get(timeout=0.1)
            except queue.Empty:
                assert process.poll() is None, 'GUI exited before shutdown marker'
                continue
            if line.startswith('SHUTDOWN_BEGIN '):
                workspace = Path(line.split(' ', 1)[1])
                break
        assert workspace is not None, 'Shutdown probe timed out'
        assert process.wait(timeout=4) == 0
        elapsed = time.monotonic() - stamp
        assert elapsed < 3, f'Exit took {elapsed:.2f}s'
        deadline = time.monotonic() + 15
        while workspace.exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        assert not workspace.exists(), f'Workspace cleanup failed: {workspace}'
        print(f'{apk.name}: close {elapsed:.3f}s; cache removed', flush=True)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)

probe(fixtures / 'resources.apks', 0)
large = os.environ.get('GARLIC_TEST_LIFECYCLE_APK')
if large:
    probe(Path(large), 1000)
    probe(Path(large), 0)
with tempfile.TemporaryDirectory(prefix='garlic-protected-') as directory:
    marker = Path(directory) / 'keep.txt'
    marker.write_text('keep')
    result = subprocess.run([str(exe), '--cleanup-workspaces', directory], timeout=5, env=env)
    assert result.returncode == 2 and marker.read_text() == 'keep'
