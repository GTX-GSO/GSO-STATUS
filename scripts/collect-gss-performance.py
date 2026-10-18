"""Collect the compile-time read-only page transition benchmark; no hardware commands."""
import argparse,json,re,time,shutil,hashlib
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('stage',choices=['baseline','xrgb-dma','optimized','rgb565']);p.add_argument('--log',required=True);args=p.parse_args();log=Path(args.log)
match=None
for _ in range(200):
 text=log.read_text(encoding='utf-8',errors='replace') if log.exists() else ''
 match=re.search(r'BENCH frames=(\d+) elapsed_ms=(\d+) render_avg_us=(\d+) ppa_avg_us=(\d+) wait_avg_us=(\d+) total_avg_us=(\d+) max_us=(\d+)',text)
 if match:break
 time.sleep(.2)
assert match,'benchmark did not complete'
report=dict(zip(['frames','elapsed_ms','render_avg_us','ppa_avg_us','wait_avg_us','total_avg_us','max_us'],map(int,match.groups())));report['stage']=args.stage;report['app_sha256']=hashlib.sha256((root/'build/gso_status_screen.bin').read_bytes()).hexdigest()
out=root/'work/gss-performance';out.mkdir(exist_ok=True);(out/(args.stage+'.json')).write_text(json.dumps(report,indent=2)+'\n');shutil.copy2(log,out/(args.stage+'.log'));print(json.dumps(report))
