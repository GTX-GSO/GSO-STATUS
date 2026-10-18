"""Bounded D3D11 fixture probe; never launches a game or retains raw frame captures."""
import csv
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cpu-only',action='store_true',help='Diagnose present events without GPU/display dependencies; cannot validate latency')
parser.add_argument('--tracking',choices=('full','gpu','display','cpu'),default='full',help='Isolate GPU versus display ETW dependencies')
options=parser.parse_args()
tracking='cpu' if options.cpu_only else options.tracking
EXE = ROOT / 'tools/presentmon/PresentMon-2.6.0-x64.exe'
assert hashlib.sha256(EXE.read_bytes()).hexdigest() == 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'
capture = ROOT / ('work/present-fixture-' + uuid.uuid4().hex + '.csv')
session = 'GSO-STATUS-Probe-' + uuid.uuid4().hex
flags = subprocess.CREATE_NO_WINDOW
console = subprocess.STARTUPINFO()
console.dwFlags = subprocess.STARTF_USESHOWWINDOW
console.wShowWindow = subprocess.SW_HIDE
trace = fixture = None
try:
    arguments=[str(EXE), '--process_name', 'GSO-PresentFixture.exe',
        '--output_file', str(capture), '--no_console_stats', '--no_track_input',
        '--qpc_time', '--session_name', session, '--timed', '11',
        '--terminate_after_timed']
    if tracking in ('cpu','display'):arguments+=['--no_track_gpu']
    if tracking in ('cpu','gpu'):arguments+=['--no_track_display']
    trace = subprocess.Popen(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        stdin=subprocess.PIPE, startupinfo=console, creationflags=subprocess.CREATE_NEW_CONSOLE)
    time.sleep(.5)
    fixture = subprocess.Popen([str(ROOT/'work/GSO-PresentFixture.exe')], creationflags=flags)
    fixture_code = fixture.wait(timeout=12)
    trace_code = trace.wait(timeout=6)
    raw = b''
    if capture.exists():
        with capture.open('rb') as stream:
            raw = stream.read(8*1024*1024+1)
    if len(raw) > 8*1024*1024:
        raise RuntimeError('controlled capture exceeds 8 MiB')
    reader = csv.DictReader(io.StringIO(raw.decode('utf-8-sig')))
    columns = reader.fieldnames or []
    rows = list(reader)
    key = 'MsRenderPresentLatency'
    samples = []
    for row in rows:
        if row.get('Application', '').lower() != 'gso-presentfixture.exe' or row.get('ProcessID') != str(fixture.pid):
            continue
        try:
            value = float(row[key])
            if 0 <= value < 10000:
                samples.append(value)
        except (ValueError, KeyError, TypeError):
            pass
    report = dict(scope='Controlled 128x72 D3D11 fixture only; no game, no raw CSV retained',
        presentmon_version='2.6.0', presentmon_exit=trace_code, fixture_exit=fixture_code,
        cpu_only=tracking=="cpu",tracking=tracking,
        columns=columns, rows=len(rows), render_present_latency_samples=len(samples),
        render_present_latency_min_ms=min(samples) if samples else None,
        render_present_latency_max_ms=max(samples) if samples else None,
        verified=bool(tracking in ("full","gpu") and trace_code == 0 and fixture_code == 0 and samples and max(samples)>0),
        basic_present_events_observed=bool(rows),
        actual_game_verified=False)
    target = ROOT/'artifacts/verification'/('presentmon-d3d11-'+tracking+'.json')
    target.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))
finally:
    for child in (fixture, trace):
        if child is not None and child.poll() is None:
            child.kill()
            child.wait(timeout=2)
    # Terminate only this probe's unique ETW session after an interrupted capture.
    if trace is not None and trace.returncode != 0:
        cleanup = subprocess.Popen([str(EXE), '--session_name', session,
            '--terminate_existing_session'],stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,creationflags=flags)
        try:
            cleanup.wait(timeout=2)
        except subprocess.TimeoutExpired:
            cleanup.kill()
            cleanup.wait(timeout=2)
    capture.unlink(missing_ok=True)
