"""Claude VIBE end to end: synthetic Claude Code transcript + status line -> GSW -> local GSN -> GSS simulator capture.

Also checks the GSN 0.1.20 / GSW 0.1.16 additions: codex.clients (both listed, Claude tokens counted from the synthetic transcript, cache reads excluded)
and the NAS storage volumes (a temporary directory stands in for a NAS volume).

Everything runs isolated under work/: a fresh GSN on a random loopback port, a temporary Claude projects folder and
rate-limit bridge. Real Claude settings, credentials and transcripts are not read; no NAS or household writes.
"""
import argparse
import datetime
import json
import secrets
import socket
import subprocess
import sys
import time
import urllib.request
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'work/contract-deps'))
try:
    from jsonschema import Draft202012Validator, FormatChecker
except ImportError:  # contract-deps is built for one Python version; GSN still schema-validates every snapshot it serves
    Draft202012Validator = None

parser = argparse.ArgumentParser()
parser.add_argument('--gsw', type=Path, default=ROOT/'artifacts/GSW/GSW.exe')
parser.add_argument('--gsn', type=Path, default=ROOT/'artifacts/GSN/GSN.exe')
parser.add_argument('--simulator', type=Path, default=ROOT/'artifacts/GSS-Test.exe')
parser.add_argument('--with-codex', action='store_true', help='also observe the real local Codex logs (read-only)')
args = parser.parse_args()

work = ROOT/('work/claude-vibe-'+uuid.uuid4().hex[:12])
work.mkdir(parents=True)
projects = work/'claude-projects'/'C--work-demo'
projects.mkdir(parents=True)
session = str(uuid.uuid4())
transcript = projects/(session+'.jsonl')
transcript.write_text('', encoding='utf-8')
bridge = work/'claude-rate-limits.json'
with socket.socket() as probe:
    probe.bind(('127.0.0.1', 0))
    port = probe.getsockname()[1]
base = f'http://127.0.0.1:{port}'
agent = 'claude-'+uuid.uuid4().hex[:10]
write, read = secrets.token_hex(32), secrets.token_hex(32)
(work/'gsn.json').write_text(json.dumps(dict(listen=f'127.0.0.1:{port}', agent_id=agent, write_token=write, read_token=read, router_url='', storage=[dict(label='e2e', path=str(work))])), encoding='utf-8')
config = json.loads((ROOT/'config/gsw-nas.json').read_text(encoding='utf-8-sig'))
config.update(SnapshotBaseUrl=base+'/', AgentId=agent, WriteToken=write, ObserveCodexTasks=args.with_codex,
              ObserveClaudeTasks=True, ClaudeProjectDirectories=[str(projects.parent)], ClaudeRateLimitsFile=str(bridge),
              GameExecutables=[], StartRtssOnDemand=False, AutoDetectGames=False, HideRtssOverlay=False)
if not args.with_codex:
    config['CodexExecutable'] = ''
(work/'gsw.json').write_text(json.dumps(config), encoding='utf-8')
(work/'display.json').write_text(json.dumps(dict(url=base+'/api/v1/snapshot', read_token=read)), encoding='utf-8')
validator = Draft202012Validator(json.loads((ROOT/'contracts/v1/snapshot.schema.json').read_text(encoding='utf-8')), format_checker=FormatChecker()) if Draft202012Validator else None
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def now():
    return datetime.datetime.now(datetime.timezone.utc)

def line(record):
    record.setdefault('timestamp', now().isoformat().replace('+00:00', 'Z'))
    record.setdefault('sessionId', session)
    with transcript.open('a', encoding='utf-8', newline='\n') as handle:
        handle.write(json.dumps(record, ensure_ascii=False)+'\n')

def snapshot():
    request = urllib.request.Request(base+'/api/v1/snapshot', headers={'Authorization': 'Bearer '+read})
    with opener.open(request, timeout=1) as response:
        value = json.loads(response.read(32769))
    if validator:
        validator.validate(value)
    return value

