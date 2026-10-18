"""Prepare ARM64 delivery and local client profiles; no NAS connection or deployment."""
import json
import secrets
import shutil
from pathlib import Path

root=Path(__file__).resolve().parents[1]
output=root/'artifacts/NAS'
(output/'config').mkdir(parents=True,exist_ok=True)
shutil.copy2(root/'artifacts/GSN/GSN-0.1.33-arm64.docker.tar',output/'GSN-0.1.33-arm64.docker.tar')
path=output/'config/gsn.json'
if not path.exists():
    config=dict(listen='0.0.0.0:2780',agent_id='desktop',write_token=secrets.token_hex(32),read_token=secrets.token_hex(32),router_url='http://192.168.124.1/mobile.asp',ha=dict(vpn_total_origin='user_confirmed:200GiB;reset_day=24',vpn_total_bytes=214748364800))
    path.write_text(json.dumps(config,indent=2)+'\n',encoding='utf-8')
config=json.loads(path.read_text(encoding='utf-8'))
local=json.loads((root/'config/gsw.json').read_text(encoding='utf-8-sig'))
local.update(SnapshotBaseUrl='http://192.168.124.14:2780',WriteToken=config['write_token'],AgentId=config['agent_id'])
(root/'config/gsw-nas.json').write_text(json.dumps(local,indent=2)+'\n',encoding='utf-8')
display_path=root/'config/display-nas.json'
display=json.loads(display_path.read_text(encoding='utf-8-sig')) if display_path.exists() else {}
display.update(url='http://192.168.124.14:2780/api/v1/snapshot',read_token=config['read_token'])
display_path.write_text(json.dumps(display,indent=2)+'\n',encoding='utf-8')
(root/'artifacts/GSW/gsw.json').write_text(json.dumps(local,indent=2)+'\n',encoding='utf-8')
print('ARM64 image, NAS configuration and matching client profiles prepared locally; nothing deployed.')
