#!/usr/bin/env python3
"""Offline checks for hosted Ubuntu build tools and the pinned ccache download."""

import hashlib
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('ubuntu_build', ROOT / 'scripts/ci/setup-ubuntu-build.py')
setup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup)


class UbuntuBuildTests(unittest.TestCase):
    def test_preinstalled_tools_need_no_download(self):
        with patch.object(setup.shutil, 'which', return_value='/usr/bin/tool') as which:
            with patch.object(setup.subprocess, 'run') as run:
                with patch('sys.argv', ['setup-ubuntu-build.py']):
                    setup.main()
        self.assertEqual([call.args[0] for call in which.call_args_list], list(setup.REQUIRED_TOOLS))
        run.assert_not_called()

    def test_missing_build_tool_fails_before_download(self):
        with patch.object(setup.shutil, 'which', return_value=None):
            with patch.object(setup.subprocess, 'run') as run:
                with patch('sys.argv', ['setup-ubuntu-build.py', '--ccache']):
                    with self.assertRaises(SystemExit):
                        setup.main()
        run.assert_not_called()

    def test_ccache_download_is_pinned_and_installs_only_the_checked_binary(self):
        for architecture in ('x86_64', 'aarch64'):
            with self.subTest(architecture=architecture), tempfile.TemporaryDirectory() as directory:
                archive = Path(directory) / 'fixture.tar.xz'
                name = f'ccache-{setup.CCACHE_VERSION}-linux-{architecture}-musl-static'
                payload = b'ccache executable fixture'
                with tarfile.open(archive, 'w:xz') as package:
                    member = tarfile.TarInfo(f'{name}/ccache')
                    member.size = len(payload)
                    package.addfile(member, io.BytesIO(payload))
                digest = hashlib.sha256(archive.read_bytes()).hexdigest()

                def command(args, **kwargs):
                    self.assertTrue(kwargs['check'])
                    if args[0] == 'curl':
                        self.assertEqual(args[-1],
                                         f'https://github.com/ccache/ccache/releases/download/v{setup.CCACHE_VERSION}/{name}.tar.xz')
                        shutil.copyfile(archive, args[args.index('--output') + 1])
                    else:
                        self.assertEqual(args[:4], ['sudo', 'install', '-m', '755'])
                        self.assertEqual(args[-1], '/usr/local/bin/ccache')
                        self.assertEqual(Path(args[-2]).read_bytes(), payload)

                with patch.object(setup.platform, 'machine', return_value=architecture):
                    with patch.dict(setup.CCACHE_SHA256, {architecture: digest}):
                        with patch.object(setup.subprocess, 'run', side_effect=command) as run:
                            setup.install_ccache()
                self.assertEqual(run.call_count, 2)

    def test_bad_checksum_stops_before_install(self):
        def download(args, **kwargs):
            Path(args[args.index('--output') + 1]).write_bytes(b'tampered archive')

        with patch.object(setup.platform, 'machine', return_value='x86_64'):
            with patch.object(setup.subprocess, 'run', side_effect=download) as run:
                with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                    setup.install_ccache()
        run.assert_called_once()

    def test_failed_download_stops_before_install(self):
        with patch.object(setup.platform, 'machine', return_value='x86_64'):
            with patch.object(setup.subprocess, 'run', side_effect=subprocess.CalledProcessError(22, ['curl'])) as run:
                with self.assertRaises(subprocess.CalledProcessError):
                    setup.install_ccache()
        run.assert_called_once()


if __name__ == '__main__':
    unittest.main()
