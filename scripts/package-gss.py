"""Package the built GSS firmware and its verified original recovery image. No serial access."""
from pathlib import Path
import hashlib,json,shutil,re,csv
root=Path(__file__).resolve().parents[1];out=root/'artifacts/GSS';out.mkdir(exist_ok=True)
assert '#define GSO_GSS_BENCHMARK' not in (root/'main/gss_bench_config.h').read_text(), 'Refusing to package benchmark firmware'
version=re.search(r'#define GSO_VERSION "([^"]+)"',(root/'main/gss_build_info.h').read_text()).group(1)
image=(root/'build/gso_status_screen.bin').read_bytes()
assert image[32:36]==bytes.fromhex('3254cdab'),'Missing ESP app descriptor'
built_version=image[48:80].split(b'\0',1)[0].decode('ascii')
assert version==built_version+'-gss','Source version differs from built firmware; rebuild before packaging'
flash=json.loads((root/'build/flasher_args.json').read_text())
files=[]
for address,source in flash['flash_files'].items():
 name=f'GSO-STATUS-{version}.bin' if source==flash['app']['file'] else Path(source).name
 files.append((address,source,name))
apps=[row for row in csv.reader(line for line in (root/'partitions.csv').read_text(encoding='utf-8').splitlines() if line.strip() and not line.lstrip().startswith('#')) if row[1].strip()=='app']
assert len(apps)==1 and apps[0][2].strip()=='factory'
app_address=int(apps[0][3].strip(),0);app_size=int(apps[0][4].strip(),0)
assert app_address==int(flash['app']['offset'],0) and app_address%0x10000==0
assert app_address+app_size==16*1024*1024, 'Factory app must use the remaining flash'
entries=[]
for address,source,name in files:
 shutil.copy2(root/'build'/source,out/name);data=(out/name).read_bytes();entries.append({'address':address,'file':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
shutil.copy2(root/'build/gso_status_screen.elf',out/f'GSO-STATUS-{version}.elf')
backup=root/'esp32p4_aibox_r3_backup.bin';assert backup.stat().st_size==16777216 and hashlib.sha256(backup.read_bytes()).hexdigest()=='1203ee2cd0248549783611c0265a42774ba6555b7edcf2417fa9f9460b6a23a0'
shutil.copy2(backup,out/backup.name)
manifest={'product':'GSO STATUS SCREEN','short_name':'GSS','version':version,'chip':'esp32p4','board':'AIBOX R3, P4 v3.2','port':'COM5','flash_mode':'dio','flash_freq':'80m','flash_size':'16MB','partition_layout':f'single factory at {app_address:#x}; no OTA','private_config_partition':'gsn_config at 0x10000/0x6000; not included in public firmware files','application_partition_bytes':app_size,'remaining_application_bytes':app_size-(root/'build'/flash['app']['file']).stat().st_size,'files':entries,'original_backup_sha256':hashlib.sha256(backup.read_bytes()).hexdigest(),'wireless':'ESP-Hosted C5 SDIO; direct GSN HTTP snapshots and HOME; private credentials in separate NVS'}
(out/'flash-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8');print('Packaged GSS '+version+', spare='+str(manifest['remaining_application_bytes']))
