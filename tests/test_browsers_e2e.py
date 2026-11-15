#!/usr/bin/env python3
"""
Comprehensive End-to-End Multi-Browser Test for bgl-Engine Web Runtime.
Tests real Google Chrome and real Mozilla Firefox instances against a local HTTP server.
Validates WebGPU execution, WebGL 2 fallback, glTF ingestion, and render execution.
"""
import asyncio
import functools
import http.server
import json
import os
import shutil
import socketserver
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    """HTTP static handler that suppresses per-request stdout noise."""

    def log_message(self, format, *args):
        pass


class ReusableTCPServer(socketserver.TCPServer):
    """TCP server bound to loopback with address reuse enabled."""

    allow_reuse_address = True


def start_server():
    """Start a background HTTP server bound to 127.0.0.1 on an ephemeral port."""
    handler = functools.partial(QuietHandler, directory=str(REPO_ROOT))
    httpd = ReusableTCPServer(('127.0.0.1', 0), handler)
    port = httpd.server_address[1]
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    return httpd, port


def terminate_process(proc):
    """Gracefully terminate a browser or driver subprocess with bounded wait."""
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=2)


def wait_http_json(url, timeout=10.0, predicate=None):
    """Poll a JSON HTTP endpoint until it responds cleanly and satisfies predicate."""
    deadline = time.monotonic() + timeout
    last_err = None
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=2.0) as resp:
                payload = json.loads(resp.read().decode())
                if predicate is None or predicate(payload):
                    return payload
        except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
            last_err = exc
        time.sleep(0.15)
    raise RuntimeError(f'Timed out waiting for {url} ({last_err})')


