"""Garlic scripting client. Standard library only; calls target this GUI session."""
import atexit
import json
import os
from pathlib import Path
import signal
import subprocess
import threading

class GarlicError(RuntimeError):
    pass

class Garlic:
    def __init__(self):
        self._process = None
        self._id = 0
        self._lock = threading.Lock()
        path = os.environ.get('GARLIC_SCRIPT_CONTEXT')
        self.context = json.loads(Path(path).read_text(encoding='utf-8')) if path else {}
        self.arguments = self.context.get('arguments', {})

    def _request(self, method, params=None):
        with self._lock:
            if self._process is None:
                self._process = subprocess.Popen(
                    [os.environ['GARLIC_SCRIPT_HOST'], '--mcp', '--socket', os.environ['GARLIC_SCRIPT_SOCKET']],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding='utf-8', bufsize=1)
            self._id += 1
            request = json.dumps(dict(jsonrpc='2.0', id=self._id, method=method, params=params or {}), ensure_ascii=False)
            self._process.stdin.write(request + '\n')
            self._process.stdin.flush()
            line = self._process.stdout.readline()
            if not line:
                raise GarlicError('Garlic connection closed; the project or script session may have ended')
            reply = json.loads(line)
            if reply.get('id') != self._id:
                raise GarlicError('Unexpected response id')
            if 'error' in reply:
                raise GarlicError(str(reply['error']))
            return reply['result']

    def tools(self):
        """Return live tool names, descriptions and input schemas."""
        return self._request('tools/list')['tools']

    def call(self, name, arguments=None, **kwargs):
        args = dict(arguments or {})
        args.update(kwargs)
        result = self._request('tools/call', dict(name=name, arguments=args))
        text = '\n'.join(item.get('text', '') for item in result.get('content', []) if item.get('type') == 'text')
        value = json.loads(text) if text else result
        if result.get('isError'):
            raise GarlicError(value.get('error', text) if isinstance(value, dict) else text)
        return value

    def __getattr__(self, name):
        if name.startswith('_'):
            raise AttributeError(name)
        return lambda **kwargs: self.call(name, kwargs)

    def classes(self, count=500):
        """Iterate all classes with bounded pages."""
        offset = 0
        while True:
            page = self.call('get_all_classes', offset=offset, count=count)['classes']
            if not page:
                return
            yield from page
            offset += len(page)

    def close(self):
        process, self._process = self._process, None
        if process:
            process.terminate()
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

api = Garlic()
atexit.register(api.close)
if threading.current_thread() is threading.main_thread():
    def _stop(signum, frame):
        api.close()
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, _stop)
