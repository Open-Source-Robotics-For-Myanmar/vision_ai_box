#!/usr/bin/env python3
"""Check a running Vision AI Box; this test takes over the current client."""
import argparse
import json
import urllib.error
import urllib.parse
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:8080')
    args = parser.parse_args()
    base = args.url.rstrip('/')

    def request(path, token='', body=None):
        req = urllib.request.Request(base + path,
            data=None if body is None else json.dumps(body).encode(),
            headers={'X-Vision-Client': token, 'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(req, timeout=15) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as error:
            return error.code, error.read()

    def claim():
        status, body = request('/api/client', body={})
        assert status == 200, (status, body)
        return json.loads(body)['client']

    first = claim()
    assert request('/api/camera/status', first)[0] == 200
    events = urllib.request.urlopen(base + '/api/events?client=' + urllib.parse.quote(first), timeout=15)
    try:
        assert events.readline().startswith(b'event:')
        second = claim()
        assert first != second
        replaced = False
        for _ in range(1000):
            line = events.readline()
            if not line:
                break
            if line.strip() == b'event: session_replaced':
                replaced = True
                break
        assert replaced, 'SSE replacement event missing'
    finally:
        events.close()
    assert request('/api/camera/status', first)[0] == 409
    assert request('/api/camera/stop', first, {})[0] == 409
    assert request('/api/camera/status')[0] == 409
    assert request('/api/camera/status', second)[0] == 200
    assert request('/api/camera/settings', second, {'color_fps': 15})[0] == 400
    assert request('/api/camera/settings', second, {'depth_fps': 15})[0] == 400
    status, body = request('/api/camera/settings', second)
    assert status == 200
    config = json.loads(body)
    assert config['settings']['color_fps'] == 30
    assert 'color_fps_options' not in config['available_settings']
    assert 'depth_fps_options' not in config['available_settings']
    assert request('/api/login', second, {})[0] == 404
    # New pipeline endpoints must use the same client ownership rules.
    assert request('/api/pipeline', second)[0] == 200
    assert request('/api/pipeline', first, {'enabled': False})[0] == 409
    third = claim()
    assert request('/api/camera/status', second)[0] == 409
    assert request('/api/camera/status', third)[0] == 200
    print('Client takeover, SSE invalidation and fixed-FPS tests passed')


if __name__ == '__main__':
    main()