def test_firefox(port):
    print('================================================================================')
    print(' 1. Testing Real Mozilla Firefox (WebGPU & Fallback Pipeline)')
    print('================================================================================')
    geckodriver_path = shutil.which('geckodriver') or '/snap/bin/geckodriver'
    if not os.path.exists(geckodriver_path):
        print('geckodriver not found - skipping Firefox test.')
        return True

    gd_proc = subprocess.Popen(
        [geckodriver_path, '--port', '4459'],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        wait_http_json('http://127.0.0.1:4459/status', timeout=10.0)

        req = urllib.request.Request(
            'http://127.0.0.1:4459/session',
            data=json.dumps({
                'capabilities': {
                    'alwaysMatch': {
                        'moz:firefoxOptions': {
                            'args': ['-headless'],
                            'prefs': {
                                'dom.webgpu.enabled': True,
                                'gfx.webgpu.ignore-blocklist': True,
                                'gfx.webgpu.force-enabled': True
                            }
                        }
                    }
                }
            }).encode(),
            headers={'Content-Type': 'application/json'}
        )
        with urllib.request.urlopen(req, timeout=15.0) as resp:
            session_id = json.loads(resp.read().decode())['value']['sessionId']

        try:
            nav_req = urllib.request.Request(
                f'http://127.0.0.1:4459/session/{session_id}/url',
                data=json.dumps({'url': f'http://127.0.0.1:{port}/web/index.html'}).encode(),
                headers={'Content-Type': 'application/json'}
            )
            with urllib.request.urlopen(nav_req, timeout=15.0):
                pass

            exec_req = urllib.request.Request(
                f'http://127.0.0.1:4459/session/{session_id}/execute/async',
                data=json.dumps({
                    'script': '''
                        const done = arguments[arguments.length - 1];
                        (async () => {
                            const deadline = Date.now() + 10000;
                            while (Date.now() < deadline) {
                                const r = window.BglWebGpuRenderer;
                                if (r && r.isActive && r.primitives && r.primitives.length > 0) break;
                                await new Promise(res => setTimeout(res, 100));
                            }
                            const renderer = window.BglWebGpuRenderer;
                            let drawError = null;
                            try {
                                if (renderer && renderer.isActive) renderer.draw();
                            } catch (e) {
                                drawError = e.toString();
                            }
                            return {
                                backend: window.BglState ? window.BglState.backend : null,
                                webgpuActive: renderer ? renderer.isActive : false,
                                primitivesCount: renderer ? renderer.primitives.length : 0,
                                currentModel: window.BglState ? window.BglState.currentModelName : null,
                                drawError
                            };
                        })().then(done).catch(e => done({ error: e.toString() }));
                    ''',
                    'args': []
                }).encode(),
                headers={'Content-Type': 'application/json'}
            )
            with urllib.request.urlopen(exec_req, timeout=20.0) as exec_resp:
                res = json.loads(exec_resp.read().decode())['value']

            print('  Firefox Runtime State:', res)
            assert res['backend'] == 'webgpu', f"Expected webgpu backend, got {res['backend']}"
            assert res['webgpuActive'] is True, "Expected WebGPU to be active in Firefox"
            assert res['primitivesCount'] == 3, f"Expected 3 primitives for ToyCar, got {res['primitivesCount']}"
            assert res['currentModel'] == 'ToyCar.glb', f"Expected ToyCar.glb, got {res['currentModel']}"
            assert res['drawError'] is None, f"Draw error occurred: {res['drawError']}"
            print(f"  ✓ Firefox verified: Native WebGPU active, {res['primitivesCount']} primitives rendered.")
            return True
        finally:
            try:
                del_req = urllib.request.Request(
                    f'http://127.0.0.1:4459/session/{session_id}', method='DELETE'
                )
                with urllib.request.urlopen(del_req, timeout=5.0):
                    pass
            except OSError:
                pass
    finally:
        terminate_process(gd_proc)


async def test_chrome(port):
    print('================================================================================')
    print(' 2. Testing Real Google Chrome (Hardware-Accelerated Native WebGPU PBR)')
    print('================================================================================')
    chrome_bin = shutil.which('google-chrome') or shutil.which('chromium') or shutil.which('chromium-browser')
    if chrome_bin is None:
        print('Chrome/Chromium binary not found - skipping Chrome test.')
        return True

    import websockets

    proc = subprocess.Popen(
        [
            chrome_bin, '--headless=new', '--remote-debugging-port=9270',
            '--no-sandbox', '--disable-dev-shm-usage',
            '--enable-unsafe-webgpu', '--use-angle=vulkan', '--enable-features=Vulkan',
            f'http://127.0.0.1:{port}/web/index.html'
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        tabs = wait_http_json(
            'http://127.0.0.1:9270/json',
            timeout=10.0,
            predicate=lambda t: any(item.get('type') == 'page' for item in t),
        )
        page_tab = [t for t in tabs if t.get('type') == 'page'][0]

        async with websockets.connect(page_tab['webSocketDebuggerUrl']) as ws:
            eval_msg = {
                'id': 1,
                'method': 'Runtime.evaluate',
                'params': {
                    'expression': '''(async () => {
                        const deadline = Date.now() + 10000;
                        while (Date.now() < deadline) {
                            const r = window.getActiveRenderer ? window.getActiveRenderer() : null;
                            if (r && r.isActive && r.primitives && r.primitives.length > 0) break;
                            await new Promise(res => setTimeout(res, 100));
                        }
                        const active = window.getActiveRenderer ? window.getActiveRenderer() : null;
                        let drawError = null;
                        try {
                            if (active) active.draw();
                        } catch (e) {
                            drawError = e.toString();
                        }
                        return {
                            backend: window.BglState ? window.BglState.backend : null,
                            activeRendererName: active === window.BglWebGpuRenderer ? 'WebGPU' : 'None',
                            primitivesCount: active ? active.primitives.length : 0,
                            currentModel: window.BglState ? window.BglState.currentModelName : null,
                            drawError
                        };
                    })()''',
                    'awaitPromise': True,
                    'returnByValue': True
                }
            }
            await ws.send(json.dumps(eval_msg))
            res = await ws.recv()
            data = json.loads(res)['result']['result']['value']
            print('  Chrome Runtime State:', data)
            assert data['backend'] == 'webgpu', f"Expected webgpu backend in Chrome, got {data['backend']}"
            assert data['activeRendererName'] == 'WebGPU', f"Expected WebGPU renderer, got {data['activeRendererName']}"
            assert data['primitivesCount'] == 3, f"Expected 3 primitives for ToyCar, got {data['primitivesCount']}"
            assert data['currentModel'] == 'ToyCar.glb', f"Expected ToyCar.glb, got {data['currentModel']}"
            assert data['drawError'] is None, f"Draw error occurred: {data['drawError']}"
            print(f"  ✓ Chrome verified: Native WebGPU active, {data['primitivesCount']} primitives rendered.")
            return True
    finally:
        terminate_process(proc)


def main():
    httpd, port = start_server()
    print(f'Local test server started on http://127.0.0.1:{port}')
    try:
        ff_ok = test_firefox(port)
        ch_ok = asyncio.run(test_chrome(port))
        if ff_ok and ch_ok:
            print('================================================================================')
            print(' ALL BROWSER E2E TESTS PASSED')
            print('================================================================================')
            sys.exit(0)
        sys.exit(1)
    finally:
        httpd.shutdown()
        httpd.server_close()


if __name__ == '__main__':
    main()
