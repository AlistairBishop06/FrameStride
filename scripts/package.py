#!/usr/bin/env python3
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tarfile
import tempfile


REQUIRED = {
    'install.sh', 'uninstall.sh', 'update.sh', 'install.ps1', 'test-on-frame.sh',
    'README.md', 'LICENSE', 'docs/INPUT.md',
    'licenses/openvr-LICENSE', 'licenses/openxr-LICENSE', 'licenses/json-LICENSE',
    'licenses/Inter-LICENSE', 'licenses/stb-LICENSE',
    'payload/framestride', 'payload/libframestride_openxr.so', 'payload/framestride.vrmanifest',
    'payload/default-settings.json', 'payload/assets/icon.png', 'payload/assets/InterVariable.ttf',
    'payload/probe/actions.json', 'payload/probe/treadmill-binding.json',
    'payload/probe/standard-locomotion-template.json',
    'payload/driver/framestride/driver.vrdrivermanifest',
    'payload/driver/framestride/resources/input/framestride_profile.json',
    'payload/driver/framestride/resources/icons/framestride.svg',
    'payload/driver/framestride/bin/linuxarm64/driver_framestride.so',
}
EXECUTABLE = {'install.sh', 'uninstall.sh', 'update.sh', 'test-on-frame.sh', 'payload/framestride'}
ARM64 = {name for name in REQUIRED if name.endswith('.so') or name == 'payload/framestride'}


def check(root):
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
    if actual != REQUIRED:
        raise ValueError(f'Package contents differ. Missing: {sorted(REQUIRED - actual)}. Extra: {sorted(actual - REQUIRED)}')
    for name in actual:
        path = root / name
        if path.is_symlink():
            raise ValueError(f'Symlink in package: {name}')
        if name in ARM64:
            with path.open('rb') as file:
                header = file.read(20)
            if header[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', header, 18)[0] != 183:
                raise ValueError(f'Expected 64-bit little-endian AArch64 ELF: {name}')
        if name.endswith(('.json', '.vrmanifest', '.vrdrivermanifest')):
            json.loads(path.read_text(encoding='utf8'))
        if name.endswith('.sh') and not path.read_bytes().startswith(b'#!/usr/bin/env bash\n'):
            raise ValueError(f'Expected Bash script with LF line endings: {name}')


def main():
    parser = argparse.ArgumentParser(description='Build and verify the FrameStride Linux ARM64 release.')
    parser.add_argument('stage', type=Path)
    parser.add_argument('distribution', type=Path)
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1].joinpath('CMakeLists.txt').read_text(encoding='utf8')
    version = re.search(r'project\(FrameStride VERSION ([0-9.]+) ', project).group(1)
    rootname = f'framestride-{version}-linux-aarch64'
    args.distribution.mkdir(parents=True, exist_ok=True)
    archive = args.distribution / f'{rootname}.tar.gz'
    temporary = archive.with_suffix('.gz.tmp')
    check(args.stage)
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', '0'))
    try:
        with temporary.open('wb') as binary, gzip.GzipFile(filename='', fileobj=binary, mode='wb', mtime=epoch) as zipped:
            with tarfile.open(fileobj=zipped, mode='w', format=tarfile.USTAR_FORMAT) as tar:
                directories = {Path('.')}
                for name in REQUIRED:
                    directories.update(Path(name).parents)
                for directory in sorted(directories):
                    item = tarfile.TarInfo((Path(rootname) / directory).as_posix())
                    item.type, item.mode, item.mtime = tarfile.DIRTYPE, 0o755, epoch
                    tar.addfile(item)
                for name in sorted(REQUIRED):
                    item = tarfile.TarInfo(f'{rootname}/{name}')
                    item.size = (args.stage / name).stat().st_size
                    item.mode, item.mtime = (0o755 if name in EXECUTABLE else 0o644), epoch
                    with (args.stage / name).open('rb') as file:
                        tar.addfile(item, file)
        with tempfile.TemporaryDirectory(prefix='framestride-release-') as directory:
            with tarfile.open(temporary, 'r:gz') as tar:
                tar.extractall(directory, filter='data')
            extracted = Path(directory) / rootname
            check(extracted)
            for name in REQUIRED:
                if (extracted / name).read_bytes() != (args.stage / name).read_bytes():
                    raise ValueError(f'Extraction mismatch: {name}')
                if bool((extracted / name).stat().st_mode & 0o111) != (name in EXECUTABLE):
                    raise ValueError(f'Extraction permission mismatch: {name}')
        temporary.replace(archive)
    finally:
        temporary.unlink(missing_ok=True)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (args.distribution / 'SHA256SUMS').write_text(f'{digest}  {archive.name}\n', encoding='ascii')
    print(f'{archive}  {digest}')


if __name__ == '__main__':
    main()
