"""Exercise the real WinHTTP worker against isolated, controlled HTTP responses."""
import json
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path

root=Path(__file__).resolve().parents[1]
fixture=(root/'contracts/v1/snapshot-offline.json').read_bytes()
for mode in ('retention','deadline'):
    class Handler(BaseHTTPRequestHandler):
        count=0
        def do_GET(self):
            Handler.count+=1
            if mode=='deadline':time.sleep(2)
            body=fixture if Handler.count==1 and mode=='retention' else (b'{' if Handler.count==2 else b'x'*32769)
            try:
                self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            except (ConnectionError,OSError):pass
        def log_message(self,*_):pass
    server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
    server.daemon_threads=True
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    config=root/'work/network-client-test.json'
    config.write_text(json.dumps(dict(url=f'http://127.0.0.1:{server.server_port}/api/v1/snapshot',read_token='t'*32)),encoding='utf-8')
    try:subprocess.run([str(root/'work/network-client-test.exe'),str(config),mode],check=True,timeout=9)
    finally:server.shutdown();server.server_close();thread.join()
