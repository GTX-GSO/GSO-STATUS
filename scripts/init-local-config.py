"""Create local-only development credentials, preserving existing configuration."""
import json
import secrets
from pathlib import Path

root = Path(__file__).resolve().parents[1]
folder = root / 'config'
folder.mkdir(exist_ok=True)

def create(name, data):
    path = folder / name
    if not path.exists():
        path.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')

create('gsn.json', dict(listen='127.0.0.1:17890', agent_id='desktop',
    write_token=secrets.token_hex(32), read_token=secrets.token_hex(32),
    router_url='http://192.168.124.1/mobile.asp'))
config = json.loads((folder / 'gsn.json').read_text(encoding='utf-8-sig'))
create('gsw.json', dict(SnapshotBaseUrl='http://127.0.0.1:17890',
    AgentId=config['agent_id'], WriteToken=config['write_token'], GpuIndex=0,
    Drives=['C:\\', 'D:\\', 'E:\\']))
create('display.json', dict(url='http://127.0.0.1:17890/api/v1/snapshot', read_token=config['read_token']))
print('Local configuration ready; credentials remain in ignored config files.')
