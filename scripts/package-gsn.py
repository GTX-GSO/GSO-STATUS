"""Build Docker-load archives from static Linux binaries; never connects to Docker/NAS."""
import hashlib
import io
import json
import tarfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
output = root / 'artifacts/GSN'

def encoded(value):
    return json.dumps(value, separators=(',', ':'), sort_keys=True).encode()

def add(archive, name, data, mode=0o644):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    archive.addfile(info, io.BytesIO(data))

for architecture in ('arm64', 'armv7'):
    binary = output / ('GSN-linux-' + architecture)
    layer = io.BytesIO()
    with tarfile.open(fileobj=layer, mode='w') as archive:
        add(archive, 'app/gsn', binary.read_bytes(), 0o755)
        add(archive, 'etc/ssl/certs/ca-certificates.crt', (root/'vendor/cacert/cacert.pem').read_bytes())
        add(archive, 'app/LICENSES/Go-LICENSE.txt', (root/'vendor/licenses/Go-LICENSE.txt').read_bytes())
        add(archive, 'app/LICENSES/coder-websocket-LICENSE.txt', (root/'vendor/github.com/coder/websocket/LICENSE.txt').read_bytes())
        add(archive, 'app/LICENSES/MPL-2.0.txt', (root/'vendor/licenses/MPL-2.0.txt').read_bytes())
    layer_data = layer.getvalue()
    digest = hashlib.sha256(layer_data).hexdigest()
    config = dict(architecture='arm64' if architecture == 'arm64' else 'arm', os='linux',
        config=dict(User='0', WorkingDir='/app',
            Entrypoint=['/app/gsn'], Cmd=['--config', '/app/config/gsn.json'],
            ExposedPorts={'2780/tcp': {}}, Env=['SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt']),
        rootfs=dict(type='layers', diff_ids=['sha256:' + digest]))
    if architecture == 'armv7':
        config['variant'] = 'v7'
    config_data = encoded(config)
    config_name = hashlib.sha256(config_data).hexdigest() + '.json'
    layer_name = digest + '/layer.tar'
    manifest = [dict(Config=config_name, RepoTags=['gso-status-nas:0.1.33-' + architecture], Layers=[layer_name])]
    target = output / ('GSN-0.1.33-' + architecture + '.docker.tar')
    with tarfile.open(target, mode='w') as archive:
        add(archive, config_name, config_data)
        add(archive, layer_name, layer_data)
        add(archive, 'manifest.json', encoded(manifest))
    with tarfile.open(target) as archive:
        assert json.load(archive.extractfile('manifest.json')) == manifest
        assert hashlib.sha256(archive.extractfile(config_name).read()).hexdigest() + '.json' == config_name
        assert hashlib.sha256(archive.extractfile(layer_name).read()).hexdigest() == digest
    print(target.relative_to(root), hashlib.sha256(target.read_bytes()).hexdigest())
