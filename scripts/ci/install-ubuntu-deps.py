#!/usr/bin/env python3
"""Install Ubuntu CI dependencies through official HTTPS mirrors."""

import argparse
from pathlib import Path
import subprocess


def use_https_sources(apt_root):
    paths = [apt_root / 'sources.list', apt_root / 'apt-mirrors.txt']
    paths.extend((apt_root / 'sources.list.d').glob('*.list'))
    paths.extend((apt_root / 'sources.list.d').glob('*.sources'))
    replacements = {
        'http://azure.archive.ubuntu.com/ubuntu': 'https://archive.ubuntu.com/ubuntu',
        'https://azure.archive.ubuntu.com/ubuntu': 'https://archive.ubuntu.com/ubuntu',
        'http://archive.ubuntu.com/ubuntu': 'https://archive.ubuntu.com/ubuntu',
        'http://security.ubuntu.com/ubuntu': 'https://security.ubuntu.com/ubuntu',
        'http://ports.ubuntu.com/ubuntu-ports': 'https://ports.ubuntu.com/ubuntu-ports',
    }
    for path in paths:
        if not path.is_file():
            continue
        original = path.read_text(encoding='utf-8')
        updated = original
        for source, target in replacements.items():
            updated = updated.replace(source, target)
        if updated != original:
            path.write_text(updated, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('packages', nargs='+')
    args = parser.parse_args()
    use_https_sources(Path('/etc/apt'))
    apt = ['apt-get', '-o', 'Acquire::Retries=3',
           '-o', 'Acquire::http::Timeout=30', '-o', 'Acquire::https::Timeout=30']
    subprocess.run([*apt, 'update', '--error-on=any'], check=True)
    subprocess.run([*apt, 'install', '-y', *args.packages], check=True)


if __name__ == '__main__':
    main()
