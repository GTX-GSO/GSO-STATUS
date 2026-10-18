"""Capture bounded, credential-free P4 network startup evidence over USB."""
import argparse,json,pathlib,re,time
import serial
p=argparse.ArgumentParser();p.add_argument('--port',default='COM4');p.add_argument('--seconds',type=int,default=50);p.add_argument('--output',required=True);p.add_argument('--require-gsn',action='store_true');a=p.parse_args()
out=pathlib.Path(a.output);out.mkdir(parents=True,exist_ok=True)
data=bytearray()
with serial.Serial(a.port,115200,timeout=.2) as port,(out/'runtime.log').open('wb') as log:
 end=time.monotonic()+a.seconds
 while time.monotonic()<end:
  part=port.read(8192)
  if part:log.write(part);log.flush();data.extend(part)
text=data.decode('utf-8','replace')
ips=re.findall(r'GOT_IP ([0-9.]+)',text)
reasons=re.findall(r'DISCONNECTED reason=(\d+)',text)
checks=re.findall(r'GATEWAY_CHECK sent=(\d+) received=(\d+)',text)
summary={'capture_seconds':a.seconds,'ip_addresses':ips,'disconnect_reasons':reasons,'gateway_checks':[{'sent':int(s),'received':int(r)} for s,r in checks],'ui_ready':'UI_READY' in text,'panic':'Guru Meditation' in text or 'panic_abort' in text,'last_state_connected':bool(ips) and text.rfind('GOT_IP')>text.rfind('DISCONNECTED'),'health_samples':text.count('HEALTH frames=')}
summary['snapshot_ok_counts']=[int(v) for v in re.findall(r'SNAPSHOT_OK count=(\d+)',text)]
summary['last_gsn_status']=int(re.findall(r'GSN_STATUS (\d+)',text)[-1]) if re.findall(r'GSN_STATUS (\d+)',text) else None
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
print(text[-7000:]);print(json.dumps(summary,indent=2))
if not summary['last_state_connected'] or summary['panic']:raise SystemExit(2)
if a.require_gsn and (not summary['snapshot_ok_counts'] or summary['last_gsn_status']!=1):raise SystemExit(3)
