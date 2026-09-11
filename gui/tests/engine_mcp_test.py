"""Exercise the native engine's MCP session without closing stdin between calls."""
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading

engine, fixtures = map(lambda p: Path(p).resolve(), sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='garlic-mcp-session-') as folder:
    root = Path(folder)
    with (root / 'stderr.log').open('wb') as errors:
        process = subprocess.Popen([str(engine), '-m'], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=errors)
        replies = queue.Queue()
        def read():
            for line in process.stdout:
                try:
                    message = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    continue
                if isinstance(message, dict) and 'id' in message:
                    replies.put(message)
            replies.put(None)
        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        serial = 0
        def call(method, **params):
            global serial
            serial += 1
            request = {'jsonrpc': '2.0', 'id': serial, 'method': method, 'params': params}
            process.stdin.write((json.dumps(request) + '\n').encode())
            process.stdin.flush()
            response = replies.get(timeout=20)
            assert response is not None, f'MCP died during request {serial}'
            assert response['id'] == serial, response
            return response
        try:
            assert 'result' in call('initialize')
            initial = call('tools/list')['result']['tools']
            assert initial
            for i in range(30):
                response = call('tools/call', name='dump_info', arguments={'path': str(root / 'missing.dex')})
                assert 'error' in response, response
                assert 'error' in call('tools/call', name='unknown')
                assert 'error' in call('tools/call', name='dump_info', arguments=[])
                assert call('tools/list')['result']['tools'] == initial
            for name in ('dump_info', 'decompile', 'call_graph') * 3:
                response = call('tools/call', name=name, arguments={
                    'path': str(fixtures / 'cases.dex'), 'output_dir': str(root / name)})
                assert 'result' in response, response
                text = response['result']['content'][0]['text']
                assert text and 'Error:' not in text and 'err:' not in text, text
                assert call('tools/list')['result']['tools'] == initial
            assert process.poll() is None
            process.stdin.close()
            assert process.wait(timeout=10) == 0
            reader.join(timeout=2)
            print('Native MCP: 99 mixed tool calls, tool list intact, clean EOF shutdown')
        except BaseException:
            print((root / 'stderr.log').read_text(errors='replace'), file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
