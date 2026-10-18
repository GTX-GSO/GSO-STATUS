"""Run with jsonschema 4.26.0 (see contracts/v1/README.md)."""
import copy
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'work/contract-deps'))
from jsonschema import Draft202012Validator, FormatChecker
assert 'date-time' in FormatChecker.checkers, 'Install contracts/requirements-dev.txt'

base = ROOT / 'contracts/v1'
validators = {}
for name in ('snapshot', 'telemetry','home-command','home-result'):
    schema = json.loads((base / f'{name}.schema.json').read_text(encoding='utf-8'))
    Draft202012Validator.check_schema(schema)
    validators[name] = Draft202012Validator(schema, format_checker=FormatChecker())
count = 0
for path in sorted(base.glob('*.json')):
    if '.schema.' in path.name:
        continue
    data = json.loads(path.read_text(encoding='utf-8'))
    kind=next(name for name in validators if path.name.startswith(name+'-'))
    validators[kind].validate(data)
    assert len(path.read_bytes()) <= 16384, path
    count += 1

sample = json.loads((base / 'telemetry-partial.json').read_text(encoding='utf-8'))
negative_temperature=copy.deepcopy(sample)
negative_temperature['windows']['cpu']['temperature_c']={'value':-5,'reason':None}
validators['telemetry'].validate(negative_temperature)
def rejects(mutate):
    candidate = copy.deepcopy(sample)
    mutate(candidate)
    assert not validators['telemetry'].is_valid(candidate), candidate

rejects(lambda x: x['windows']['cpu']['load_percent'].update(value=101))
rejects(lambda x: x['windows']['cpu']['load_percent'].update(value=None))
rejects(lambda x: x['windows']['cpu']['temperature_c'].update(reason=None))
rejects(lambda x: x.update(seq=-1))
rejects(lambda x: x.update(sampled_at='yesterday'))
rejects(lambda x: x.update(boot_id='not-a-uuid'))
rejects(lambda x: x.update(unknown_field=True))
rejects(lambda x: x['game'].update(fps_history=[{'value':60,'reason':None}]*25))
rejects(lambda x: x['codex'].update(sessions=[{'id':'a','title':'b','state':'idle'}]*5))
rejects(lambda x: x['windows'].pop('source'))
rejects(lambda x: x['codex'].update(clients=[{'id':'codex','online':True,'tokens_today':-1}]))
rejects(lambda x: x['codex'].update(clients=[{'id':'other','online':True,'tokens_today':0}]))
rejects(lambda x: x['codex'].update(clients=[{'id':'codex','online':True,'tokens_today':0}]*3))
with_clients=copy.deepcopy(sample);with_clients['codex']['clients']=[{'id':'codex','online':False,'tokens_today':None},{'id':'claude','online':True,'tokens_today':5}]
validators['telemetry'].validate(with_clients)
printer_fixture = json.loads((base / 'snapshot-printer.json').read_text(encoding='utf-8'))
def rejects_snapshot(mutate):
    candidate = copy.deepcopy(printer_fixture)
    mutate(candidate)
    assert not validators['snapshot'].is_valid(candidate), candidate

rejects_snapshot(lambda x: x['printer']['progress_percent'].update(value=101))
rejects_snapshot(lambda x: x['printer'].update(state='melting'))
rejects_snapshot(lambda x: x['printer']['nozzle_c'].update(value=401))
rejects_snapshot(lambda x: x['printer'].update(access_code='secret'))
rejects_snapshot(lambda x: x['printer'].pop('layer'))
rejects_snapshot(lambda x: x['printer']['layer'].update(value=-1))
rejects_snapshot(lambda x: x['printer'].update(thumbnail_id='not-a-picture-id'))
car_fixture = json.loads((base / 'snapshot-car.json').read_text(encoding='utf-8'))
def rejects_car(mutate):
    candidate = copy.deepcopy(car_fixture)
    mutate(candidate)
    assert not validators['snapshot'].is_valid(candidate), candidate

