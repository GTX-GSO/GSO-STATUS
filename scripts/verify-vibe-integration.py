"""Real local VIBE metadata through isolated GSW/GSN; no NAS or household writes."""
import argparse
import datetime
import json
import re
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import time
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'work/contract-deps'))
from jsonschema import Draft202012Validator, FormatChecker

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None

parser = argparse.ArgumentParser()
parser.add_argument('--gsw',type=Path,default=ROOT/'artifacts/GSW/GSW.exe')
parser.add_argument('--scenario',choices=('metadata','rpc','hardware'),default='metadata')
parser.add_argument('--seconds',type=int,default=10)
args = parser.parse_args()
if not 5<=args.seconds<=180:parser.error('seconds must be 5..180')
gsw = args.gsw.resolve()
work = ROOT/('work/vibe-integration-'+uuid.uuid4().hex)
work.mkdir()
with socket.socket() as probe:
    probe.bind(('127.0.0.1',0))
    port=probe.getsockname()[1]
base=f'http://127.0.0.1:{port}'
agent='verify-'+uuid.uuid4().hex[:12]
write,read=secrets.token_hex(32),secrets.token_hex(32)
nas=dict(listen=f'127.0.0.1:{port}',agent_id=agent,write_token=write,read_token=read,router_url='')
(work/'gsn.json').write_text(json.dumps(nas),encoding='utf-8')
config=json.loads((ROOT/'config/gsw-nas.json').read_text(encoding='utf-8-sig'))
config.update(SnapshotBaseUrl=base,AgentId=agent,WriteToken=write,ObserveCodexTasks=True,
    GameExecutables=[],StartRtssOnDemand=False,AutoDetectGames=False,HideRtssOverlay=False)
if args.scenario=='rpc':config['ObserveCodexTasks']=False
(work/'gsw.json').write_text(json.dumps(config),encoding='utf-8')
validator=Draft202012Validator(json.loads((ROOT/'contracts/v1/snapshot.schema.json').read_text(encoding='utf-8')),format_checker=FormatChecker())
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),NoRedirect())
def get(path):
    request=urllib.request.Request(base+path,headers={'Authorization':'Bearer '+read})
    with opener.open(request,timeout=1) as response:
        raw=response.read(32769)
    if len(raw)>32768:
        raise RuntimeError('snapshot exceeds contract limit')
    value=json.loads(raw)
    validator.validate(value)
    return value

