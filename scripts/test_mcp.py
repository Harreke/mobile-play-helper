# SPDX-License-Identifier: Apache-2.0
"""MCP protocol and validation tests. No phone or native client is launched."""
import json
from pathlib import Path
import subprocess
import sys
import unittest

from mcp_server import TOOLS, validate, Bridge
from unittest.mock import patch


class MCPTests(unittest.TestCase):
    def test_screenshot_mask(self):
        spec = next(t[2] for t in TOOLS if t[0] == 'mph_screenshot')
        for mask in [None, {'x': 0}, {'x': 0, 'y': 0, 'width': float('nan'), 'height': 1},
                     {'x': 0, 'y': 0, 'width': 1, 'height': True}]:
            with self.assertRaises(ValueError):
                validate(spec, {'mask': mask})
        validate(spec, {'mask': {'x': 0, 'y': .95, 'width': .35, 'height': .05}})
        import tempfile
        with tempfile.TemporaryDirectory() as data, patch('mcp_server.time.sleep'):
            bridge = Bridge('.', data)
            with self.assertRaisesRegex(ValueError, 'outside'):
                bridge.call('mph_screenshot', {'mask': {'x': .9, 'y': 0, 'width': .5, 'height': 1}})

    def test_invalid_touch_arguments(self):
        tap = next(t[2] for t in TOOLS if t[0] == 'mph_tap')
        for args in [{'x': -1, 'y': 0}, {'x': float('nan'), 'y': .5},
                     {'x': True, 'y': .5}, {'x': .5, 'y': .5, 'duration_ms': 6000},
                     {'x': .5, 'y': .5, 'shell': 'anything'}, {'x': .5}]:
            with self.assertRaises(ValueError):
                validate(tap, args)
        validate(tap, {'x': .1, 'y': .9, 'duration_ms': 5000})

    def test_profile_path_escape(self):
        spec = next(t[2] for t in TOOLS if t[0] == 'mph_profile')
        for name in ['../personal', 'a/b', 'C:\\test', 'test\nstate', '', 'x' * 65]:
            with self.assertRaises(ValueError):
                validate(spec, {'name': name, 'create': True})

    def test_stdio_round_trip(self):
        import tempfile
        with tempfile.TemporaryDirectory() as data:
            p = subprocess.Popen([sys.executable, str(Path(__file__).with_name('mcp_server.py')), '--data-dir', data],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            requests = [
                {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'protocolVersion': '2025-06-18'}},
                {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
                {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'},
                {'jsonrpc': '2.0', 'id': 3, 'method': 'ping'},
                {'jsonrpc': '2.0', 'id': 4, 'method': 'missing'},
            ]
            stdout, stderr = p.communicate('\n'.join(map(json.dumps, requests)) + '\n', timeout=10)
            self.assertEqual(p.returncode, 0, stderr)
            results = {r['id']: r for r in map(json.loads, stdout.splitlines())}
            self.assertEqual(results[1]['result']['protocolVersion'], '2025-06-18')
            self.assertEqual(len(results[2]['result']['tools']), len(TOOLS))
            self.assertEqual(results[3]['result'], {})
            self.assertEqual(results[4]['error']['code'], -32601)


if __name__ == '__main__':
    unittest.main()
