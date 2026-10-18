"""Click all real LVGL segmented control against an isolated controller."""
import http.server,json,pathlib,subprocess,threading
root=pathlib.Path(__file__).resolve().parents[1];out=root/'work/clash-mode';out.mkdir(parents=True,exist_ok=True)
state={'mode':'rule','patches':[]}
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  assert self.path=='/configs' and self.headers['Authorization']=='Bearer test-secret'
  body=json.dumps({'mode':state['mode']}).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
 def do_PATCH(self):
  assert self.path=='/configs' and self.headers['Authorization']=='Bearer test-secret'
  mode=json.loads(self.rfile.read(int(self.headers['Content-Length'])))['mode'];assert mode in ('rule','global','direct');state['mode']=mode;state['patches'].append(mode);self.send_response(204);self.send_header('Content-Length','0');self.end_headers()
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
config=json.loads((root/'config/display-nas.json').read_text(encoding='utf-8-sig'));config['mihomo_url']=f'http://127.0.0.1:{server.server_port}';config['mihomo_token']='test-secret'
profile=out/'isolated-ui.private.json';profile.write_text(json.dumps(config))
try:
 subprocess.run([str(root/'artifacts/GSS-Test.exe'),'--clash-switch-test','--config',str(profile)],cwd=root,check=True,timeout=30,creationflags=subprocess.CREATE_NO_WINDOW)
 assert state['patches']==['global','direct','rule']*2,state['patches']
finally:
 profile.unlink(missing_ok=True);server.shutdown();server.server_close();thread.join()
print('PASS: main and detail segmented control clicks issue global/direct/rule once each; readback selects only one; real controller untouched')
