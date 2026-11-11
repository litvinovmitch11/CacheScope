#!/usr/bin/env python3
"""Exercise the runner through the agreed HTTP contract and real cache levels."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import copy
import json
import math
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--repo', type=Path, required=True)
    return parser.parse_args()


ARGS = None


class RunnerHTTP(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='cachescope-http-')
        cls.root = Path(cls.directory.name)
        cls.process = subprocess.Popen(
            [str(ARGS.server), '--port', '0', '--allow-origin', 'http://localhost:5173'], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True,
        )
        selector = selectors.DefaultSelector()
        selector.register(cls.process.stdout, selectors.EVENT_READ)
        try:
            if not selector.select(timeout=15):
                raise AssertionError('HTTP server did not start')
            cls.url = cls.process.stdout.readline().strip()
            if not cls.url.startswith('http://127.0.0.1:'):
                raise AssertionError('Server startup failed: ' + cls.url)
        except BaseException:
            cls.process.kill()
            cls.process.wait()
            cls.directory.cleanup()
            raise
        finally:
            selector.close()

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        try:
            cls.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            cls.process.kill()
            cls.process.wait()
        cls.process.stdout.close()
        cls.directory.cleanup()

    def request(self, method, path, body=None, query=None):
        url = self.url + path
        if query:
            url += '?' + urllib.parse.urlencode(query)
        request = urllib.request.Request(url, method=method,
            data=None if body is None else json.dumps(body).encode(),
            headers={'Content-Type': 'application/json'})
        try:
            response = urllib.request.urlopen(request, timeout=30)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = response.read()
            return response.status, dict(response.headers), json.loads(data) if data else None

    def ready(self, sid):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            status, _, session = self.request('GET', '/api/v1/sessions/' + sid)
            self.assertEqual(status, 200)
            if session['status'] != 'preparing':
                return session
            time.sleep(0.01)
        self.fail('preparation did not finish')

    def assert_dto(self, actual, expected, path='response'):
        if isinstance(expected, float):
            self.assertTrue(isinstance(actual, (int, float)) and math.isclose(actual, expected, abs_tol=1e-10), path)
        elif isinstance(expected, dict):
            self.assertEqual(set(actual), set(expected), path)
            for key in expected:
                self.assert_dto(actual[key], expected[key], path + '.' + key)
        elif isinstance(expected, list):
            self.assertEqual(len(actual), len(expected), path)
            for index, value in enumerate(expected):
                self.assert_dto(actual[index], value, path + f'[{index}]')
        else:
            self.assertEqual(actual, expected, path)

    def setUp(self):
        _, _, body = self.request('GET', '/api/v1/sessions')
        for session in body['sessions']:
            status, _, _ = self.request('DELETE', '/api/v1/sessions/' + session['session_id'],
                query={'expected_revision': session['revision']})
            self.assertEqual(status, 204)

    def trace(self, events=None, accesses=None, name='trace.json'):
        value = json.loads((ARGS.repo / 'schemas/fixtures/trace-v1-complete.json').read_text())
        if events is not None:
            value['events'] = events
        if accesses is not None:
            value['accesses'] = accesses
        value['total_events'] = value['recorded_events'] = len(value['events'])
        path = self.root / name
        path.write_text(json.dumps(value))
        return path, value

    def config(self, trace, policy='LRU', seed=None, ways=2, sets=2):
        level = dict(name='L1', size_bytes=64 * ways * sets, line_size_bytes=64,
                     associativity=ways, replacement_policy=policy)
        if seed is not None:
            level['random_seed'] = seed
        return dict(trace_path=str(trace), levels=[level])

    def create(self, trace, **options):
        status, headers, body = self.request('POST', '/api/v1/sessions', self.config(trace, **options))
        self.assertEqual(status, 202, body)
        self.assertEqual(body['status'], 'preparing')
        self.assertEqual(body['cursor'], '0')
        self.assertEqual(body['revision'], '0')
        sid = body['session_id']
        self.assertEqual(headers['Location'], '/api/v1/sessions/' + sid)
        return sid, self.ready(sid)

    def endpoint(self, sid, resource=''):
        return '/api/v1/sessions/' + sid + ('/' + resource if resource else '')

    def test_empty_reset_and_pages(self):
        trace, _ = self.trace(events=[])
        sid, session = self.create(trace)
        self.assertEqual(session['status'], 'finished')
        self.assertEqual(session['total_events'], '0')
        status, _, first = self.request('GET', self.endpoint(sid, 'state'),
            query=dict(expected_revision='1', level=1, set_limit=1))
        self.assertEqual(status, 200)
        self.assertEqual(first['cache']['next_set_offset'], 1)
        status, _, second = self.request('GET', self.endpoint(sid, 'state'),
            query=dict(expected_revision='1', level=1, set_offset=1, set_limit=1))
        self.assertEqual(status, 200)
        self.assertIsNone(second['cache']['next_set_offset'])
        self.assertEqual(second['cache']['sets'][0]['set'], 1)
        for page in (first, second):
            self.assertEqual(page['summary']['requests']['events'], '0')
            self.assertIsNone(page['last_event'])
            for cell in page['cache']['sets'][0]['ways']:
                self.assertEqual(cell, dict(way=cell['way'], valid=False, line_address=None, dirty=False))
        _, _, empty = self.request('GET', self.endpoint(sid, 'state'),
            query=dict(expected_revision='1', level=1, set_offset=2, set_limit=1))
        self.assertEqual(empty['cache']['sets'], [])
        _, _, eof = self.request('POST', self.endpoint(sid, 'step'), dict(expected_revision='1', count=999))
        self.assertEqual(eof['events'], [])
        self.assertEqual(eof['revision'], '1')
        _, _, reset = self.request('POST', self.endpoint(sid, 'reset'), dict(expected_revision='1'))
        self.assertEqual(reset['status'], 'finished')
        self.assertEqual(reset['revision'], '2')
        status, _, stale = self.request('GET', self.endpoint(sid, 'state'),
            query=dict(expected_revision='1', level=1, set_limit=1))
        self.assertEqual((status, stale['error']['code']), (409, 'revision_mismatch'))

    def test_random_repeats_after_reset(self):
        _, value = self.trace()
        events = [dict(value['events'][0], sequence=i, byte_offset=offset, size_bytes=4)
                  for i, offset in enumerate([0, 64, 128, 192, 0, 256, 64, 320, 128])]
        trace, _ = self.trace(events=events)
        sid, session = self.create(trace, policy='Random', seed='18446744073709551615', sets=1)
        self.assertEqual(session['levels'][0]['random_seed'], '18446744073709551615')
        _, _, first = self.request('POST', self.endpoint(sid, 'step'), dict(expected_revision='1', count=99))
        self.assertEqual(first['status'], 'finished')
        _, _, first_state = self.request('GET', self.endpoint(sid, 'state'),
            query=dict(expected_revision='2', level=1, set_limit=1))
        self.request('POST', self.endpoint(sid, 'reset'), dict(expected_revision='2'))
        _, _, second = self.request('POST', self.endpoint(sid, 'step'), dict(expected_revision='3', count=99))
        _, _, second_state = self.request('GET', self.endpoint(sid, 'state'),
            query=dict(expected_revision='4', level=1, set_limit=1))
        self.assert_dto(dict(first, revision='4'), second)
        self.assert_dto(dict(first_state, revision='4'), second_state)

    def test_source_map_large_ids_and_negative_offset(self):
        value = json.loads((ARGS.repo / 'schemas/fixtures/source-map-v1/trace.json').read_text())
        wide = 9007199254740993
        value['accesses'][0].update(access_id=wide, memref_id=wide, origin_ids=[wide])
        value['source_map']['origins'][0]['origin_id'] = wide
        value['events'][0].update(access_id=wide, memref_id=wide, byte_offset=-4, size_bytes=4)
        # Numeric order must put 2 before 10, even though its string sorts later.
        value['accesses'] += [dict(value['accesses'][0], access_id=i) for i in [10, 2]]
        value['accesses'][-1]['memref_id'] = 0
        value['events'].insert(0, dict(value['events'][0], sequence=0, access_id=2, memref_id=0, byte_offset=wide))
        value['events'][1]['sequence'] = 1
        value['total_events'] = value['recorded_events'] = 2
        trace = self.root / 'mapped.json'
        trace.write_text(json.dumps(value))
        sid, session = self.create(trace)
        self.assertEqual(session['status'], 'ready', session)
        _, _, metadata = self.request('GET', self.endpoint(sid, 'metadata'))
        self.assertEqual([a['access_id'] for a in metadata['accesses']], ['2', '10', str(wide)])
        mapping = metadata['source']['source_map']
        self.assertEqual(mapping['source_schema'], 'cachescope.source-map.v1')
        self.assertNotIn('schema', mapping)
        self.assertEqual(mapping['origins'][0]['origin_id'], str(wide))
        self.assertEqual(metadata['buffers'][1]['min_offset'], '-4')
        self.assertEqual(metadata['source']['producer'], value['producer'])
        _, _, step = self.request('POST', self.endpoint(sid, 'step'), dict(expected_revision='1', count=2))
        self.assertEqual(step['events'][1]['trace_event']['byte_offset'], '-4')
        self.assertEqual(step['events'][1]['trace_event']['access_id'], str(wide))
        self.assertEqual(step['events'][1]['address'], '0x2000001000003c')
        _, _, rows = self.request('GET', self.endpoint(sid, 'instructions'),
            query=dict(expected_revision='2', limit=2))
        self.assertEqual(rows['next_offset'], '2')
        self.assertEqual([row['access_id'] for row in rows['instructions']], ['2', '10'])
        self.assertEqual([row['events'] for row in rows['instructions']], ['1', '0'])

    def test_preparation_can_be_cancelled_and_replaced(self):
        small, original = self.trace()
        large = self.root / 'large.json'
        total = 50000
        original.pop('events')
        original['total_events'] = original['recorded_events'] = total
        with large.open('w') as out:
            out.write(json.dumps(original)[:-1] + ',"events":[')
            for sequence in range(total):
                if sequence:
                    out.write(',')
                out.write(json.dumps(dict(sequence=sequence, access_id=0, memref_id=0,
                    byte_offset=0, size_bytes=4, kind='read', address_kind='memref_offset')))
            out.write(']}')
        status, _, preparing = self.request('POST', '/api/v1/sessions', self.config(large))
        self.assertEqual(status, 202)
        sid = preparing['session_id']
        status, _, _ = self.request('DELETE', self.endpoint(sid), query=dict(expected_revision='0'))
        self.assertEqual(status, 204)
        next_id, session = self.create(small)
        self.assertNotEqual(next_id, sid)
        self.assertEqual(session['status'], 'ready', session)
        status, _, _ = self.request('GET', self.endpoint(sid))
        self.assertEqual(status, 404)

    def test_json_transport_and_cors(self):
        trace, _ = self.trace()
        sid, _ = self.create(trace)
        for raw in [b'{', b'null', b'{"expected_revision":"1","expected_revision":"1"}']:
            request = urllib.request.Request(self.url + self.endpoint(sid, 'step'),
                method='POST', data=raw, headers={'Content-Type': 'application/json'})
            with self.assertRaises(urllib.error.HTTPError) as caught:
                urllib.request.urlopen(request, timeout=10)
            with caught.exception as response:
                self.assertEqual(response.code, 400)
                self.assertEqual(json.loads(response.read())['error']['code'], 'invalid_request')
        request = urllib.request.Request(self.url + self.endpoint(sid, 'step'), method='OPTIONS',
            headers={'Origin': 'http://localhost:5173', 'Access-Control-Request-Method': 'POST'})
        with urllib.request.urlopen(request, timeout=10) as response:
            self.assertEqual(response.status, 204)
            self.assertEqual(response.headers['Access-Control-Allow-Origin'], 'http://localhost:5173')
        request = urllib.request.Request(self.url + '/api/v1/sessions', headers={'Host': 'foreign.example'})
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(request, timeout=10)
        with caught.exception as response:
            self.assertEqual(response.code, 400)
            self.assertEqual(json.loads(response.read())['error']['code'], 'invalid_request')
        status, _, result = self.request('GET', self.endpoint(sid, 'state') +
            '?expected_revision=1&level=1&level=2&set_limit=1')
        self.assertEqual((status, result['error']['code']), (400, 'invalid_request'))
        _, _, session = self.request('GET', self.endpoint(sid))
        self.assertEqual((session['cursor'], session['revision']), ('0', '1'))

    def test_invalid_requests_do_not_mutate(self):
        trace, _ = self.trace()
        config = self.config(trace)
        for path in ['/', 'trace.json', str(trace) + '\0ignored']:
            status, _, result = self.request('POST', '/api/v1/sessions', dict(config, trace_path=path))
            self.assertEqual((status, result['error']['code']), (400, 'invalid_request'))
        for key, value in [('line_size_bytes', 63), ('size_bytes', 0), ('replacement_policy', 'bad')]:
            invalid = copy.deepcopy(config)
            invalid['levels'][0][key] = value
            status, _, _ = self.request('POST', '/api/v1/sessions', invalid)
            self.assertEqual(status, 400)
            _, _, listing = self.request('GET', '/api/v1/sessions')
            self.assertEqual(listing, dict(sessions=[]))
        sid, _ = self.create(trace)
        for count in [0, -1, '1', 1.5, True, 1001, 9007199254740992]:
            status, _, result = self.request('POST', self.endpoint(sid, 'step'), dict(expected_revision='1', count=count))
            self.assertEqual((status, result['error']['code']), (400, 'invalid_count'))
        for revision in [1, '01', '-1', '+1', '18446744073709551616', True]:
            status, _, _ = self.request('POST', self.endpoint(sid, 'step'), dict(expected_revision=revision))
            self.assertEqual(status, 400)
        status, _, occupied = self.request('POST', '/api/v1/sessions', config)
        self.assertEqual((status, occupied['error']['code']), (409, 'session_exists'))
        for page in [dict(level=0, set_limit=1), dict(level=2, set_limit=1),
                     dict(level=1, set_limit=0), dict(level=1, set_limit=65),
                     dict(level=1, set_limit=1, set_offset=3)]:
            status, _, result = self.request('GET', self.endpoint(sid, 'state'), query=dict(expected_revision='1', **page))
            self.assertEqual((status, result['error']['code']), (400, 'invalid_page'))
        _, _, unchanged = self.request('GET', self.endpoint(sid))
        self.assertEqual((unchanged['cursor'], unchanged['revision']), ('0', '1'))

    def test_preflight_rejects_bad_traces(self):
        _, original = self.trace()
        invalid = []
        for key, value in [('valid', False), ('complete', False), ('truncated', True), ('errors', ['bad'])]:
            invalid.append(dict(original, **{key: value}))
        for key, value in [('sequence', 1), ('kind', 'read_write'), ('address_kind', 'process_address'),
                           ('size_bytes', 0), ('byte_offset', 2**63)]:
            trace = copy.deepcopy(original)
            trace['events'][0][key] = value
            invalid.append(trace)
        for index, trace in enumerate(invalid):
            path = self.root / f'invalid-{index}.json'
            path.write_text(json.dumps(trace))
            sid, session = self.create(path)
            self.assertEqual(session['status'], 'failed', session)
            self.assertEqual(session['cursor'], '0')
            self.assertEqual(session['error']['recovery'], 'recreate')
            self.assertEqual(session['error']['code'], 'invalid_trace')
            status, _, _ = self.request('POST', self.endpoint(sid, 'reset'), dict(expected_revision='1'))
            self.assertEqual(status, 409)
            self.request('DELETE', self.endpoint(sid), query=dict(expected_revision='1'))
        sid, session = self.create(self.root / 'missing.json')
        self.assertEqual(session['error']['code'], 'trace_unreadable')

    def test_preflight_rejects_non_regular_files(self):
        pipe = self.root / 'trace.pipe'
        os.mkfifo(pipe)
        sid, session = self.create(pipe)
        self.assertEqual(session['status'], 'failed')
        self.assertEqual(session['error']['code'], 'trace_unreadable')
        self.request('DELETE', self.endpoint(sid), query=dict(expected_revision=session['revision']))
        path, _ = self.trace()
        _, session = self.create(path)
        self.assertEqual(session['status'], 'ready')

    def test_trace_change_requires_recreate(self):
        trace, _ = self.trace()
        sid, _ = self.create(trace)
        trace.write_text(trace.read_text() + ' ')
        status, _, error = self.request('POST', self.endpoint(sid, 'step'), dict(expected_revision='1'))
        self.assertEqual((status, error['error']['code']), (500, 'trace_changed'))
        self.assertEqual(error['error']['recovery'], 'recreate')
        _, _, session = self.request('GET', self.endpoint(sid))
        self.assertEqual((session['status'], session['cursor'], session['revision']), ('failed', '0', '2'))
        for method, resource, body, query in [
            ('POST', 'reset', dict(expected_revision='2'), None),
            ('POST', 'step', dict(expected_revision='2'), None),
            ('GET', 'state', None, dict(expected_revision='2', level=1, set_limit=1)),
            ('GET', 'instructions', None, dict(expected_revision='2', limit=1)),
        ]:
            status, _, error = self.request(method, self.endpoint(sid, resource), body, query)
            self.assertEqual((status, error['error']['code']), (409, 'session_failed'))

    def test_full_width_ids_and_minimum_signed_offset(self):
        _, source = self.trace()
        maximum = 2**64 - 1
        minimum = -2**63
        events = [dict(source['events'][0], access_id=maximum, memref_id=maximum, byte_offset=minimum)]
        accesses = [dict(source['accesses'][0], access_id=maximum, memref_id=maximum)]
        trace, _ = self.trace(events=events, accesses=accesses)
        sid, session = self.create(trace)
        self.assertEqual(session['status'], 'ready', session)
        _, _, metadata = self.request('GET', self.endpoint(sid, 'metadata'))
        self.assertEqual(metadata['accesses'][0]['access_id'], str(maximum))
        self.assertEqual(metadata['buffers'][0]['min_offset'], str(minimum))
        self.assertEqual(metadata['buffers'][0]['base_address'], '0x8000000010000000')
        _, _, result = self.request('POST', self.endpoint(sid, 'step'), dict(expected_revision='1'))
        event = result['events'][0]
        self.assertEqual(event['trace_event']['access_id'], str(maximum))
        self.assertEqual(event['trace_event']['byte_offset'], str(minimum))
        self.assertEqual(event['address'], '0x10000000')

    def test_busy_rejects_mutations_and_snapshots(self):
        _, original = self.trace()
        events = [dict(original['events'][0], sequence=i, byte_offset=0, size_bytes=16384) for i in range(500)]
        trace, _ = self.trace(events=events)
        sid, _ = self.create(trace, sets=1)
        with ThreadPoolExecutor(max_workers=1) as pool:
            work = pool.submit(self.request, 'POST', self.endpoint(sid, 'step'), dict(expected_revision='1', count=500))
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                _, _, session = self.request('GET', self.endpoint(sid))
                if session['busy']:
                    break
                self.assertFalse(work.done(), 'batch finished before busy could be observed')
            else:
                self.fail('busy was not published')
            self.assertEqual((session['cursor'], session['revision']), ('0', '1'))
            for method, resource, body, query in [
                ('GET', 'state', None, dict(expected_revision='1', level=1, set_limit=1)),
                ('GET', 'instructions', None, dict(expected_revision='1', limit=1)),
                ('POST', 'reset', dict(expected_revision='1'), None),
                ('DELETE', '', None, dict(expected_revision='1')),
            ]:
                status, _, result = self.request(method, self.endpoint(sid, resource), body, query)
                self.assertEqual((status, result['error']['code']), (409, 'session_busy'))
            status, _, result = work.result(timeout=30)
            self.assertEqual(status, 200)
            self.assertEqual((result['cursor'], result['revision']), ('500', '2'))
        status, _, retry = self.request('POST', self.endpoint(sid, 'step'), dict(expected_revision='1', count=500))
        self.assertEqual((status, retry['error']['code']), (409, 'revision_mismatch'))

    def test_contract_fifo_conversation(self):
        fixture = json.loads((ARGS.repo / 'schemas/fixtures/runner-api-v1/replay.json').read_text())
        cases = fixture['exchanges']
        metadata = next(e['response']['body'] for e in cases if e['name'] == 'metadata-without-source-map')
        source = copy.deepcopy(metadata['source'])
        source['schema'] = source.pop('source_schema')
        source['total_events'] = int(source['total_events'])
        source['recorded_events'] = int(source['recorded_events'])
        source['accesses'] = copy.deepcopy(metadata['accesses'])
        for access in source['accesses']:
            for key in ['access_id', 'memref_id']:
                access[key] = int(access[key])
        source['events'] = []
        for case in cases:
            if case['request']['method'] == 'POST' and case['request']['path'].endswith('/step') and case['name'] != 'eof-no-op':
                for outcome in case['response']['body']['events']:
                    event = copy.deepcopy(outcome['trace_event'])
                    for key in ['sequence', 'access_id', 'memref_id', 'byte_offset', 'size_bytes']:
                        event[key] = int(event[key])
                    source['events'].append(event)
        trace = self.root / 'fifo.json'
        trace.write_text(json.dumps(source))
        sid = 'demo-fifo'

        def normalized(value):
            if isinstance(value, dict):
                return {key: normalized(val) for key, val in value.items()}
            if isinstance(value, list):
                return [normalized(val) for val in value]
            if value == 'demo-fifo':
                return sid
            if value == '/workspace/traces/fifo.json':
                return str(trace)
            return value

        for case in cases:
            with self.subTest(case=case['name']):
                request = normalized(case['request'])
                request['path'] = request['path'].replace('demo-fifo', sid)
                if case['name'] == 'ready':
                    self.ready(sid)
                status, headers, body = self.request(request['method'], request['path'],
                    body=request.get('body'), query=request.get('query'))
                self.assertEqual(status, case['response']['status'], body)
                if case['name'] == 'create':
                    sid = body['session_id']
                    self.assertEqual(headers['Location'], '/api/v1/sessions/' + sid)
                expected = normalized(case['response'].get('body'))
                if status >= 400:
                    self.assertEqual(body['error']['code'], expected['error']['code'])
                    for key in ['session_id', 'revision']:
                        if key in expected:
                            self.assertEqual(body[key], expected[key])
                else:
                    self.assert_dto(body, expected)


if __name__ == '__main__':
    ARGS = arguments()
    unittest.main(argv=['http_tests.py'], verbosity=2)
