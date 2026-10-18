"""Explicitly authorized reversible HOME checks; restore each touched device and verify it."""
import datetime, json, socket, subprocess, time, uuid, urllib.request, sys
if "--execute" not in sys.argv:raise SystemExit("Requires --execute for explicitly authorized reversible physical tests")
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
work=ROOT/('work/home-live-'+uuid.uuid4().hex);work.mkdir()
config=json.loads((ROOT/'artifacts/NAS/config/gsn.json').read_text(encoding='utf-8'))
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
config.update(listen=f'127.0.0.1:{port}',router_url='');config['mihomo']={};config.pop('docker',None)
(work/'gsn.json').write_text(json.dumps(config),encoding='utf-8')
base=f'http://127.0.0.1:{port}';ha=config['ha'];opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
process=None;touched={};originals={};outcomes=[];restores=[];failure=None
sections=set(sys.argv[sys.argv.index('--sections')+1].split(',')) if '--sections' in sys.argv else {'lights','climate','fan','purifier','dehumidifier','washer','curtains'}

def http(url,token,data=None,timeout=3):
 request=urllib.request.Request(url,data=None if data is None else json.dumps(data).encode(),headers={'Authorization':'Bearer '+token,'Content-Type':'application/json'})
 with opener.open(request,timeout=timeout) as response:
  raw=response.read(32769)
  if len(raw)>32768:raise RuntimeError('response exceeds 32 KiB')
  return json.loads(raw)
def state(entity):return http(ha['base_url']+'/api/states/'+entity,ha['token'])
def service(entity,action,**data):return http(ha['base_url']+'/api/services/'+entity.split('.')[0]+'/'+action,ha['token'],dict(entity_id=entity,**data),timeout=5)
def snap():return http(base+'/api/v1/snapshot',config['read_token'])
def command(entity,action,**data):
 snapshot=snap();identifier=str(uuid.uuid4());seconds=28 if entity.startswith('cover.') else 10
 body=dict(control_epoch=snapshot['home']['control_epoch'],request_id=identifier,entity_id=entity,action=action,expires_at=(datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(seconds=seconds)).isoformat(),**data)
 response=http(base+'/api/v1/home/commands',config['home_control_token'],body)
 until=time.monotonic()+seconds+2
 while response['status'] in ('queued','sent') and time.monotonic()<until:
  time.sleep(.25);response=http(base+'/api/v1/home/commands/'+identifier,config['read_token'])
 outcomes.append(dict(entity_id=entity,action=action,status=response['status']))
 print(json.dumps(dict(action=action,status=response['status'])),flush=True)
 if response['status']!='confirmed':
  target=next((e for e in snap()['home']['entities'] if e['entity_id']==entity),{})
  print(json.dumps(dict(observed_state=target.get('state'),source=target.get('source'),actual_state=state(entity)['state'] if not entity.startswith('cover.gso') else None)),flush=True)
  raise RuntimeError('unconfirmed '+entity+' '+action+' '+str(response.get('error_code')))
 time.sleep(.55)
def remember(entity):
 if entity not in touched:
  touched[entity]=state(entity);originals[entity]=touched[entity]
  (work/'restore-state.json').write_text(json.dumps(originals,ensure_ascii=False,indent=2),encoding='utf-8')
 return touched[entity]
def close(a,b,tolerance=.01):return a is not None and b is not None and abs(float(a)-float(b))<=tolerance

def matches(original,current):
 if current['state']!=original['state']:return False
 a=original['attributes'];b=current['attributes'];d=original['entity_id'].split('.')[0]
 keys={'light':['brightness','color_temp_kelvin'],'climate':['temperature'],'fan':['percentage','preset_mode'],'humidifier':['humidity','mode'],'cover':['current_position']}.get(d,[])
 for key in keys:
  if a.get(key) is None:continue
  if isinstance(a[key],(int,float)):
   if not close(a[key],b.get(key),1.01 if key=='percentage' else 1 if key in ('brightness','color_temp_kelvin') else .01):return False
  elif a[key]!=b.get(key):return False
 return True

def restore(entity):
 original=touched[entity];attrs=original['attributes'];d=entity.split('.')[0];now=state(entity)
 if not matches(original,now):
  if d=='light':
   data={k:attrs[k] for k in ('brightness','color_temp_kelvin') if attrs.get(k) is not None}
   if original['state']=='on' or data:service(entity,'turn_on',**data)
   if original['state']=='off':service(entity,'turn_off')
  elif d=='climate':
   if attrs.get('temperature') is not None:service(entity,'set_temperature',temperature=attrs['temperature'])
   service(entity,'set_hvac_mode',hvac_mode=original['state'])
  elif d=='fan':
   if attrs.get('percentage') is not None:service(entity,'set_percentage',percentage=round(attrs['percentage']))
   if attrs.get('preset_mode') is not None:service(entity,'set_preset_mode',preset_mode=attrs['preset_mode'])
   service(entity,'turn_off' if original['state']=='off' else 'turn_on')
  elif d=='humidifier':
   if attrs.get('humidity') is not None and now['attributes'].get('humidity')!=attrs['humidity']:service(entity,'set_humidity',humidity=attrs['humidity'])
   if attrs.get('mode') is not None and now['attributes'].get('mode')!=attrs['mode']:service(entity,'set_mode',mode=attrs['mode'])
   service(entity,'turn_off' if original['state']=='off' else 'turn_on')
  elif d=='cover':
   position=attrs.get('current_position')
   if position in (0,100):service(entity,'close_cover' if position==0 else 'open_cover')
   elif position is not None:service(entity,'set_cover_position',position=position)
   else:service(entity,'close_cover' if original['state']=='closed' else 'open_cover')
  else:service(entity,'turn_off' if original['state']=='off' else 'turn_on')
 deadline=time.monotonic()+35;stable=0
 while time.monotonic()<deadline:
  if matches(original,state(entity)):
   stable+=1
   if stable>=6:restores.append(dict(entity_id=entity,verified=True));del touched[entity];print(json.dumps(dict(restored=entity)),flush=True);return
  else:stable=0
  time.sleep(.5)
 raise RuntimeError('restore not confirmed: '+entity)