rejects_car(lambda x: x['car']['battery_percent'].update(value=101))
rejects_car(lambda x: x['car'].update(lock_state='ajar'))
rejects_car(lambda x: x['car'].update(charge_state='full'))
rejects_car(lambda x: x['car'].update(actions=['lock', 'unlock', 'press', 'lock']))
rejects_car(lambda x: x['car'].update(actions=['open_trunk']))
rejects_car(lambda x: x['car'].update(pin='0000'))
rejects_car(lambda x: x['car'].pop('odometer_km'))
rejects_car(lambda x: x['car']['range_km'].update(value=-1))
rejects_car(lambda x: x['car'].update(updated_at=12))
rejects_car(lambda x: x['car']['tyre_fl_bar'].update(value=11))
rejects_car(lambda x: x['car'].update(tyre_alarm='warning'))
rejects_car(lambda x: x['car'].update(windows_state='ajar'))
rejects_car(lambda x: x['car'].update(trunk_state=True))
rejects_car(lambda x: x['car']['charging_power_kw'].update(value=501))
rejects_car(lambda x: x['car'].pop('tyre_rr_bar'))
nas_fixture = json.loads((base / 'snapshot-nas.json').read_text(encoding='utf-8'))
def rejects_nas(mutate):
    candidate = copy.deepcopy(nas_fixture)
    mutate(candidate)
    assert not validators['snapshot'].is_valid(candidate), candidate

rejects_nas(lambda x: x['nas']['cpu_percent'].update(value=101))
rejects_nas(lambda x: x['nas']['memory_percent'].update(value=-1))
rejects_nas(lambda x: x['nas']['soc_temperature_c'].update(value=201))
rejects_nas(lambda x: x['nas']['memory_total_bytes'].update(value=1.5))
rejects_nas(lambda x: x['nas'].update(hostname='nas'))
rejects_nas(lambda x: x['nas'].pop('disk_temperature_c'))
rejects_nas(lambda x: x['nas'].update(disk_state='sleeping'))
rejects_nas(lambda x: x['nas'].update(disk_state=None))
rejects_nas(lambda x: x['nas']['disk_busy_percent'].update(value=101))
rejects_nas(lambda x: x['nas']['disk_read_bytes_per_second'].update(value=-1))
rejects_nas(lambda x: x['nas']['emmc_busy_percent'].update(value=100.5))
rejects_nas(lambda x: x['nas']['disk_wake_count_today'].update(value='3'))
rejects_nas(lambda x: x['nas'].update(disk_health='fine'))
rejects_nas(lambda x: x['nas']['disk_power_on_hours'].update(value='11234'))
rejects_nas(lambda x: x['nas']['disk_pending_sectors'].update(value=None))
rejects_nas(lambda x: x['nas']['emmc_write_bytes_per_second'].update(value=-1))
rejects_nas(lambda x: x['windows']['disks'][0]['busy_percent'].update(value=100.5))
rejects_nas(lambda x: x['windows']['disks'][0]['read_ms'].update(value=-0.1))
rejects_nas(lambda x: x['windows']['disks'][0].update(write_ms={'value':1}))
rejects_nas(lambda x: x['windows']['disks'][0].update(queue=1))
rejects_nas(lambda x: x['windows']['disks'][0]['response_ms'].update(value=-1))
rejects_nas(lambda x: x['windows']['disks'][0]['read_bytes_per_second'].update(value=-1))
rejects_nas(lambda x: x['windows']['disks'][0].update(write_bytes_per_second=5))
rejects(lambda x: x['windows']['disks'][0]['busy_percent'].update(value=101))
old_disk=copy.deepcopy(sample);[old_disk['windows']['disks'][0].pop(k) for k in ('busy_percent','read_ms','write_ms','response_ms','read_bytes_per_second','write_bytes_per_second')]
validators['telemetry'].validate(old_disk)
print(f'PASS: {len(validators)} schemas, {count} fixtures, 59 invalid payload cases')
