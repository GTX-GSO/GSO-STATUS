"""Exercise the running local chain; save only bounded, credential-free evidence."""
import argparse
import concurrent.futures
import json
import statistics
import time
import urllib.error
import urllib.request
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--seconds', type=int, default=30)
parser.add_argument('--clients', type=int, default=3)
args = parser.parse_args()
assert 1 <= args.clients <= 3 and 1 <= args.seconds <= 7200
config = json.loads((root/'config/gsn.json').read_text(encoding='utf-8-sig'))

def get():
    request = urllib.request.Request('http://127.0.0.1:17890/api/v1/snapshot',
        headers={'Authorization': 'Bearer '+config['read_token']})
    start = time.monotonic()
    with urllib.request.urlopen(request, timeout=2) as response:
        raw = response.read(32769)
    assert len(raw) <= 32768
    return json.loads(raw), (time.monotonic()-start)*1000, len(raw)

times = []
maximum_size = 0
last = None
with concurrent.futures.ThreadPoolExecutor(max_workers=args.clients) as pool:
    for _ in range(args.seconds):
        start = time.monotonic()
        for snapshot, duration, size in pool.map(lambda _: get(), range(args.clients)):
            times.append(duration)
            maximum_size = max(maximum_size, size)
            last = snapshot
        time.sleep(max(0, 1-(time.monotonic()-start)))

assert last is not None
windows = last['windows']
assert windows['source']['quality'] == 'ok', 'GSW is not providing current telemetry'
assert windows['cpu']['load_percent']['value'] is not None, 'CPU warmup/data missing'
assert windows['cpu']['memory_total_bytes']['value'] > 0
gsw_config=json.loads((root/'config/gsw.json').read_text(encoding='utf-8-sig'))
if not gsw_config.get('ObserveCodexTasks', False):
    assert last['codex']['state'] == 'unknown', 'Unconnected task observer must stay unknown'
report = dict(scope='Windows local development chain; not NAS or hardware acceptance',
    clients=args.clients, seconds=args.seconds, requests=len(times),
    get_p95_ms=sorted(times)[int((len(times)-1)*.95)],
    get_mean_ms=statistics.mean(times), snapshot_max_bytes=maximum_size,
    windows_quality=windows['source']['quality'], wan_quality=last['net']['wan']['source']['quality'],
    codex_task_state=last['codex']['state'], quota_windows=len(last['codex']['quotas']),
    cpu_model=windows['cpu']['model'], gpu_model=windows['gpu']['model'],
    disks=[{'id':d['id'],'model':d['model']} for d in windows['disks']])
output = root/'work/local-integration-report.json'
output.write_text(json.dumps(report, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')
(root/'work/live-snapshot.json').write_text(json.dumps(last, ensure_ascii=False), encoding='utf-8')
print(json.dumps(report, indent=2, ensure_ascii=False))
