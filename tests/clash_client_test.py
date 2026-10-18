"""Exercise the real Windows client against an isolated Mihomo HTTP stub."""
from pathlib import Path
import http.server,json,socket,subprocess,threading,time
root=Path(__file__).resolve().parents[1];out=root/'work/clash-client-test';out.mkdir(exist_ok=True)
harness=out/'main.c'
harness.write_text(r'''
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clash_client.h"
#undef assert
#define assert(x) do{if(!(x)){fprintf(stderr,"check failed: %s\n",#x);return 5;}}while(0)
uint64_t desk_data_now_ms(void){return GetTickCount64();}
int main(int argc,char **argv){
 assert(argc>=4);assert(!desk_clash_start("http://127.0.0.1/path","test-secret"));
 assert(!desk_clash_start("http://user@127.0.0.1","test-secret"));
 assert(!desk_clash_start(argv[1],"bad\r\nheader"));
 assert(desk_clash_start(argv[1],"test-secret"));
 for(int n=0;n<100;n++){desk_clash_result r=desk_clash_latest();if(r.observed_ms||r.state==CLASH_UNAVAILABLE)break;Sleep(20);}
 for(int i=3;i<argc;i++){
  if(strcmp(argv[i],"-")){assert(!desk_clash_mode("invalid"));assert(desk_clash_mode(argv[i]));assert(!desk_clash_mode("global"));}
  desk_clash_result r={0};for(int n=0;n<250;n++){Sleep(20);r=desk_clash_latest();if(!r.busy&&r.state!=CLASH_IDLE)break;}
  if(r.busy||r.state!=atoi(argv[2])){fprintf(stderr,"Unexpected state=%d busy=%d\n",r.state,r.busy);return 3;}
  if(r.state==CLASH_CONFIRMED&&strcmp(argv[i],r.mode))return 4;
 }
 desk_clash_stop();assert(!desk_clash_latest().configured);return 0;
}
''',encoding='utf-8')
subprocess.run([str(root/'tools/zig/zig.exe'),'cc','-target','x86_64-windows-gnu','-std=c11','-O2','-I'+str(root/'simulator'),'-I'+str(root/'shared'),'-I'+str(root/'vendor/cjson'),str(harness),str(root/'simulator/clash_client.c'),str(root/'vendor/cjson/cJSON.c'),'-lwinhttp','-lkernel32','-o',str(out/'test.exe')],check=True)
for scenario,expected,modes in [('normal',2,['rule','global','direct']),('auth',4,['direct']),('unchanged',5,['direct']),('lost_response',2,['direct']),('redirect',5,['direct']),('invalid_read',3,['-'])]:
 state={'mode':'rule','patches':[],'paths':[]}
 class Handler(http.server.BaseHTTPRequestHandler):
  def log_message(self,*args):pass
  def do_GET(self):
   state['paths'].append(self.path);assert self.headers.get('Authorization')=='Bearer test-secret'
   body=json.dumps({'mode':'invalid' if scenario=='invalid_read' else state['mode']}).encode()
   self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
  def do_PATCH(self):
   time.sleep(.05)
   assert self.path=='/configs';assert self.headers.get('Authorization')=='Bearer test-secret';assert self.headers.get('Content-Type')=='application/json'
   value=json.loads(self.rfile.read(int(self.headers['Content-Length'])));assert set(value)=={'mode'};assert value['mode'] in ('rule','global','direct');state['patches'].append(value['mode'])
   if scenario=='auth':self.send_response(401)
   elif scenario=='redirect':self.send_response(307);self.send_header('Location','/leak')
   else:
    if scenario!='unchanged':state['mode']=value['mode']
    if scenario=='lost_response':self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
    self.send_response(204)
   self.send_header('Content-Length','0');self.end_headers()
 server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
 try:
  subprocess.run([str(out/'test.exe'),f'http://127.0.0.1:{server.server_port}',str(expected),*modes],check=True,timeout=12)
  assert state['patches']==[mode for mode in modes if mode!='-'],state
  assert '/leak' not in state['paths']
 finally:server.shutdown();server.server_close();thread.join()
 print('PASS:',scenario)
print('PASS: actual mode readback, mutual request guard, authentication errors, no PATCH replay, no redirects, invalid payload rejection')