processes=[]
started=time.monotonic()
report=dict(scope='Two isolated local production executables, actual read-only Windows/Codex sources, no NAS or household write',scenario=args.scenario,gsw_version='0.1.10',gsn_version='0.1.18',passed=False)
try:
    processes.append(subprocess.Popen([str(ROOT/'artifacts/GSN/GSN.exe'),'--config',str(work/'gsn.json')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW))
    processes.append(subprocess.Popen([str(gsw),'--config',str(work/'gsw.json')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW))
    samples=[]
    previous_id=None
    quota_observations=set()
    deadline=started+80+args.seconds
    while time.monotonic()<deadline and len(samples)<args.seconds:
        if any(p.poll() is not None for p in processes):
            raise RuntimeError('isolated process exited before validation')
        try:
            model=get('/api/v1/snapshot')
        except (OSError,ValueError):
            time.sleep(.5)
            continue
        windows=model['windows']['source']
        codex=model['codex']
        if args.scenario=='hardware':
            win=model['windows']
            values=[win['cpu'].get('clock_mhz',{}).get('value'),win['cpu'].get('memory_rate_mts',{}).get('value'),win['gpu'].get('clock_mhz',{}).get('value'),win['gpu'].get('memory_clock_mhz',{}).get('value'),win['cpu'].get('memory_clock_mhz',{}).get('value'),win['cpu'].get('memory_clock_source')]
            if windows['quality']=='ok' and all(isinstance(v,(int,float)) and 0<v<=100000 for v in values[:4]):samples.append((time.monotonic(),values))
            time.sleep(1)
            continue
        if args.scenario=='rpc':
            if windows['quality']=='ok' and codex['source']['quality']=='ok' and codex['quotas']:
                quota_observations.add(codex['source']['observed_at'])
                samples.append((time.monotonic(),0))
            time.sleep(1)
            continue
        sessions=[s for s in codex['sessions'] if s['state']=='running' and s.get('title') and s.get('running_duration_ms') is not None and s.get('started_at')]
        if windows['quality']=='ok' and sessions:
            session=sessions[0]
            if previous_id!=session['id']:
                samples=[]
                previous_id=session['id']
            # .NET's round-trip form has seven fractional digits; Python 3.10
            # accepts microseconds. Contract validation above checks the full text.
            stamp_text=re.sub(r'(\.\d{6})\d+(?=[+-]\d{2}:\d{2}$)',r'\1',session['started_at'].replace('Z','+00:00'))
            stamp=datetime.datetime.fromisoformat(stamp_text)
            if stamp>datetime.datetime.now(datetime.timezone.utc):
                raise RuntimeError('task start is in the future')
            samples.append((time.monotonic(),session['running_duration_ms']))
        time.sleep(1)
    if len(samples)<args.seconds:
        raise RuntimeError('requested real-source scenario unavailable within cold-start window')
    if args.scenario=='hardware':
        report.update(passed=True,valid_clock_samples=len(samples),cpu_clock_mhz=[min(x[1][0] for x in samples),max(x[1][0] for x in samples)],ram_configured_mts=samples[-1][1][1],ram_clock_mhz=samples[-1][1][4],ram_clock_source=samples[-1][1][5],gpu_clock_mhz=[min(x[1][2] for x in samples),max(x[1][2] for x in samples)],vram_clock_mhz=[min(x[1][3] for x in samples),max(x[1][3] for x in samples)],seconds=round(time.monotonic()-started,2),real_household_actions=0,nas_container_changed=False)
    elif args.scenario=='rpc':
        report.update(passed=True,valid_quota_samples=len(samples),rpc_observation_count=len(quota_observations),
            local_quota_events_disabled=True,model_tasks_started=0,seconds=round(time.monotonic()-started,2),
            nas_container_changed=False,quota_amounts_or_raw_rpc_saved=False)
    else:
        durations=[x[1] for x in samples]
        if any(b<a for a,b in zip(durations,durations[1:])):
            raise RuntimeError('duration decreased in the same session')
        wall_ms=(samples[-1][0]-samples[0][0])*1000
        growth=durations[-1]-durations[0]
        if growth<3000 or abs(growth-wall_ms)>3000:
            raise RuntimeError('duration progression does not match observation interval')
        report.update(passed=True,valid_named_running_samples=len(samples),same_session=True,
            actual_title_present=True,actual_start_present=True,duration_monotonic=True,
            duration_growth_ms=growth,observation_interval_ms=round(wall_ms),
            seconds=round(time.monotonic()-started,2),real_household_actions=0,
            nas_container_changed=False,titles_ids_or_bodies_saved=False)
except Exception as error:
    report['error_type']=type(error).__name__
    raise
finally:
    if len(processes)>1 and processes[1].poll() is None:
        stop=subprocess.Popen([str(gsw),'--config',str(work/'gsw.json'),'--stop'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
        try:stop.wait(timeout=3)
        except subprocess.TimeoutExpired:stop.kill();stop.wait(timeout=2)
    for p in reversed(processes):
        try:p.wait(timeout=3)
        except subprocess.TimeoutExpired:p.terminate();p.wait(timeout=3)
    name={'metadata':'vibe-integration-0.1.10.json','rpc':'quota-rpc-integration-0.1.10.json','hardware':'hardware-clocks-0.1.10.json'}[args.scenario]
    (ROOT/'artifacts/verification'/name).write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))