def run():
 global process
 process=subprocess.Popen([str(ROOT/'artifacts/GSN/GSN.exe'),'--config',str(work/'gsn.json')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
 deadline=time.monotonic()+8
 while time.monotonic()<deadline:
  try:
   s=snap()
   if len(s['home']['entities'])==13 and all(e['source']['quality']=='ok' for e in s['home']['entities']):break
  except (OSError,ValueError):pass
  time.sleep(.1)
 else:raise RuntimeError('live HOME sources not ready')
 print(json.dumps(dict(ready_cards=len(s['home']['entities']))),flush=True)
 for entity in (['light.lemesh_cn_1165200266_wy0c15_s_2_light','light.yeelink_cn_707607957_lamp22_s_2'] if 'lights' in sections else []):
  original=remember(entity);a=original['attributes']
  if original['state']=='on':
   if '1165200266' in entity:command(entity,'turn_off');command(entity,'turn_on')
   command(entity,'set_brightness',brightness_percent=max(10,a['brightness']*100/255-5))
   command(entity,'set_color_temperature',color_temperature_kelvin=4000 if a['color_temp_kelvin']!=4000 else 4500)
  restore(entity)
 for entity in (['climate.cuco_cn_769323959_cp6','climate.lumi_cn_582865511_mcn02'] if 'climate' in sections else []):
  original=remember(entity);a=original['attributes'];target=a['temperature']-1 if a['temperature']>a['min_temp'] else a['temperature']+1
  command(entity,'set_temperature',temperature_c=target);command(entity,'set_hvac_mode',mode=original['state']);restore(entity)
 for entity in ([e for kind,e in [('fan','fan.dmaker_cn_779495617_p45_s_2_fan'),('purifier','fan.zhimi_cn_392259420_ma4_s_2_air_purifier')] if kind in sections]):
  original=remember(entity);a=original['attributes'];step=a['percentage_step'];target=step if a['percentage']>step+1 else min(100,step*2)
  command(entity,'set_percentage',percentage=target)
  alternatives=[m for m in a['preset_modes'] if m!=a['preset_mode']]
  if alternatives:command(entity,'set_preset_mode',mode=alternatives[0])
  restore(entity)
 if 'dehumidifier' in sections:
  entity='humidifier.dmaker_cn_752982459_22l';original=remember(entity);a=original['attributes']
  # HA reports target humidity and mode as non-writable on this specific device.
  command(entity,'turn_off' if original['state']=='on' else 'turn_on')
  restore(entity)
 if 'washer' in sections:
  entity='switch.midjd8_cn_835517055_45_on_p_2_1';original=remember(entity)
  # Verify existing power state only; never start, pause or stop a wash program.
  command(entity,'turn_off' if original['state']=='off' else 'turn_on');restore(entity)
 if 'curtains' in sections:
  group=next(b for b in ha['home'] if b.get('members'))
  for member in group['members']:remember(member)
  command(group['entity_id'],'open_cover');command(group['entity_id'],'close_cover')
  for member in group['members']:restore(member)

try:run()
except Exception as error:failure=str(error);print(json.dumps(dict(failed=failure)),flush=True)
finally:
 if process is not None and process.poll() is None:process.terminate();process.wait(timeout=4)
 for entity in list(touched):
  try:restore(entity)
  except Exception as error:restores.append(dict(entity_id=entity,verified=False,error=str(error)))
 report=dict(scope='User-authorized physical HOME tests through isolated production GSN; HA restore and read-back verification',gsn_version='0.1.18',sections=sorted(sections),commands=outcomes,restoration=restores,remaining_unrestored=list(touched),failure=failure,passed=failure is None and not touched,washer_program_started=False,nas_container_changed=False)
 report_path=ROOT/'artifacts/verification/home-physical-0.1.18.json'
 if report_path.exists():
  prior=report_path.read_text(encoding='utf-8');(ROOT/'artifacts/verification'/('home-physical-0.1.18-attempt-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'.json')).write_text(prior,encoding='utf-8')
 report_path.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 print(json.dumps(dict(passed=report['passed'],confirmed_commands=sum(c['status']=='confirmed' for c in outcomes),restored_devices=sum(r['verified'] for r in restores),remaining_unrestored=list(touched))),flush=True)
 if touched:raise RuntimeError('Device restoration requires attention: '+','.join(touched))
 if failure:raise RuntimeError(failure)
