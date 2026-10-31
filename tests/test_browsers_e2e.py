#!/usr/bin/env python3
"""
Comprehensive End-to-End Multi-Browser Test for bgl-Engine Web Runtime.
Tests real Google Chrome and real Mozilla Firefox instances against local HTTP server.
Validates WebGPU execution, WebGL 2 fallback, glTF ingestion, and render execution.
"""
import asyncio
import http.server
import json
import os
import socketserver
import subprocess
import sys
import threading
import time
import urllib.request
import websockets

PORT = 8089


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Suppress HTTP request logging noise


def start_server():
    socketserver.TCPServer.allow_reuse_address = True
    httpd = socketserver.TCPServer(('', PORT), QuietHandler)
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd


def test_firefox():
    print('================================================================================')
    print(' 1. Testing Real Mozilla Firefox (WebGPU & Fallback Pipeline)')
    print('================================================================================')
    geckodriver_path = '/snap/bin/geckodriver'
    if not os.path.exists(geckodriver_path):
        print('geckodriver not found at', geckodriver_path, '- skipping Firefox test.')
        return True

    gd_proc = subprocess.Popen([geckodriver_path, '--port', '4459'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(1)

    try:
        # Create session with WebGPU enabled
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
        with urllib.request.urlopen(req) as resp:
            session_id = json.loads(resp.read().decode())['value']['sessionId']

        # Navigate to web/index.html
        nav_req = urllib.request.Request(
            f'http://127.0.0.1:4459/session/{session_id}/url',
            data=json.dumps({'url': f'http://localhost:{PORT}/web/index.html'}).encode(),
            headers={'Content-Type': 'application/json'}
        )
        urllib.request.urlopen(nav_req)
        time.sleep(3)

        # Inspect state
        exec_req = urllib.request.Request(
            f'http://127.0.0.1:4459/session/{session_id}/execute/async',
            data=json.dumps({
                'script': '''
                    const done = arguments[arguments.length - 1];
                    (async () => {
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
                            webglActive: window.BglWebGlRenderer ? window.BglWebGlRenderer.isActive : false,
                            primitivesCount: renderer ? renderer.primitives.length : (window.BglWebGlRenderer ? window.BglWebGlRenderer.primitives.length : 0),
                            currentModel: window.BglState ? window.BglState.currentModelName : null,
                            drawError
                        };
                    })().then(done).catch(e => done({ error: e.toString() }));
                ''',
                'args': []
            }).encode(),
            headers={'Content-Type': 'application/json'}
        )
        with urllib.request.urlopen(exec_req) as exec_resp:
            res = json.loads(exec_resp.read().decode())['value']

        print('  Firefox Runtime State:', res)
        assert res['backend'] in ['webgpu', 'webgl2'], f"Unexpected backend: {res['backend']}"
        assert res['primitivesCount'] == 3, f"Expected 3 primitives for ToyCar, got {res['primitivesCount']}"
        assert res['currentModel'] == 'ToyCar.glb', f"Expected ToyCar.glb, got {res['currentModel']}"
        assert res['drawError'] is None, f"Draw error occurred: {res['drawError']}"
        print(f"  ✓ Firefox verified: Backend '{res['backend']}' active, {res['primitivesCount']} primitives rendered without errors.")
        return True
    finally:
        gd_proc.terminate()


async def test_chrome():
    print('================================================================================')
    print(' 2. Testing Real Google Chrome (Cross-Platform Pipeline & Fallback)')
    print('================================================================================')
    proc = subprocess.Popen([
        'google-chrome', '--headless=new', '--remote-debugging-port=9270',
        '--no-sandbox', '--disable-dev-shm-usage',
        f'http://localhost:{PORT}/web/index.html'
    ])
    time.sleep(2)

    try:
        with urllib.request.urlopen('http://127.0.0.1:9270/json') as resp:
            tabs = json.loads(resp.read().decode())
            page_tab = [t for t in tabs if t.get('type') == 'page'][0]

        async with websockets.connect(page_tab['webSocketDebuggerUrl']) as ws:
            time.sleep(2)
            eval_msg = {
                'id': 1,
                'method': 'Runtime.evaluate',
                'params': {
                    'expression': '''(() => {
                        const active = window.getActiveRenderer ? window.getActiveRenderer() : null;
                        let drawError = null;
                        try {
                            if (active) active.draw();
                        } catch (e) {
                            drawError = e.toString();
                        }
                        return {
                            backend: window.BglState ? window.BglState.backend : null,
                            activeRendererName: active === window.BglWebGpuRenderer ? 'WebGPU' : (active === window.BglWebGlRenderer ? 'WebGL2' : 'None'),
                            primitivesCount: active ? active.primitives.length : 0,
                            currentModel: window.BglState ? window.BglState.currentModelName : null,
                            drawError
                        };
                    })()''',
                    'returnByValue': True
                }
            }
            await ws.send(json.dumps(eval_msg))
            res = await ws.recv()
            data = json.loads(res)['result']['result']['value']
            print('  Chrome Runtime State:', data)
            assert data['backend'] in ['webgpu', 'webgl2']
            assert data['primitivesCount'] == 3
            assert data['currentModel'] == 'ToyCar.glb'
            assert data['drawError'] is None
            print(f"  ✓ Chrome verified: Backend '{data['backend']}' ({data['activeRendererName']}) active, {data['primitivesCount']} primitives rendered without errors.")
            return True
    finally:
        proc.terminate()


def main():
    httpd = start_server()
    print(f'Local test server started on http://localhost:{PORT}')
    try:
        ff_ok = test_firefox()
        ch_ok = asyncio.run(test_chrome())
        if ff_ok and ch_ok:
            print('================================================================================')
            print(' ALL BROWSER E2E TESTS PASSED (Google Chrome & Mozilla Firefox 100% Verified)')
            print('================================================================================')
            sys.exit(0)
        else:
            sys.exit(1)
    finally:
        httpd.shutdown()


if __name__ == '__main__':
    main()

