"""Explicit RTSS configuration action, with local backup and SDK visibility control."""
import argparse
import ctypes
import datetime
import json
import re
import shutil
from pathlib import Path

root=Path(__file__).resolve().parents[1]
base=Path('C:/Program Files (x86)/RivaTuner Statistics Server')
profiles=base/'Profiles'
parser=argparse.ArgumentParser()
parser.add_argument('--restore',type=Path)
args=parser.parse_args()
library=ctypes.CDLL(str(base/'RTSSHooks64.dll'))
flags=library.SetFlags
flags.argtypes=[ctypes.c_uint32,ctypes.c_uint32]
flags.restype=ctypes.c_uint32

def setting(data,section,key,value):
    pattern=rb'(\['+section.encode()+rb'\]\r?\n)(.*?)(?=\r?\n\[|\Z)'
    match=re.search(pattern,data,re.S)
    if not match:raise ValueError('Missing section '+section)
    content=match[2]
    line=rb'(?m)^'+key.encode()+rb'\s*=.*?(?=\r?$)'
    replacement=key.encode()+b'='+value.encode()
    content,n=re.subn(line,lambda _:replacement,content)
    if n==0:content+=b'\r\n'+replacement+b'\r\n'
    return data[:match.start(2)]+content+data[match.end(2):]

if args.restore:
    backup=args.restore.resolve()
    backup.relative_to((root/'artifacts/RTSS-backups').resolve())
    metadata=json.loads((backup/'metadata.json').read_text())
    for name in ('Global','Config'):
        if not (backup/name).is_file():raise ValueError('Incomplete backup')
    for name in ('Global','Config'):shutil.copy2(backup/name,profiles/name)
    library.UpdateProfiles()
    flags(0,metadata['runtime_flags_before'])
    print('RTSS profile files and SDK visibility flags restored.')
else:
    backup=root/'artifacts/RTSS-backups'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup.mkdir(parents=True)
    original={name:(profiles/name).read_bytes() for name in ('Global','Config')}
    for name in original:shutil.copy2(profiles/name,backup/name)
    before=flags(0xffffffff,0)
    (backup/'metadata.json').write_text(json.dumps(dict(runtime_flags_before=before),indent=2))
    global_data=original['Global']
    for section,key,value in [('OSD','EnableOSD','1'),('OSD','EnableStat','0'),('OSD','EnableFrametimeHistory','0'),('OSD','EnableFrameColorBar','0'),('Framerate','Limit','0'),('Framerate','SyncScanline0','0'),('Framerate','SyncScanline1','0'),('Framerate','SyncPeriods','0')]:
        global_data=setting(global_data,section,key,value)
    config_data=setting(original['Config'],'Settings','EnableEncoderServer','0')
    config_data=setting(config_data,'Shared','Flags',f'{before & ~1:08X}')
    written=[]
    try:
        (profiles/'Global').write_bytes(global_data);written.append('Global')
        (profiles/'Config').write_bytes(config_data);written.append('Config')
        library.UpdateProfiles()
        after=flags(0xfffffffe,0)
        assert not (flags(0xffffffff,0)&1)
    except Exception:
        for name in written:(profiles/name).write_bytes(original[name])
        flags(0,before)
        raise
    print('OSD globally hidden; encoder, own statistics, limiter and scanline sync disabled.')
    print('Capture support preserved. Backup: '+str(backup))
