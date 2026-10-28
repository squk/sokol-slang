// Run an Emscripten test in an isolated headless Chromium profile. Node 22+
// supplies WebSocket and fetch, so no npm dependencies are needed.
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { dirname, basename, resolve, sep } from 'node:path';
import { tmpdir } from 'node:os';
import { setTimeout as delay } from 'node:timers/promises';

const [browser, input] = process.argv.slice(2);
if (!browser || !input) throw new Error('usage: node run.mjs /path/to/chromium /path/to/test.html');
const root = dirname(resolve(input));
const profile = await mkdtemp(resolve(tmpdir(), 'sokol-browser-'));
const server = createServer(async (request, response) => {
  try {
    const name = resolve(root, '.' + decodeURIComponent(new URL(request.url, 'http://localhost').pathname));
    if (!name.startsWith(root + sep)) throw new Error('invalid path');
    const data = await readFile(name);
    const type = name.endsWith('.wasm') ? 'application/wasm' : name.endsWith('.js') ? 'text/javascript' : 'text/html';
    response.writeHead(200, { 'Content-Type': type });
    response.end(data);
  } catch {
    response.writeHead(404); response.end();
  }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const child = spawn(browser, ['--headless=new', '--no-first-run', '--no-default-browser-check',
  '--disable-background-networking', '--enable-unsafe-webgpu', '--enable-unsafe-swiftshader',
  '--remote-debugging-port=0', '--user-data-dir=' + profile, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
let diagnostics = '';
child.stderr.on('data', chunk => { diagnostics = (diagnostics + chunk).slice(-16000); });
let spawnError;
child.on('error', error => { spawnError = error; });
let socket;
let timedOut = false;
const timeout = setTimeout(() => { timedOut = true; child.kill('SIGKILL'); }, 60000);
try {
  let port;
  while (!port) {
    if (spawnError) throw spawnError;
    if (timedOut || child.exitCode !== null || child.signalCode) throw new Error('browser exited before debugger startup\n' + diagnostics);
    try { port = (await readFile(resolve(profile, 'DevToolsActivePort'), 'utf8')).split('\n')[0]; }
    catch { await delay(100); }
  }
  const pageUrl = `http://127.0.0.1:${server.address().port}/${encodeURIComponent(basename(input))}`;
  const page = await (await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(pageUrl)}`, { method: 'PUT' })).json();
  socket = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
  let nextId = 0;
  const pending = new Map();
  socket.onmessage = event => {
    const message = JSON.parse(event.data);
    if (pending.has(message.id)) {
      const { resolve, reject } = pending.get(message.id);
      pending.delete(message.id);
      if (message.error) reject(new Error(JSON.stringify(message.error))); else resolve(message.result);
    }
  };
  socket.onclose = () => {
    for (const { reject } of pending.values()) reject(new Error('debugger closed\n' + diagnostics));
    pending.clear();
  };
  const evaluate = expression => new Promise((resolve, reject) => {
    const id = ++nextId;
    pending.set(id, { resolve, reject });
    socket.send(JSON.stringify({ id, method: 'Runtime.evaluate', params: { expression, returnByValue: true } }));
  });
  let result;
  while (!result) {
    if (timedOut) throw new Error('browser test timed out\n' + diagnostics);
    const evaluated = await evaluate('globalThis.testResult || null');
    result = evaluated.result.value;
    if (!result) await delay(100);
  }
  console.log(result.output.join('\n'));
  if (result.status !== 0) throw new Error(`browser test exited with ${result.status}`);
} finally {
  clearTimeout(timeout);
  socket?.close();
  if (!spawnError && child.exitCode === null && !child.signalCode) {
    const stopped = new Promise(resolve => child.once('exit', resolve));
    child.kill();
    const forceStop = setTimeout(() => child.kill('SIGKILL'), 2000);
    await stopped;
    clearTimeout(forceStop);
  }
  server.closeAllConnections();
  await new Promise(resolve => server.close(resolve));
  await rm(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
}
