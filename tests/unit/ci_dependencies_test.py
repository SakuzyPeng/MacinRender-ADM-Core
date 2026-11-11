#!/usr/bin/env python3
"""Offline checks for Ubuntu CI source configuration and failed index updates."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('ubuntu_deps', ROOT / 'scripts/ci/install-ubuntu-deps.py')
installer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(installer)


class UbuntuDependenciesTests(unittest.TestCase):
    def test_runner_sources_use_https_and_keep_architecture_and_signature_settings(self):
        fixtures = {
            'sources.list': (
                'deb [arch=amd64 signed-by=/keys/ubuntu.gpg] http://azure.archive.ubuntu.com/ubuntu/ noble main\n',
                'deb [arch=amd64 signed-by=/keys/ubuntu.gpg] https://archive.ubuntu.com/ubuntu/ noble main\n'),
            'sources.list.d/ubuntu.sources': (
                'Types: deb\nURIs: http://ports.ubuntu.com/ubuntu-ports/\nSuites: noble noble-security\n'
                'Components: main universe\nSigned-By: /keys/ubuntu.gpg\n',
                'Types: deb\nURIs: https://ports.ubuntu.com/ubuntu-ports/\nSuites: noble noble-security\n'
                'Components: main universe\nSigned-By: /keys/ubuntu.gpg\n'),
            'apt-mirrors.txt': (
                'http://azure.archive.ubuntu.com/ubuntu/\nhttps://archive.ubuntu.com/ubuntu/\n'
                'http://security.ubuntu.com/ubuntu/\n',
                'https://archive.ubuntu.com/ubuntu/\nhttps://archive.ubuntu.com/ubuntu/\n'
                'https://security.ubuntu.com/ubuntu/\n'),
            'sources.list.d/mirrors.sources': (
                'Types: deb\nURIs: mirror+file:/etc/apt/apt-mirrors.txt\nSuites: noble\n',
                'Types: deb\nURIs: mirror+file:/etc/apt/apt-mirrors.txt\nSuites: noble\n'),
            'sources.list.d/other.list': (
                'deb https://packages.microsoft.com/ubuntu/24.04/prod noble main\n',
                'deb https://packages.microsoft.com/ubuntu/24.04/prod noble main\n'),
        }
        with tempfile.TemporaryDirectory() as directory:
            apt_root = Path(directory)
            for name, (original, _) in fixtures.items():
                path = apt_root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(original, encoding='utf-8')
            for _ in range(2):
                installer.use_https_sources(apt_root)
                for name, (_, expected) in fixtures.items():
                    with self.subTest(name=name):
                        self.assertEqual((apt_root / name).read_text(encoding='utf-8'), expected)

    def test_missing_source_formats_are_optional(self):
        with tempfile.TemporaryDirectory() as directory:
            installer.use_https_sources(Path(directory))
            self.assertEqual(list(Path(directory).iterdir()), [])

    @patch.object(installer, 'use_https_sources')
    @patch.object(installer.subprocess, 'run')
    def test_update_and_install_retry_with_timeouts(self, run, configure):
        with patch('sys.argv', ['install-ubuntu-deps.py', 'cmake', 'ninja-build']):
            installer.main()
        configure.assert_called_once_with(Path('/etc/apt'))
        update, install = run.call_args_list
        self.assertEqual(update.args[0][-2:], ['update', '--error-on=any'])
        self.assertEqual(install.args[0][-4:], ['install', '-y', 'cmake', 'ninja-build'])
        for call in (update, install):
            self.assertTrue(call.kwargs['check'])
            for option in ('Acquire::Retries=3', 'Acquire::http::Timeout=30', 'Acquire::https::Timeout=30'):
                self.assertIn(option, call.args[0])

    @patch.object(installer, 'use_https_sources')
    @patch.object(installer.subprocess, 'run', side_effect=subprocess.CalledProcessError(100, ['apt-get']))
    def test_failed_update_stops_before_install(self, run, configure):
        with patch('sys.argv', ['install-ubuntu-deps.py', 'cmake']):
            with self.assertRaises(subprocess.CalledProcessError):
                installer.main()
        run.assert_called_once()


if __name__ == '__main__':
    unittest.main()
