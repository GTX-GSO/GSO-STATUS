import importlib.util
import json
import threading
import unittest
from unittest.mock import patch
import tempfile
import io
from contextlib import redirect_stdout
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('chain_probe', ROOT/'scripts/verify-chain.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.snapshot = json.loads((ROOT/'contracts/v1/snapshot-vibe-running.json').read_text(encoding='utf-8'))
        self.snapshot['windows']['source'].update(quality='ok', observed_at='2026-10-16T00:00:00Z', age_ms=0, last_error_code=None)
        for key, value in [('load_percent', 0), ('temperature_c', 60), ('power_w', 45)]:
            self.snapshot['windows']['cpu'][key] = {'value': value, 'reason': None}
        self.body = None
        self.redirect = False
        self.paths = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                owner.paths.append(self.path)
                self.send_response(302 if owner.redirect else 200)
                if owner.redirect: self.send_header('Location', '/leak')
                self.end_headers()
                self.wfile.write(owner.body if owner.body is not None else json.dumps(owner.snapshot).encode())

            def log_message(self, *args): pass

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.opener = probe.urllib.request.build_opener(probe.urllib.request.ProxyHandler({}), probe.NoRedirect())
        self.validator = probe.Draft202012Validator(json.loads((ROOT/'contracts/v1/snapshot.schema.json').read_text(encoding='utf-8')), format_checker=probe.FormatChecker())

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def sample(self):
        return probe.sample(self.opener, 'http://127.0.0.1:'+str(self.server.server_port)+'/snapshot', 't'*32, self.validator, True)

    def test_summary_has_no_raw_snapshot_or_title(self):
        result = self.sample()
        self.assertNotIn('error', result)
        self.assertTrue(result['active_title'])
        self.assertTrue(result['active_duration'])
        self.assertNotIn(self.snapshot['codex']['sessions'][0]['title'], json.dumps(result))
        self.assertNotIn('snapshot', result)

    def test_large_response_is_rejected(self):
        self.body = b' '*32769
        self.assertEqual(self.sample()['error'], 'response_too_large')

    def test_redirect_is_not_followed(self):
        self.redirect = True
        self.assertEqual(self.sample()['error'], 'http_302')
        self.assertEqual(self.paths, ['/snapshot'])

    def test_missing_cpu_sensor_fails_acceptance(self):
        self.snapshot['windows']['cpu']['power_w'] = {'value': None, 'reason': 'sensor_unsupported'}
        self.assertEqual(self.sample()['error'], 'cpu_sensor_missing')

    def test_invalid_contract_is_not_a_success(self):
        self.snapshot['codex']['sessions'][0]['running_duration_ms'] = -1
        self.assertEqual(self.sample()['error'], 'invalid_contract')

    def test_recovery_does_not_erase_failed_acceptance(self):
        healthy=self.sample()
        with tempfile.TemporaryDirectory(dir=ROOT/'work') as temp:
            profile=Path(temp)/'profile.json';output=Path(temp)/'summary.json'
            profile.write_text(json.dumps({'url':'http://127.0.0.1/snapshot','read_token':'t'*32}))
            with patch.object(probe,'sample',side_effect=[{'error':'request_or_payload_failed'},{'error':'request_or_payload_failed'},healthy,healthy]),patch.object(probe.time,'sleep'),redirect_stdout(io.StringIO()):
                code=probe.run(profile,4,1,True,output)
            summary=json.loads(output.read_text())
            self.assertEqual(code,1)
            self.assertFalse(summary['passed'])
            self.assertEqual(summary['errors'],{'request_or_payload_failed':2})
            self.assertEqual(summary['recovery_events'],1)
            self.assertEqual(summary['longest_consecutive_failures_per_client'],2)


if __name__ == '__main__': unittest.main()
