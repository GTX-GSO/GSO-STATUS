"""Collect private development deliverables and credential-free verification evidence."""
import hashlib
import json
import shutil
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

root=Path(__file__).resolve().parents[1]
output=root/'artifacts'
verification=output/'verification'
verification.mkdir(exist_ok=True)
for name in ('local-integration-report.json','local-resource-report.json','rtss-settings-result.json'):
    source=root/'work'/name
    if source.exists():shutil.copy2(source,verification/name)
ui_report=root/'work/qa/selftest.txt'
if ui_report.exists():shutil.copy2(ui_report,verification/'ui-selftest.txt')
licenses=output/'GSW/licenses'
licenses.mkdir(exist_ok=True)
for name in ('LICENSE.txt','ThirdPartyNotices.txt'):shutil.copy2(root/'tools/dotnet'/name,licenses/('dotnet-'+name))
shutil.copy2(root/'tools/presentmon/LICENSE.txt',licenses/'PresentMon-LICENSE.txt')
shutil.copy2(root/'tools/presentmon/PresentMon-2.6.0-x64.exe',output/'GSW/PresentMon-2.6.0-x64.exe')
gsw_version=ET.parse(root/'gsw/GSW.csproj').getroot().findtext('.//Version')
private_bundle=output/f'GSO-STATUS-WIN-{gsw_version}-win-x64.zip'
shutil.copy2(root/'scripts/install-cpu-sensor.ps1',output/'GSW/install-cpu-sensor.ps1')
shutil.copy2(root/'scripts/install-gsw.ps1',output/'GSW/install-gsw.ps1')
shutil.copy2(root/'config/gsw.example.json',output/'GSW/gsw.example.json')
shutil.copy2(root/'tools/sensors/PawnIO_setup.exe',output/'GSW/PawnIO_setup.exe')
# The working gsw.json next to the build carries this machine's GSN write token; the bundle ships only the example.
with zipfile.ZipFile(private_bundle,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=1) as bundle:
    for path in (output/'GSW').rglob('*'):
        if path.is_file() and path.relative_to(output/'GSW').as_posix()!='gsw.json':bundle.write(path,path.relative_to(output/'GSW'))
with zipfile.ZipFile(private_bundle) as bundle:assert 'gsw.json' not in bundle.namelist(),'gsw.json must not be bundled'
manifest=[]
targets=['GSS-Simulator.exe','GSW/GSW.exe','GSW/PresentMon-2.6.0-x64.exe',private_bundle.name,'NAS/GSN-0.1.35-arm64.docker.tar']
gss_manifest=output/'GSS/flash-manifest.json'
if gss_manifest.exists():
    gss=json.loads(gss_manifest.read_text(encoding='utf-8'))
    for item in gss['files']:
        if item['file'].startswith('GSO-STATUS-') and item['file'].endswith('.bin'):targets.append('GSS/'+item['file'])
    release_version=gss['version'].removesuffix('-gss')
    for name in (f'GSS/GSO-STATUS-GSS-{release_version}.zip',f'GSS/GSO-GSS-{release_version}-verification.zip'):
        if (output/name).exists():targets.append(name)
for relative in targets:
    path=output/relative
    manifest.append(dict(file=relative,bytes=path.stat().st_size,sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
(output/'release-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
print('Private GSW bundle, ARM64 delivery and verification evidence ready.')
