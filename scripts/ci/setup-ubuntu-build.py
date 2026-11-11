#!/usr/bin/env python3
"""Use the hosted Ubuntu build tools and install a pinned ccache release when requested."""

import argparse
import hashlib
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile


REQUIRED_TOOLS = ('cmake', 'ninja', 'cc', 'c++', 'git', 'pkg-config')
CCACHE_VERSION = '4.14.3'
CCACHE_SHA256 = {
    'x86_64': 'd553fac006ed93a37342cee9228cf4e13dd66690fa1a8075c54c7bace9b72ae6',
    'aarch64': 'f13e56bdb02f62b6f08f54ac527a0a7674dff22ea607167f33621fe2f2ded37b',
}


def install_ccache():
    architecture = platform.machine()
    expected = CCACHE_SHA256[architecture]
    name = f'ccache-{CCACHE_VERSION}-linux-{architecture}-musl-static'
    url = f'https://github.com/ccache/ccache/releases/download/v{CCACHE_VERSION}/{name}.tar.xz'
    with tempfile.TemporaryDirectory(prefix='mradm-ccache-') as directory:
        archive = Path(directory) / 'ccache.tar.xz'
        subprocess.run(['curl', '--fail', '--location', '--retry', '3', '--retry-all-errors',
                        '--connect-timeout', '10', '--max-time', '120', '--output', str(archive), url], check=True)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
            raise ValueError('ccache archive SHA-256 mismatch')
        executable = Path(directory) / 'ccache'
        with tarfile.open(archive) as package:
            with package.extractfile(f'{name}/ccache') as binary:
                executable.write_bytes(binary.read())
        subprocess.run(['sudo', 'install', '-m', '755', str(executable), '/usr/local/bin/ccache'], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ccache', action='store_true')
    args = parser.parse_args()
    # The hosted x64 and ARM64 images already provide these tools. Check before any download.
    for tool in REQUIRED_TOOLS:
        location = shutil.which(tool)
        if location is None:
            parser.error(f'hosted runner is missing {tool}')
        print(f'{tool}: {location}', flush=True)
    if args.ccache:
        install_ccache()
        subprocess.run(['ccache', '--version'], check=True)


if __name__ == '__main__':
    main()