def wait_for(predicate, seconds, keepalive=None):
    deadline = time.monotonic()+seconds
    last = None
    while time.monotonic() < deadline:
        try:
            last = snapshot()
            if predicate(last):
                return last
        except (OSError, ValueError):
            pass
        if keepalive:
            keepalive()
        time.sleep(1)
    raise RuntimeError('condition not reached; last codex='+json.dumps((last or {}).get('codex'), ensure_ascii=False)[:600])

def capture(name, detail=None):
    command = [str(args.simulator), '--config', str(work/'display.json'), '--capture-file', str(work/name)]
    if detail is not None:
        command += ['--capture-detail', str(detail)]
    result = subprocess.run(command, cwd=work, timeout=30)
    if result.returncode != 0:
        raise RuntimeError(f'simulator capture failed: {name} rc={result.returncode}')
    return str((work/name).relative_to(ROOT))

report = dict(scope='Isolated GSW+GSN+GSS simulator with synthetic Claude Code transcript and status line input', passed=False,
              client_schema_validation=Draft202012Validator is not None, real_claude_files_read=False, nas_container_changed=False, household_actions=0)
processes = []
try:
    reset_5h = int(time.time())+2*3600
    reset_7d = int(time.time())+4*86400
    status_input = json.dumps({'model': {'display_name': 'Opus'}, 'session_id': session, 'session_name': 'private name',
                               'rate_limits': {'five_hour': {'used_percentage': 23.5, 'resets_at': reset_5h},
                                               'seven_day': {'used_percentage': 41.2, 'resets_at': reset_7d}}})
    status = subprocess.run([str(args.gsw), '--claude-statusline', '--output', str(bridge)], input=status_input.encode('utf-8'), capture_output=True, timeout=20)
    printed = status.stdout.decode('utf-8').strip()
    stored = json.loads(bridge.read_text(encoding='utf-8'))
    if status.returncode != 0 or printed != '[Opus] 5h 77% | 7d 59% left' or 'private' in bridge.read_text(encoding='utf-8') or stored['five_hour']['used_percentage'] != 23.5:
        raise RuntimeError('status line bridge mismatch: '+printed)
    report['status_line_output'] = printed

    processes.append(subprocess.Popen([str(args.gsn), '--config', str(work/'gsn.json')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW))
    processes.append(subprocess.Popen([str(args.gsw), '--config', str(work/'gsw.json')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW))

    idle = wait_for(lambda m: m['windows']['source']['quality'] == 'ok' and m['codex']['state'] == 'idle' and any(q['bucket'] == 'claude' for q in m['codex']['quotas']), 40)
    report['idle_before_prompt'] = True
    line({'type': 'custom-title', 'customTitle': 'VIBE接入Claude平台'})
    line({'type': 'user', 'message': {'role': 'user', 'content': '把VIBE监控接入Claude（测试正文，不应出现在快照）'}})
    # One assistant record with token usage: input 10 + cache write 20 + output 30 count, the 999 cache reads do not.
    line({'type': 'assistant', 'requestId': 'req_e2e', 'message': {'id': 'msg_e2e', 'role': 'assistant', 'stop_reason': 'tool_use', 'content': [{'type': 'tool_use', 'id': 'u', 'name': 'Bash', 'input': {}}],
                                                              'usage': {'input_tokens': 10, 'cache_creation_input_tokens': 20, 'cache_read_input_tokens': 999, 'output_tokens': 30}}})
    started = time.monotonic()
    def working():
        if time.monotonic()-working.last > 5:
            line({'type': 'assistant', 'message': {'role': 'assistant', 'stop_reason': 'tool_use', 'content': [{'type': 'tool_use', 'id': 't', 'name': 'Bash', 'input': {}}]}})
            line({'type': 'user', 'message': {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': 't', 'content': 'ok'}]}})
            working.last = time.monotonic()
    working.last = time.monotonic()
    running = wait_for(lambda m: m['codex']['state'] == 'running' and m['codex']['sessions'] and m['codex']['sessions'][0]['id'].startswith('claude-')
                       and m['codex']['sessions'][0]['title'] == 'VIBE接入Claude平台', 30, working)
    raw = json.dumps(running, ensure_ascii=False)
    if '测试正文' in raw or 'private name' in raw or session in raw:
        raise RuntimeError('message body, session name or raw session id leaked into snapshot')
    first = running['codex']['sessions'][0]
    durations = []
    for _ in range(6):
        working()
        model = snapshot()
        durations.append(model['codex']['sessions'][0]['running_duration_ms'])
        time.sleep(1)
    if any(b < a for a, b in zip(durations, durations[1:])) or durations[-1]-durations[0] < 3000:
        raise RuntimeError(f'duration not progressing: {durations}')
    # Client facts (GSN 0.1.20 / GSW 0.1.16): both clients listed, today's Claude tokens from the synthetic transcript only.
    header = opener.open(urllib.request.Request(base+'/healthz'), timeout=2).headers.get('X-GSO-Vibe-Clients')
    if header != '1':
        raise RuntimeError('GSN does not advertise X-GSO-Vibe-Clients')
    with_tokens = wait_for(lambda m: {c['id']: c for c in m['codex'].get('clients', [])}.get('claude', {}).get('tokens_today') == 60, 20, working)
    clients = {c['id']: c for c in with_tokens['codex']['clients']}
    if set(clients) != {'codex', 'claude'} or not all(isinstance(c['online'], bool) for c in clients.values()):
        raise RuntimeError('codex.clients malformed: '+json.dumps(clients))
    if not args.with_codex and clients['codex']['tokens_today'] != 0:
        raise RuntimeError('isolated run read real Codex logs')
    volumes = (with_tokens.get('storage') or {}).get('volumes') or []
    if not volumes or volumes[0]['label'] != 'e2e' or not volumes[0]['total_bytes']['value'] or volumes[0]['free_bytes']['value'] > volumes[0]['total_bytes']['value'] or with_tokens['storage']['source']['quality'] != 'ok':
        raise RuntimeError('storage volumes missing or inconsistent: '+json.dumps(with_tokens.get('storage')))
    report.update(clients={k: dict(online=v['online'], tokens_today=v['tokens_today']) for k, v in clients.items()}, storage_volume=dict(label=volumes[0]['label'], total_bytes=volumes[0]['total_bytes']['value']))
    claude_quotas = {q['window_duration_mins']: q for q in model['codex']['quotas'] if q['bucket'] == 'claude'}
    if claude_quotas[300]['remaining_percent']['value'] != 76.5 or abs(claude_quotas[10080]['remaining_percent']['value']-58.8) > 1e-6:
        raise RuntimeError('claude quota mismatch')
    report.update(running_detected_s=round(time.monotonic()-started, 1), session_id_prefixed=True, title_from_custom_title=True,
                  durations_ms=durations, quota_remaining={'5h': 76.5, '7d': 58.8}, codex_source_quality=model['codex']['source']['quality'])
    working()
    report['capture_running_card'] = capture('vibe-claude-running-card.bmp')
    working()
    report['capture_running_detail'] = capture('vibe-claude-running-detail.bmp', 5)
    line({'type': 'assistant', 'message': {'role': 'assistant', 'stop_reason': 'end_turn', 'content': [{'type': 'text', 'text': 'done'}]}})
    wait_for(lambda m: m['codex']['state'] == 'idle' and m['codex']['sessions'][0]['state'] == 'idle' and m['codex']['sessions'][0]['running_duration_ms'] is None, 15)
    report['end_turn_returns_ready'] = True
    report['capture_idle_card'] = capture('vibe-claude-idle-card.bmp')
    report['passed'] = True
except Exception as error:
    report['error'] = f'{type(error).__name__}: {error}'
    raise
finally:
    if len(processes) > 1 and processes[1].poll() is None:
        subprocess.run([str(args.gsw), '--config', str(work/'gsw.json'), '--stop'], timeout=10)
    if processes and processes[0].poll() is None:
        processes[0].terminate()
    for process in reversed(processes):
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)
    report['work_folder'] = str(work.relative_to(ROOT))
    out = ROOT/'artifacts/verification/claude-vibe-integration-0.1.16.json'
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))
