#!/usr/bin/env python3
"""Build a bounded binary index for the native read-only RomFS backend, and the image's size and hashes the runtime checks
it against: build/game-data/<version>/romfs-config.txt, read at start (runtime/game_data.h).
The on-device setup writes the same files (android/setup/romfs_index.c)."""
import argparse,hashlib,json,struct
from pathlib import Path
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--version', choices=('v0','v112','v121','v143'), default='v0')
parser.add_argument('--out', type=Path, help='game data folder (default build/game-data/<version>)')
args=parser.parse_args();version=args.version
out=args.out or root/f'build/game-data/{version}';out.mkdir(parents=True,exist_ok=True)
folder=root/f'extracted/{version}/romfs';manifest=json.loads((folder/'index.json').read_text())
assert manifest['status']=='pass' and (folder/'romfs.bin').stat().st_size==manifest['romfs_size']
strings=bytearray();rows=bytearray()
for item in manifest['files']:
    name=item['path'].encode()+b'\0';rows.extend(struct.pack('<QQII',item['offset'],item['size'],len(strings),len(name)));strings.extend(name)
index=struct.pack('<8sQQ',b'TOTKRFS1',len(manifest['files']),24+len(rows))+rows+strings
(folder/'runtime-index.bin').write_bytes(index)
(out/'romfs-config.txt').write_text(f'ROMFS_IMAGE_SIZE {manifest["romfs_size"]}\nROMFS_IMAGE_SHA256 {manifest["romfs_sha256"]}\nROMFS_INDEX_SHA256 {hashlib.sha256(index).hexdigest()}\n')
print(json.dumps({'files':len(manifest['files']),'index_bytes':len(index)}))
