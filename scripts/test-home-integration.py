"""Exercise built GSN + native/LVGL clients + loopback HA, without real household access."""
import datetime
import hashlib
import json
import re
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
work=ROOT/'work/qa-home-full'
work.mkdir(parents=True,exist_ok=True)
gate=threading.Lock()
states={'light.test':'off','climate.test':'cool','cover.left':'closed','cover.right':'closed'}
positions={'cover.left':0,'cover.right':0}
calls=[]
light_brightness=204
light_kelvin=4000

class HA(BaseHTTPRequestHandler):
    def do_GET(self):
        entity=self.path.removeprefix('/api/states/')
        if entity not in states or self.headers.get('Authorization')!='Bearer '+'h'*32:
            self.send_response(404);self.end_headers();return
        with gate:
            attrs={'friendly_name':entity,'supported_features':0}
            if entity.startswith('climate.'):
                attrs.update(supported_features=385,temperature_unit='°C',temperature=25,current_temperature=None,min_temp=16,max_temp=30,target_temp_step=1,hvac_modes=['cool','off'])
            if entity.startswith('light.'):attrs.update(supported_color_modes=['color_temp'],brightness=light_brightness,color_temp_kelvin=light_kelvin,min_color_temp_kelvin=2700,max_color_temp_kelvin=6500)
            if entity.startswith('cover.'):attrs.update(supported_features=11,current_position=positions[entity])
            body=json.dumps(dict(state=states[entity],attributes=attrs,last_updated=datetime.datetime.now(datetime.timezone.utc).isoformat())).encode()
        self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
    def do_POST(self):
        global light_brightness,light_kelvin
        assert self.headers.get('Authorization')=='Bearer '+'h'*32
        target=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        entity=target['entity_id'];action=self.path.split('/')[-1]
        assert entity in states
        with gate:
            calls.append((entity,action))
            if action=='turn_on':
                states[entity]='on'
                if 'brightness_pct' in target:light_brightness=round(target['brightness_pct']*255/100)
                if 'color_temp_kelvin' in target:light_kelvin=target['color_temp_kelvin']
            elif action=='turn_off':states[entity]='off'
            elif action=='set_hvac_mode':states[entity]=target['hvac_mode']
            elif action=='open_cover':states[entity]='opening';positions[entity]=40
            elif action=='stop_cover':states[entity]='open'
            elif action=='close_cover':states[entity]='closed';positions[entity]=0
            else:raise AssertionError('unexpected test action')
        self.send_response(200);self.send_header('Content-Length','2');self.end_headers();self.wfile.write(b'[]')
    def log_message(self,*_):pass

ha=ThreadingHTTPServer(('127.0.0.1',0),HA);ha.daemon_threads=True
thread=threading.Thread(target=ha.serve_forever,daemon=True);thread.start()
with socket.socket() as reservation:
    reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
config=dict(listen=f'127.0.0.1:{port}',agent_id='desktop',write_token='w'*32,read_token='r'*32,home_control_token='c'*32,
            ha=dict(base_url=f'http://127.0.0.1:{ha.server_port}',token='h'*32,home=[dict(entity_id=e) for e in states]))
config_path=work/'gsn.json';config_path.write_text(json.dumps(config),encoding='utf-8')
profile=work/'display.json';profile.write_text(json.dumps(dict(url=f'http://127.0.0.1:{port}/api/v1/snapshot',read_token='r'*32,home_control_token='c'*32)),encoding='utf-8')
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
process=None
def get_snapshot():
    request=urllib.request.Request(f'http://127.0.0.1:{port}/api/v1/snapshot',headers={'Authorization':'Bearer '+'r'*32})
    with opener.open(request,timeout=1) as response:return json.load(response)
def launch():
    process=subprocess.Popen([str(ROOT/'artifacts/GSN/GSN.exe'),'--config',str(config_path)],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
    for _ in range(100):
        if process.poll() is not None:raise AssertionError('built GSN failed to start')
        try:
            snapshot=get_snapshot()
            if snapshot['home']['source']['quality']=='ok':return process,snapshot
        except (OSError,ValueError):pass
        time.sleep(.05)
    process.terminate();process.wait(timeout=3);raise AssertionError('HOME source did not become ready')
def native(mode):
    subprocess.run([str(ROOT/'work/home-client-test.exe'),str(profile),mode],check=True,timeout=18)
def click(mode):
    subprocess.run([str(ROOT/'artifacts/GSS-Test.exe'),mode,'--config',str(profile)],check=True,timeout=12)
try:
    process,snapshot=launch()
    assert snapshot['windows']['source']['quality']=='unconfigured'
    climate=snapshot['home']['entities'][1]
    assert climate['current_temperature_c']['value'] is None and climate['target_temperature_c']['value']==25
    native('confirmed');click('--home-click-test')
    native('stop');click('--home-cover-test')
    with gate:
        assert calls==[('light.test','turn_on'),('light.test','turn_off'),('cover.left','open_cover'),('cover.left','stop_cover'),('cover.left','open_cover'),('cover.left','close_cover')],calls
        before=len(calls)
    click('--home-slider-test');click('--home-mode-test')
    with gate:
        assert calls[-2:]==[('light.test','turn_on'),('climate.test','set_hvac_mode')],calls
        before=len(calls)
    old_epoch=get_snapshot()['home']['control_epoch']
    process.terminate();process.wait(timeout=3);process,snapshot=launch()
    assert old_epoch!=snapshot['home']['control_epoch']
    body=dict(request_id='00000000-0000-4000-8000-000000000099',control_epoch=old_epoch,entity_id='light.test',action='turn_on',expires_at=(datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(seconds=10)).isoformat())
    request=urllib.request.Request(f'http://127.0.0.1:{port}/api/v1/home/commands',data=json.dumps(body).encode(),headers={'Authorization':'Bearer '+'c'*32,'Content-Type':'application/json'})
    try:opener.open(request,timeout=1);raise AssertionError('old boot command was accepted')
    except urllib.error.HTTPError as error:assert error.code==409
    with gate:assert len(calls)==before
    report=dict(scope='Built Windows GSN, native WinHTTP and LVGL clients, loopback HA only; no real household action',
                gsn_version=re.search(r'gso-status-nas:([0-9.]+)-',(ROOT/'scripts/package-gsn.py').read_text(encoding='utf-8')).group(1),display_version=re.search(r'#define GSO_VERSION "([^"]+)"',(ROOT/'simulator/build_info.h').read_text(encoding='utf-8')).group(1),gsn_sha256=hashlib.sha256((ROOT/'artifacts/GSN/GSN.exe').read_bytes()).hexdigest(),
                no_windows_agent_required=True,missing_room_temperature_preserved=True,native_light_and_stop=True,lvgl_double_touch_and_cover_switch=True,lvgl_slider_release_and_mode_selection=True,
                service_calls=before,old_epoch_replay_rejected=True,real_household_actions=0,passed=True)
    output=ROOT/'artifacts/verification/home-current.json';output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))
finally:
    if process is not None and process.poll() is None:process.terminate();process.wait(timeout=3)
    ha.shutdown();ha.server_close();thread.join()
