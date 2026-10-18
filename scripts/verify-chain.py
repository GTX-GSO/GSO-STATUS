"""Bounded chain acceptance probe; stores aggregate evidence, never raw snapshots/titles."""
import argparse
import concurrent.futures
import json
import math
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'work/contract-deps'))
from jsonschema import Draft202012Validator, FormatChecker


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def sample(opener, url, token, validator, require_cpu):
    start = time.monotonic()
    try:
        request = urllib.request.Request(url, headers={'Authorization': 'Bearer ' + token})
        with opener.open(request, timeout=0.8) as response:
            raw = response.read(32769)
        if len(raw) > 32768:
            return {'error': 'response_too_large'}
        snapshot = json.loads(raw)
        if not validator.is_valid(snapshot):
            return {'error': 'invalid_contract'}
        windows = snapshot['windows']
        if windows['source']['quality'] != 'ok':
            return {'error': 'windows_not_fresh'}
        cpu = windows['cpu']
        load = cpu['load_percent']['value']
        if load is None or not math.isfinite(load) or not 0 <= load <= 100:
            return {'error': 'cpu_load_missing'}
        if require_cpu and any(cpu[key]['value'] is None for key in ('temperature_c', 'power_w')):
            return {'error': 'cpu_sensor_missing'}
        # Only scalar aggregate inputs leave this function. Do not return titles or raw JSON.
        active = [s for s in snapshot['codex']['sessions'] if s['state'] == 'running']
        return dict(ms=(time.monotonic()-start)*1000, size=len(raw),
                    windows_age_ms=windows['source']['age_ms'],
                    wan_ok=snapshot['net']['wan']['source']['quality'] == 'ok',
                    quota_ok=snapshot['codex']['source']['quality'] == 'ok',
                    running=snapshot['codex']['state'] == 'running',
                    active_title=bool(active and active[0].get('title')),
                    active_duration=bool(active and active[0].get('running_duration_ms') is not None),
                    temperature=cpu['temperature_c']['value'], power=cpu['power_w']['value'])
    except urllib.error.HTTPError as error:
        return {'error': 'http_' + str(error.code)}
    except (OSError, ValueError, KeyError, TypeError):
        return {'error': 'request_or_payload_failed'}


def run(profile, seconds, clients, require_cpu, output):
    configuration = json.loads(profile.read_text(encoding='utf-8-sig'))
    url, token = configuration['url'], configuration['read_token']
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError('invalid profile URL')
    if len(token) < 32 or any(c in token for c in '\r\n'):
        raise ValueError('invalid read token')
    validator = Draft202012Validator(json.loads((ROOT/'contracts/v1/snapshot.schema.json').read_text(encoding='utf-8')), format_checker=FormatChecker())
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    durations, sizes, ages, temperatures, powers = [], [], [], [], []
    errors = {}
    streaks = [0]*clients
    longest_streak = 0
    recoveries = 0
    counts = {key: 0 for key in ('wan_ok', 'quota_ok', 'running', 'active_title', 'active_duration')}
    start = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:
        for second in range(seconds):
            for client, result in enumerate(pool.map(lambda _: sample(opener, url, token, validator, require_cpu), range(clients))):
                if 'error' in result:
                    code = result['error']; first = code not in errors
                    errors[code] = errors.get(code, 0) + 1
                    streaks[client] += 1
                    longest_streak = max(longest_streak, streaks[client])
                    if first:
                        print(json.dumps({'elapsed_samples_s':second+1,'error_counts':errors}),flush=True)
                    continue
                if streaks[client]: recoveries += 1
                streaks[client] = 0
                durations.append(result['ms']); sizes.append(result['size']); ages.append(result['windows_age_ms'])
                for key in counts:
                    counts[key] += int(result[key])
                if result['temperature'] is not None: temperatures.append(result['temperature'])
                if result['power'] is not None: powers.append(result['power'])
            if (second+1) % 60 == 0:
                print(json.dumps({'elapsed_samples_s': second+1, 'valid_responses': len(durations), 'errors': sum(errors.values()),'error_counts':errors}), flush=True)
            time.sleep(max(0, start+second+1-time.monotonic()))
    ordered = sorted(durations)
    report = dict(scope='HTTP chain from this Windows client; not NAS process resource or P4 acceptance',
                  requested_seconds=seconds, elapsed_seconds=round(time.monotonic()-start, 2), clients=clients,
                  requests=seconds*clients, valid_responses=len(durations), errors=errors,
                  longest_consecutive_failures_per_client=longest_streak,recovery_events=recoveries,
                  get_p95_ms=round(ordered[math.ceil(len(ordered)*.95)-1], 3) if ordered else None,
                  get_max_ms=round(max(durations), 3) if durations else None,
                  snapshot_max_bytes=max(sizes, default=None), windows_max_age_ms=max(ages, default=None),
                  temperature_range_c=[min(temperatures), max(temperatures)] if temperatures else None,
                  power_range_w=[min(powers), max(powers)] if powers else None,
                  counts=counts, raw_snapshots_saved=False, titles_saved=False,
                  passed=not errors and len(durations)==seconds*clients)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, indent=2), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=7200, choices=range(1, 86401), metavar='1..86400')
    parser.add_argument('--clients', type=int, default=1, choices=(1, 2, 3))
    parser.add_argument('--require-cpu-sensors', action='store_true')
    parser.add_argument('--output', type=Path, default=ROOT/'artifacts/verification/chain-soak-latest.json')
    args = parser.parse_args()
    sys.exit(run(args.profile, args.seconds, args.clients, args.require_cpu_sensors, args.output))
