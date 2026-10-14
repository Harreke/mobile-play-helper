# SPDX-License-Identifier: Apache-2.0
"""Dependency-free MCP stdio server for the native MobilePlayHelper client.

No TCP listener, desktop automation, arbitrary shell, or arbitrary ADB commands.
The private per-session mailbox is enabled only for the child we start.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse
import base64
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import uuid


def schema(properties=None, required=()):
    return {'type': 'object', 'properties': properties or {},
            'required': list(required), 'additionalProperties': False}


NUMBER = {'type': 'number', 'minimum': 0, 'maximum': 1}
KEY = {'type': 'integer', 'minimum': 1, 'maximum': 517,
       'description': 'SDL scancode (e.g. Space=44, R=21) or Mouse1=513, Mouse2=515, Mouse3=514.'}
NAME = {'type': 'string', 'pattern': '^[A-Za-z0-9_-]{1,64}$'}
TOOLS = [
    ('mph_start', 'Start a dedicated MCP-controlled client. Existing personal profiles are not modified.',
     schema({'serial': {'type': 'string', 'maxLength': 128}, 'profile': NAME})),
    ('mph_stop', 'Release input and close only the client owned by this MCP server.', schema()),
    ('mph_state', 'Read live video size, FPS, mapping/editor state and normalized bindings.', schema()),
    ('mph_screenshot', 'Capture real video and toolbar as PNG. Optional mask hides one sensitive rectangle in the exported phone image only.',
     schema({'settle_ms': {'type': 'integer', 'minimum': 0, 'maximum': 2000},
             'mask': schema({'x': NUMBER, 'y': NUMBER, 'width': NUMBER, 'height': NUMBER}, ['x', 'y', 'width', 'height'])})),
    ('mph_mapping', 'Enable or disable the normal keyboard/mouse mapper. Editor must be closed.',
     schema({'enabled': {'type': 'boolean'}}, ['enabled'])),
    ('mph_editor', 'Begin, save, or discard layout edits. Editing never sends touch events.',
     schema({'action': {'type': 'string', 'enum': ['begin', 'save', 'discard']}}, ['action'])),
    ('mph_profile', 'Create or load a named profile in this MCP server data directory. Never overwrites on create.',
     schema({'name': NAME, 'create': {'type': 'boolean'}}, ['name', 'create'])),
    ('mph_binding', 'Add or move one mapping, or delete it. Requires editor mode. Changes remain unsaved.',
     schema({'key': KEY, 'x': NUMBER, 'y': NUMBER, 'delete': {'type': 'boolean'}}, ['key'])),
    ('mph_zone', 'Set the joystick or look zone while editing; normalized coordinates, radius relative to height.',
     schema({'zone': {'type': 'string', 'enum': ['joystick', 'look']}, 'x': NUMBER, 'y': NUMBER,
             'radius': {'type': 'number', 'exclusiveMinimum': 0, 'maximum': .4},
             'sensitivity': {'type': 'number', 'exclusiveMinimum': 0, 'maximum': .05}},
            ['zone', 'x', 'y', 'radius'])),
    ('mph_tap', 'Send a real bounded single-finger tap or long press to Android. Ordinary mode only. Verify effects with a new screenshot.',
     schema({'x': NUMBER, 'y': NUMBER, 'duration_ms': {'type': 'integer', 'minimum': 50, 'maximum': 5000}}, ['x', 'y'])),
    ('mph_swipe', 'Send a real interpolated single-finger swipe. Coordinates refer to the full phone frame, not the toolbar. Ordinary mode only.',
     schema({'x': NUMBER, 'y': NUMBER, 'to_x': NUMBER, 'to_y': NUMBER,
             'duration_ms': {'type': 'integer', 'minimum': 50, 'maximum': 5000}},
            ['x', 'y', 'to_x', 'to_y'])),
    ('mph_cancel', 'Request release of an active gesture and disable mapping. No game-success guarantee.', schema()),
]


def validate(spec, args):
    if not isinstance(args, dict):
        raise ValueError('arguments must be an object')
    if set(args) - set(spec['properties']) or set(spec['required']) - set(args):
        raise ValueError('unknown or missing argument')
    for key, value in args.items():
        rule = spec['properties'][key]
        kind = rule['type']
        if kind == 'object':
            validate(rule, value)
        if kind == 'boolean' and type(value) is not bool:
            raise ValueError(key + ' must be boolean')
        if kind in ('number', 'integer'):
            if type(value) not in (int, float) or not math.isfinite(value) or (kind == 'integer' and type(value) is not int):
                raise ValueError(key + ' must be finite ' + kind)
            if ('minimum' in rule and value < rule['minimum'] or
                'maximum' in rule and value > rule['maximum'] or
                'exclusiveMinimum' in rule and value <= rule['exclusiveMinimum']):
                raise ValueError(key + ' outside range')
        if kind == 'string':
            if not isinstance(value, str) or '\n' in value or '\r' in value or '\0' in value:
                raise ValueError(key + ' must be a single-line string')
            if len(value) > rule.get('maxLength', 128) or ('pattern' in rule and not re.fullmatch(rule['pattern'], value)):
                raise ValueError(key + ' invalid')
        if 'enum' in rule and value not in rule['enum']:
            raise ValueError(key + ' invalid choice')


class Bridge:
    def __init__(self, root, data_dir):
        self.root = Path(root).resolve()
        self.data = Path(data_dir).resolve()
        self.profiles = self.data / 'profiles'
        self.profiles.mkdir(parents=True, exist_ok=True)
        self.session = None
        self.process = None
        self.log = None
        self.poisoned = False

    def request(self, command, timeout=12):
        if not self.process or self.process.poll() is not None:
            raise ValueError('client is not running; call mph_start')
        if self.poisoned:
            raise ValueError('previous operation timed out; stop and restart the session')
        request_id = uuid.uuid4().hex
        temporary = self.session / 'request.tmp'
        temporary.write_text(request_id + '\n' + command, encoding='utf-8', newline='\n')
        temporary.replace(self.session / 'request.txt')
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise ValueError('native client exited; inspect the MCP session client.log')
            try:
                response = json.loads((self.session / 'response.json').read_text(encoding='utf-8'))
                if response.get('id') == request_id:
                    if not response.get('ok'):
                        raise ValueError(response.get('error', 'native command failed'))
                    return response
            except (FileNotFoundError, json.JSONDecodeError, PermissionError):
                pass
            time.sleep(.025)
        self.cancel()
        self.poisoned = True
        raise ValueError('operation timed out; outcome unknown; cancellation requested; restart required')

    def cancel(self):
        if self.session and self.process and self.process.poll() is None:
            (self.session / 'cancel').touch()

    def start(self, args):
        if self.process and self.process.poll() is None:
            return self.request('state')
        serial = args.get('serial')
        if serial and not re.fullmatch(r'[A-Za-z0-9_.:\-]{1,128}', serial):
            raise ValueError('invalid device serial')
        name = args.get('profile', 'default')
        profile = self.profiles / (name + '.mph')
        if not profile.exists():
            template = self.root / 'assets/default-fps.mph'
            if not template.exists():
                template = self.root / 'default-fps.mph'
            with profile.open('xb') as output:
                output.write(template.read_bytes())
        executable = self.root / 'dist/MobilePlayHelper.exe'
        if not executable.is_file():
            raise ValueError('build the native client first, or use the complete portable ZIP')
        self.session = Path(tempfile.mkdtemp(prefix='session-', dir=self.data))
        self.log = (self.session / 'client.log').open('wb')
        env = dict(os.environ, MPH_MCP_DIR=str(self.session), MPH_PROFILE=str(profile),
                   MPH_DIAGNOSTICS_OFF='1', MPH_BENCHMARK='1')
        # Explicit modest settings avoid inheriting local stress-test settings.
        command = [str(executable), '--max-size=1280', '--max-fps=30', '--video-bit-rate=4M',
                   '--window-width=1000', '--window-title=MobilePlayHelper MCP', '--no-audio',
                   '--no-clipboard-autosync']
        if serial:
            command += ['--serial', serial]
        self.poisoned = False
        self.process = subprocess.Popen(command, cwd=self.root, env=env, stdin=subprocess.DEVNULL,
                                        stdout=self.log, stderr=self.log,
                                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        return self.request('state', timeout=30)

    def stop(self):
        if not self.process:
            return {'stopped': True}
        if self.process.poll() is None:
            self.cancel()
            time.sleep(.1)
            self.poisoned = False
            try:
                self.request('stop', timeout=3)
            except ValueError:
                pass
            try:
                self.process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=4)
        if self.log:
            self.log.close()
        self.process = None
        return {'stopped': True}

    def call(self, name, args):
        spec = next((tool[2] for tool in TOOLS if tool[0] == name), None)
        if spec is None:
            raise ValueError('unknown tool')
        validate(spec, args)
        if name == 'mph_start':
            return self.start(args)
        if name == 'mph_stop':
            return self.stop()
        if name == 'mph_cancel':
            self.cancel()
            return {'cancellation_requested': True}
        if name == 'mph_state':
            return self.request('state')
        if name == 'mph_screenshot':
            time.sleep(args.get('settle_ms', 250) / 1000)
            command = 'screenshot'
            if 'mask' in args:
                m = args['mask']
                if m['x']+m['width'] > 1 or m['y']+m['height'] > 1:
                    raise ValueError('mask extends outside the phone image')
                command += f" {m['x']} {m['y']} {m['width']} {m['height']}"
            result = self.request(command)
            result['redaction'] = args.get('mask')
            image = (self.session / 'screenshot.png').read_bytes()
            result['image_coordinate_note'] = 'Phone occupies the left region; toolbar on the right. Touch coordinates use the phone frame only.'
            return {'content': [{'type': 'text', 'text': json.dumps(result)},
                                {'type': 'image', 'mimeType': 'image/png', 'data': base64.b64encode(image).decode('ascii')}]}
        if name == 'mph_mapping':
            return self.request('mapping ' + str(int(args['enabled'])))
        if name == 'mph_editor':
            return self.request('edit ' + args['action'])
        if name == 'mph_profile':
            status = self.request('state')
            if status['editing']:
                raise ValueError('save or discard edits before switching profile')
            path = self.profiles / (args['name'] + '.mph')
            if args['create']:
                if path.exists():
                    raise ValueError('profile already exists; create never overwrites')
                joy, look = status['joystick'], status['look']
                lines = ['toggle 58', 'joystick ' + ' '.join(map(str, joy)), 'look ' + ' '.join(map(str, look))]
                for binding in status['bindings']:
                    key = binding['key']
                    token = {513: 'Mouse1', 515: 'Mouse2', 514: 'Mouse3', 516: 'Mouse4', 517: 'Mouse5'}.get(key, 'Key' + str(key))
                    lines.append(f"bind {token} {binding['x']} {binding['y']}")
                with path.open('x', encoding='utf-8') as output:
                    output.write('\n'.join(lines) + '\n')
            elif not path.is_file():
                raise ValueError('profile does not exist')
            return self.request('profile ' + str(path))
        if name == 'mph_binding':
            if args.get('delete', False):
                return self.request(f"delete {args['key']}")
            if not {'x', 'y'} <= set(args):
                raise ValueError('x and y required unless deleting')
            return self.request(f"bind {args['key']} {args['x']} {args['y']}")
        if name == 'mph_zone':
            return self.request(f"zone {int(args['zone']=='look')} {args['x']} {args['y']} {args['radius']} {args.get('sensitivity', .001)}")
        if name in ('mph_tap', 'mph_swipe'):
            x, y = args['x'], args['y']
            x2, y2 = (args['to_x'], args['to_y']) if name == 'mph_swipe' else (x, y)
            duration = args.get('duration_ms', 350 if name == 'mph_swipe' else 100)
            return self.request(f'touch {x} {y} {x2} {y2} {duration}')
        raise ValueError('unsupported tool')


def serve(bridge):
    output_lock = threading.Lock()
    initialized = False
    executor = ThreadPoolExecutor(max_workers=1)
    active_id = [None]

    def send(message):
        with output_lock:
            sys.stdout.write(json.dumps(message, ensure_ascii=True, allow_nan=False) + '\n')
            sys.stdout.flush()

    def error(request_id, code, text):
        send({'jsonrpc': '2.0', 'id': request_id, 'error': {'code': code, 'message': text}})

    def tool_call(request_id, params):
        try:
            result = bridge.call(params.get('name'), params.get('arguments', {}))
            if not isinstance(result, dict) or 'content' not in result:
                result = {'content': [{'type': 'text', 'text': json.dumps(result)}]}
        except Exception as exc:
            result = {'isError': True, 'content': [{'type': 'text', 'text': str(exc)}]}
        send({'jsonrpc': '2.0', 'id': request_id, 'result': result})

    def serial_call(request_id, params):
        try:
            tool_call(request_id, params)
        finally:
            active_id[0] = None

    try:
        while True:
            line = sys.stdin.buffer.readline(65537)
            if not line:
                break
            if len(line) > 65536:
                error(None, -32700, 'message exceeds 64 KiB')
                break
            try:
                message = json.loads(line)
            except (ValueError, UnicodeError):
                error(None, -32700, 'invalid JSON')
                continue
            if not isinstance(message, dict) or message.get('jsonrpc') != '2.0' or not isinstance(message.get('method'), str):
                error(None, -32600, 'invalid request')
                continue
            method, request_id = message['method'], message.get('id')
            params = message.get('params', {})
            if not isinstance(params, dict):
                error(request_id, -32602, 'params must be an object')
                continue
            if method == 'notifications/cancelled':
                if params.get('requestId') == active_id[0] and active_id[0] is not None:
                    bridge.cancel()
                continue
            if 'id' not in message:
                continue
            if method == 'initialize':
                initialized = True
                version = params.get('protocolVersion')
                if version not in ('2024-11-05', '2025-03-26', '2025-06-18'):
                    version = '2025-06-18'
                send({'jsonrpc': '2.0', 'id': request_id, 'result': {
                    'protocolVersion': version, 'capabilities': {'tools': {}},
                    'serverInfo': {'name': 'mobile-play-helper', 'version': '0.3.0'},
                    'instructions': 'Use screenshots to verify game state. Touch success means queue acceptance, not game acknowledgement. Ordinary mode is required for remote gestures.'}})
            elif method == 'ping':
                send({'jsonrpc': '2.0', 'id': request_id, 'result': {}})
            elif not initialized:
                error(request_id, -32000, 'initialize first')
            elif method == 'tools/list':
                send({'jsonrpc': '2.0', 'id': request_id, 'result': {'tools': [
                    {'name': name, 'description': description, 'inputSchema': spec,
                     'annotations': {'readOnlyHint': name in ('mph_state', 'mph_screenshot'),
                                     'destructiveHint': name in ('mph_tap', 'mph_swipe'),
                                     'openWorldHint': name in ('mph_tap', 'mph_swipe')}}
                    for name, description, spec in TOOLS]}})
            elif method == 'tools/call':
                if params.get('name') == 'mph_cancel':
                    tool_call(request_id, params)
                elif active_id[0] is not None:
                    error(request_id, -32000, 'another operation is active; wait or call mph_cancel')
                else:
                    active_id[0] = request_id
                    executor.submit(serial_call, request_id, params)
            else:
                error(request_id, -32601, 'method not found')
    finally:
        bridge.cancel()
        executor.shutdown(wait=True, cancel_futures=True)
        bridge.stop()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--data-dir', type=Path)
    options = parser.parse_args()
    data_dir = options.data_dir or Path(os.getenv('LOCALAPPDATA', str(Path.home()))) / 'MobilePlayHelper/MCP'
    serve(Bridge(options.root, data_dir))
