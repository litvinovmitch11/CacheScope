#!/usr/bin/env python3
"""Real upload -> compiler -> runner integration, through public HTTP only."""
import argparse
import io
import json
from pathlib import Path
import struct
import time
import urllib.error
import urllib.request
import zipfile


def inputs(shape=(4, 4), dtype='<f4', start=0):
    header = repr(dict(descr=dtype, fortran_order=False, shape=shape)).encode()
    header += b' ' * ((64 - (10 + len(header) + 1) % 64) % 64) + b'\n'
    count = 1
    for dim in shape:
        count *= dim
    array = b'\x93NUMPY\x01\x00' + struct.pack('<H', len(header)) + header
    array += struct.pack('<' + ('f' if dtype == '<f4' else 'd') * count, *range(start, start + count))
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w') as archive:
        archive.writestr('source.npy', array)
    return out.getvalue()


class Client:
    def __init__(self, url):
        self.url = url.rstrip('/') + '/api/v1/experiments'

    def request(self, method, path='', body=None, content_type='application/json', headers=None):
        data = body if isinstance(body, bytes) else (json.dumps(body).encode() if body is not None else None)
        req = urllib.request.Request(self.url + path, method=method, data=data,
                                     headers={'Content-Type': content_type, **(headers or {})})
        try:
            response = urllib.request.urlopen(req, timeout=60)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, json.load(response)

    def upload(self, mlir, npz=None):
        boundary = 'cachescope-test-boundary'
        parts = []
        for name, filename, data in [('mlir', 'example.mlir', mlir), ('inputs', 'inputs.npz', inputs() if npz is None else npz)]:
            parts += [f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"; filename="{filename}"\r\nContent-Type: application/octet-stream\r\n\r\n'.encode(), data, b'\r\n']
        parts += [f'--{boundary}--\r\n'.encode()]
        status, result = self.request('POST', body=b''.join(parts), content_type='multipart/form-data; boundary=' + boundary)
        assert status == 202, (status, result)
        return result['experiment_id']

    def wait(self, eid, expected):
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            status, job = self.request('GET', '/' + eid)
            assert status == 200, (status, job)
            if job['status'] not in ('preparing', 'running', 'cancelling'):
                assert job['status'] == expected, job
                return job
            time.sleep(0.05)
        raise AssertionError('experiment did not finish')


def spec():
    return dict(function='copy_kernel', bindings=[dict(source='npz', key='source'), dict(source='zeros')],
                levels=[dict(name='L1', size_bytes=128, line_size_bytes=64, associativity=2, replacement_policy='LRU')],
                timeout_seconds=None, trace_limit=None)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', required=True)
    parser.add_argument('--repo', type=Path, required=True)
    args = parser.parse_args()
    client = Client(args.url)
    assert client.request('POST', body=b'', headers={'Origin': 'https://unrelated.example'})[0] == 403
    eid = client.upload((args.repo / 'examples/ui-upload/copy.mlir').read_bytes())
    job = client.wait(eid, 'ready')
    assert [f['name'] for f in job['metadata']['functions'] if f['supported']] == ['copy_kernel']
    assert job['metadata']['arrays'][0]['shape'] == [4, 4]
    status, _ = client.request('POST', '/' + eid + '/run', spec())
    assert status == 202
    job = client.wait(eid, 'completed')
    status, report = client.request('GET', '/' + eid + '/report')
    assert status == 200, report
    assert report['statistics']['processed']['trace_events'] == 32
    assert report['statistics']['totals']['hits'] == 30
    assert report['statistics']['totals']['misses'] == 2
    assert report['trace']['complete'] is True
    status, current = client.request('GET')
    assert current['experiments'][0]['experiment_id'] == eid
    print('upload/compiler/runner/report integration OK: 32 events, 30 hits, 2 misses')

    # Bad data is rejected before running, without consuming the prepared job.
    for payload in [inputs(shape=(2, 2)), inputs(dtype='<f8')]:
        eid = client.upload((args.repo / 'examples/ui-upload/copy.mlir').read_bytes(), payload)
        client.wait(eid, 'ready')
        status, error = client.request('POST', '/' + eid + '/run', spec())
        assert status == 400, (status, error)
        client.wait(eid, 'ready')
    for source, payload in [(b'not MLIR', inputs()), ((args.repo / 'examples/ui-upload/copy.mlir').read_bytes(), b'not NPZ')]:
        eid = client.upload(source, payload)
        job = client.wait(eid, 'failed')
        assert job['error']['code'] == 'invalid_input', job
        assert client.request('GET', '/' + eid + '/report')[0] == 409

    source = (args.repo / 'examples/ui-upload/copy.mlir').read_bytes()
    eid = client.upload(source)
    client.wait(eid, 'ready')
    bad_geometry = spec()
    bad_geometry['levels'][0]['size_bytes'] = 127
    assert client.request('POST', '/' + eid + '/run', bad_geometry)[0] == 400
    bad_plru = spec()
    bad_plru['levels'][0]['replacement_policy'] = 'PLRU4'
    assert client.request('POST', '/' + eid + '/run', bad_plru)[0] == 400
    duplicate_json = json.dumps(spec()).replace('"function": "copy_kernel"', '"function": "other", "function": "copy_kernel"').encode()
    assert client.request('POST', '/' + eid + '/run', duplicate_json)[0] == 400
    client.wait(eid, 'ready')
    limited = spec()
    limited['trace_limit'] = '1'
    assert client.request('POST', '/' + eid + '/run', limited)[0] == 202
    job = client.wait(eid, 'failed')
    assert job['error']['code'] == 'trace_limit_exceeded', job
    assert client.request('GET', '/' + eid + '/report')[0] == 409

    eid = client.upload(source)
    client.wait(eid, 'ready')
    timed = spec()
    timed['timeout_seconds'] = 0.001
    assert client.request('POST', '/' + eid + '/run', timed)[0] == 202
    assert client.wait(eid, 'failed')['error']['code'] == 'timeout'

    # Cancel a real infinite native loop after it starts executing; no growth
    # of the trace buffer is needed to prove process-group cancellation.
    infinite = source.replace(b'    return\n', b'''    %forever = arith.constant true
    scf.while : () -> () {
      scf.condition(%forever)
    } do {
      scf.yield
    }
    return
''', 1)
    eid = client.upload(infinite)
    client.wait(eid, 'ready')
    assert client.request('POST', '/' + eid + '/run', spec())[0] == 202
    assert client.request('POST', '/' + eid + '/run', spec())[0] == 409
    deadline = time.monotonic() + 60
    while True:
        _, job = client.request('GET', '/' + eid)
        assert job['status'] == 'running', job
        if job['stage'] == 'execution':
            break
        assert time.monotonic() < deadline, job
        time.sleep(0.05)
    status, _ = client.request('POST', '/' + eid + '/cancel')
    assert status == 202
    client.wait(eid, 'cancelled')
    assert client.request('GET', '/' + eid + '/report')[0] == 409

    eid = client.upload(source)
    client.wait(eid, 'ready')
    valid = spec()
    valid['levels'][0]['name'] = 'L1:custom'
    assert client.request('POST', '/' + eid + '/run', valid)[0] == 202
    client.wait(eid, 'completed')
    assert client.request('GET', '/' + eid + '/report')[1]['statistics']['levels'][0]['name'] == 'L1:custom'
    print('validation, optional limits, native cancellation and recovery OK')

    # The uploaded values must actually reach the native function: only a
    # positive first element takes the store branch.
    branch = b'''module {
      func.func @copy_kernel(%a: memref<4x4xf32>, %b: memref<4x4xf32>) {
        %c0 = arith.constant 0 : index
        %zero = arith.constant 0.0 : f32
        %value = memref.load %a[%c0, %c0] : memref<4x4xf32>
        %positive = arith.cmpf ogt, %value, %zero : f32
        scf.if %positive {
          memref.store %value, %b[%c0, %c0] : memref<4x4xf32>
        }
        return
      }
    }'''
    for start, expected in [(0, 1), (1, 2)]:
        eid = client.upload(branch, inputs(start=start))
        client.wait(eid, 'ready')
        assert client.request('POST', '/' + eid + '/run', spec())[0] == 202
        client.wait(eid, 'completed')
        assert client.request('GET', '/' + eid + '/report')[1]['trace']['total_events'] == expected

    affine = b'''module {
      func.func @copy_kernel(%a: memref<4x4xf32>, %b: memref<4x4xf32>) {
        affine.for %i = 0 to 4 {
          affine.for %j = 0 to 4 {
            %value = affine.load %a[%i, %j] : memref<4x4xf32>
            affine.store %value, %b[%i, %j] : memref<4x4xf32>
          }
        }
        return
      }
    }'''
    eid = client.upload(affine)
    client.wait(eid, 'ready')
    assert client.request('POST', '/' + eid + '/run', spec())[0] == 202
    client.wait(eid, 'completed')
    assert client.request('GET', '/' + eid + '/report')[1]['trace']['total_events'] == 32

    for shape, indexes in [((), ''), ((1, 2, 2), '%zero, %zero, %zero')]:
        ty = 'memref<' + ''.join(str(n) + 'x' for n in shape) + 'f32>'
        mlir = ('module { func.func @copy_kernel(%a: '+ty+', %b: '+ty+') { '
                '%zero = arith.constant 0 : index '
                '%v = memref.load %a['+indexes+'] : '+ty+' '
                'memref.store %v, %b['+indexes+'] : '+ty+' return } }').encode()
        eid = client.upload(mlir, inputs(shape=shape))
        client.wait(eid, 'ready')
        assert client.request('POST', '/' + eid + '/run', spec())[0] == 202
        client.wait(eid, 'completed')
        assert client.request('GET', '/' + eid + '/report')[1]['trace']['total_events'] == 2
    print('NPZ values affect execution; affine, rank-zero and rank-three ABI OK')

    unsupported_operation = b'''module {
      func.func @copy_kernel(%a: memref<4x4xf32>, %b: memref<4x4xf32>) {
        memref.copy %a, %b : memref<4x4xf32> to memref<4x4xf32>
        return
      }
    }'''
    eid = client.upload(unsupported_operation)
    client.wait(eid, 'ready')
    assert client.request('POST', '/' + eid + '/run', spec())[0] == 202
    assert client.wait(eid, 'failed')['error']['code'] == 'compilation_failed'

    crashing = b'''module {
      func.func @copy_kernel(%a: memref<4x4xf32>, %b: memref<4x4xf32>) {
        %far = arith.constant 281474976710656 : index
        %zero = arith.constant 0 : index
        %v = memref.load %a[%far, %zero] : memref<4x4xf32>
        memref.store %v, %b[%zero, %zero] : memref<4x4xf32>
        return
      }
    }'''
    eid = client.upload(crashing)
    client.wait(eid, 'ready')
    assert client.request('POST', '/' + eid + '/run', spec())[0] == 202
    assert client.wait(eid, 'failed')['error']['code'] == 'execution_failed'
    assert client.request('GET', '/' + eid + '/report')[0] == 409
    print('compile/native failures stay isolated; no partial report exposed')

    # Leave the UI in a prepared, unconfigured state for browser checks.
    eid = client.upload(source)
    client.wait(eid, 'ready')


if __name__ == '__main__':
    main()
