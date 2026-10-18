"""Generate private board NVS from local NAS config. Never package this image."""
import argparse,csv,json,pathlib,subprocess,sys,urllib.parse
p=argparse.ArgumentParser();p.add_argument('--config',default='config/display-nas.json');p.add_argument('--output',default='work/private-gss-network');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1];config=json.loads((root/a.config).read_text(encoding='utf-8-sig'))
url=urllib.parse.urlsplit(config['url']);assert url.scheme in ('http','https') and url.hostname and not url.username and not url.password and url.path=='/api/v1/snapshot' and not url.query
base=urllib.parse.urlunsplit((url.scheme,url.netloc,'','',''));assert len(base.encode())<256
read=config['read_token'];control=config.get('home_control_token','');assert 32<=len(read)<=256 and (not control or 32<=len(control)<=256)
out=root/a.output;out.mkdir(parents=True,exist_ok=True);table=out/'network.csv'
try:
 with table.open('w',newline='',encoding='utf-8') as f:
  writer=csv.writer(f);writer.writerow(['key','type','encoding','value']);writer.writerow(['gso_net','namespace','',''])
  for key,value in [('base_url',base),('read_token',read),('control_token',control),('mihomo_url',config.get('mihomo_url','')),('mihomo_token',config.get('mihomo_token',''))]:writer.writerow([key,'data','string',value])
 import os
 generator=pathlib.Path(os.environ['IDF_PATH'])/'components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py'
 with (out/'generate.log').open('w') as log:subprocess.run([sys.executable,str(generator),'generate',str(table),str(out/'network.bin'),'0x6000'],stdout=log,stderr=subprocess.STDOUT,check=True)
finally:
 table.unlink(missing_ok=True)
print('Private GSN NVS image generated; flash only at 0x10000. Tokens omitted.')
