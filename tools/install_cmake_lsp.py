#!/usr/bin/env python3
"""Install the pinned upstream CMake language server in the project tools cache."""
import hashlib
import io
from pathlib import Path
import platform
import tarfile
import urllib.request

VERSION = '0.12.0'
RELEASES = {
    'x86_64': ('x86_64-unknown-linux-gnu', 'fae7bf87e114b0748fef5aa8c464b8413a2022925a2a61821bae5e2a9a7fdda2'),
    'aarch64': ('aarch64-unknown-linux-gnu', '4a2e989b0c2de01452955ac4f7a5de8a82eda5ca2501d3d40272886f3a3726c8'),
}


def install():
    if platform.system() != 'Linux' or platform.machine() not in RELEASES:
        raise RuntimeError('Install neocmakelsp on PATH for this platform.')
    architecture, digest = RELEASES[platform.machine()]
    url = f'https://github.com/neocmakelsp/neocmakelsp/releases/download/v{VERSION}/neocmakelsp-{architecture}.tar.gz'
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != digest:
        raise RuntimeError('The CMake language server download failed SHA-256 verification.')
    target = Path(__file__).resolve().parents[1] / '.cache/tools/neocmakelsp'
    target.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        binaries = [m for m in archive.getmembers() if m.isfile() and Path(m.name).name == 'neocmakelsp']
        if len(binaries) != 1:
            raise RuntimeError('Unexpected upstream release archive.')
        temporary = target.with_suffix('.tmp')
        temporary.write_bytes(archive.extractfile(binaries[0]).read())
    temporary.chmod(0o755)
    temporary.replace(target)
    print(f'Installed neocmakelsp {VERSION}: {target}')


if __name__ == '__main__':
    install()
