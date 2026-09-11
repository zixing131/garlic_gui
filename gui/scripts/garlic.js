'use strict';
const { spawn } = require('node:child_process');
const fs = require('node:fs');
class Garlic {
  constructor() {
    this.context = process.env.GARLIC_SCRIPT_CONTEXT ? JSON.parse(fs.readFileSync(process.env.GARLIC_SCRIPT_CONTEXT, 'utf8')) : {};
    this.arguments = this.context.arguments || {};
    this.child = null; this.nextId = 0; this.pending = new Map(); this.queue = Promise.resolve();
  }
  request(method, params = {}) {
    // Serialize calls: the decompiler and search have a single foreground queue.
    const request = () => new Promise((resolve, reject) => {
      if (!this.child) {
        const child = this.child = spawn(process.env.GARLIC_SCRIPT_HOST, ['--mcp', '--socket', process.env.GARLIC_SCRIPT_SOCKET], {stdio: ['pipe', 'pipe', 'inherit'], windowsHide: true});
        let buffer = '';
        child.stdout.setEncoding('utf8');
        child.stdout.on('data', data => {
          buffer += data;
          let end;
          while ((end = buffer.indexOf('\n')) >= 0) {
            const line = buffer.slice(0, end); buffer = buffer.slice(end + 1);
            try {
              const response = JSON.parse(line), task = this.pending.get(response.id);
              if (task) { this.pending.delete(response.id); response.error ? task.reject(new Error(JSON.stringify(response.error))) : task.resolve(response.result); }
            } catch (error) { this.fail(error); }
          }
        });
        child.on('error', error => this.fail(error));
        child.stdin.on('error', error => this.fail(error));
        child.on('exit', () => { if (this.child === child) this.child = null; this.fail(new Error('Garlic connection closed')); });
      }
      const id = ++this.nextId;
      this.pending.set(id, {resolve, reject});
      this.child.stdin.write(JSON.stringify({jsonrpc: '2.0', id, method, params}) + '\n');
    });
    const result = this.queue.then(request);
    this.queue = result.catch(() => {});
    return result;
  }
  fail(error) { for (const task of this.pending.values()) task.reject(error); this.pending.clear(); }
  async tools() { return (await this.request('tools/list')).tools; }
  async call(name, args = {}) {
    const result = await this.request('tools/call', {name, arguments: args});
    const text = (result.content || []).filter(item => item.type === 'text').map(item => item.text).join('\n');
    const value = text ? JSON.parse(text) : result;
    if (result.isError) throw new Error(value.error || text);
    return value;
  }
  async *classes(count = 500) {
    let offset = 0;
    while (true) {
      const page = (await this.call('get_all_classes', {offset, count})).classes;
      if (!page.length) return;
      yield* page; offset += page.length;
    }
  }
  close() { if (this.child) { this.child.kill(); this.child = null; } this.fail(new Error('Script ended')); }
}
const client = new Garlic();
const api = new Proxy(client, {get(target, name) {
  if (name === 'then') return undefined;
  if (name in target) return typeof target[name] === 'function' ? target[name].bind(target) : target[name];
  return args => target.call(name, args);
}});
globalThis.garlic = api;
module.exports = api;
process.on('exit', () => client.close());
process.on('SIGTERM', () => { client.close(); process.exit(143); });
