"""Read-only HA lux -> isolated new GSN -> C parser and LVGL settings. No device service calls."""
import json,secrets,socket,subprocess,tempfile,time,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
qa=ROOT/'work/qa';qa.mkdir(exist_ok=True,parents=True)
private=json.loads((ROOT/'artifacts/NAS/config/gsn.json').read_text(encoding='utf-8-sig'))
with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
with tempfile.TemporaryDirectory(dir=ROOT/'work',prefix='ambient-readonly-') as directory:
 folder=Path(directory);read=secrets.token_hex(32)
 config={'listen':f'127.0.0.1:{port}','agent_id':'ambient_test','read_token':read,'write_token':secrets.token_hex(32),'ha':{'base_url':private['ha']['base_url'],'token':private['ha']['token'],'entities':{'ambient_light':private['ha']['entities']['ambient_light']}}}
 cp=folder/'gsn.json';cp.write_text(json.dumps(config),encoding='utf-8')
 process=subprocess.Popen([str(ROOT/'artifacts/GSN/GSN.exe'),'--config',str(cp)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
 try:
  url=f'http://127.0.0.1:{port}/api/v1/snapshot';snapshot=None
  for attempt in range(80):
   try:
    snapshot=json.load(urllib.request.urlopen(urllib.request.Request(url,headers={'Authorization':'Bearer '+read}),timeout=1))
    if snapshot.get('ambient',{}).get('source',{}).get('quality')=='ok':break
   except OSError:pass
   time.sleep(.1)
  assert snapshot and snapshot['ambient']['source']['quality']=='ok','ambient never healthy'
  lux=snapshot['ambient']['illuminance_lux']['value'];assert isinstance(lux,(float,int)) and 0<=lux<=200000
  sp=folder/'read-only-input.json';sp.write_text(json.dumps(snapshot),encoding='utf-8')
  subprocess.run([str(ROOT/'work/snapshot-test.exe'),str(sp)],check=True,timeout=5,creationflags=subprocess.CREATE_NO_WINDOW)
  dp=folder/'display.json';dp.write_text(json.dumps({'url':url,'read_token':read}),encoding='utf-8')
  subprocess.run([str(ROOT/'artifacts/GSS-Test.exe'),'--config',str(dp),'--capture-settings','--capture-file',str(qa/'live-brightness.bmp')],check=True,timeout=12,creationflags=subprocess.CREATE_NO_WINDOW)
  result={'gsn':'0.1.19','display':'0.5.20-dev','ha_ambient_quality':'ok','illuminance_lux':lux,'source_updated_at':snapshot['ambient']['source']['source_updated_at'],'parser':'PASS','live_settings_capture':'PASS','ha_device_commands':0,'production_nas_container_changed':False}
  (ROOT/'artifacts/verification/ambient-0.1.19.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
 finally:
  process.terminate()
  try:process.wait(timeout=5)
  except subprocess.TimeoutExpired:process.kill();process.wait()
