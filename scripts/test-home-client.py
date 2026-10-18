"""Controlled loopback HOME service; never uses private config or contacts household HA."""
import copy
import datetime
import json
import socket
import subprocess
import threading
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
fixture=json.loads((ROOT/'contracts/v1/snapshot-home.json').read_text(encoding='utf-8'))
output=ROOT/'work/qa-home'
output.mkdir(parents=True,exist_ok=True)

for mode in ('confirmed','rejected','redirect','lost-response','restart','wrong-command','stop'):
    shared={'post_count':0,'actions':[],'leak_count':0,'command':None,'epoch':fixture['home']['control_epoch']}
    gate=threading.Lock()
    class Handler(BaseHTTPRequestHandler):
        def send_json(self,data,status=200):
            raw=json.dumps(data,ensure_ascii=False).encode()
            self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers()
            try:self.wfile.write(raw)
            except OSError:pass
        def do_GET(self):
            if self.headers.get('Authorization')!='Bearer '+'r'*32:
                self.send_json({},401);return
            with gate:
                if self.path=='/api/v1/snapshot':
                    value=copy.deepcopy(fixture);value['generated_at']=datetime.datetime.now(datetime.timezone.utc).isoformat();value['home']['control_epoch']=shared['epoch'];self.send_json(value);return
                if self.path=='/leak':shared['leak_count']+=1
                command=shared['command']
                if not command or mode=='restart':self.send_json({},404);return
                self.send_json({'command_id':command['request_id'] if mode!='wrong-command' else '00000000-0000-4000-8000-999999999999','entity_id':command['entity_id'],'action':command['action'],'status':'sent' if mode=='stop' and command['action']=='open_cover' else 'confirmed'})
        def do_POST(self):
            assert self.path=='/api/v1/home/commands'
            assert self.headers.get('Authorization')=='Bearer '+'c'*32
            size=int(self.headers['Content-Length']);assert size<=2048
            command=json.loads(self.rfile.read(size));assert command['entity_id']==('cover.left' if mode=='stop' else 'light.test');assert command['action'] in (('open_cover','stop_cover') if mode=='stop' else ('turn_off',) if shared.get('ui_toggle') else ('turn_on',));assert command['control_epoch']==fixture['home']['control_epoch']
            expiry=datetime.datetime.fromisoformat(command['expires_at'].replace('Z','+00:00'));assert 0<(expiry-datetime.datetime.now(datetime.timezone.utc)).total_seconds()<=(29 if command['action']=='open_cover' else 11)
            with gate:
                shared['post_count']+=1;shared['actions'].append(command['action']);shared['command']=command
                if mode=='restart':shared['epoch']='87654321-4321-4321-8321-210987654321'
            if mode=='rejected':self.send_json({},401);return
            if mode=='redirect':self.send_response(302);self.send_header('Location','/leak');self.end_headers();return
            if mode=='lost-response':self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
            self.send_json({'command_id':command['request_id'],'entity_id':command['entity_id'],'action':command['action'],'status':'queued'},202)
        def log_message(self,*_):pass
    server=ThreadingHTTPServer(('127.0.0.1',0),Handler);server.daemon_threads=True
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    profile=dict(url=f'http://127.0.0.1:{server.server_port}/api/v1/snapshot',read_token='r'*32,home_control_token='c'*32)
    config=output/'profile.json';config.write_text(json.dumps(profile),encoding='utf-8')
    try:
        expected='stop' if mode=='stop' else 'confirmed' if mode in ('confirmed','lost-response') else 'rejected' if mode in ('rejected','redirect') else 'unknown'
        subprocess.run([str(ROOT/'work/home-client-test.exe'),str(config),expected],check=True,timeout=22)
        assert shared['post_count']==(2 if mode=='stop' else 1),(mode,shared['post_count'])
        if mode=='stop':
            assert shared['actions']==['open_cover','stop_cover']
        assert shared['leak_count']==0
        if mode=='confirmed' and (ROOT/'artifacts/GSS-Test.exe').exists():
            with gate:shared['post_count']=0;shared['ui_toggle']=True
            subprocess.run([str(ROOT/'artifacts/GSS-Test.exe'),'--home-click-test','--config',str(config)],check=True,timeout=12)
            assert shared['post_count']==1,'double touch queued duplicate actions'
            for name,args in [('detail',['--capture-detail','15']),('summary',['--capture-page','1'])]:
                subprocess.run([str(ROOT/'artifacts/GSS-Test.exe'),'--capture-file',str(output/(name+'.bmp')),*args,'--config',str(config)],check=True,timeout=12)
            assert shared['post_count']==1,'visual capture triggered a household action'
            for bad in ('same-token','userinfo','query'):
                invalid=dict(profile)
                if bad=='same-token':invalid['home_control_token']=invalid['read_token']
                elif bad=='userinfo':invalid['url']=invalid['url'].replace('http://','http://user:pass@')
                else:invalid['url']+='?token=bad'
                invalid_config=output/'invalid-profile.json';invalid_config.write_text(json.dumps(invalid),encoding='utf-8')
                subprocess.run([str(ROOT/'work/home-client-test.exe'),str(invalid_config),'read-only' if bad=='same-token' else 'invalid'],check=True,timeout=3)
        print('PASS:',mode,'bounded POST count, no redirect leak, identity matched',flush=True)
    finally:server.shutdown();server.server_close();thread.join()
