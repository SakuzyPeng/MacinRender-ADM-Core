#!/usr/bin/env python3
"""调用驻留 Logic 的真实并轨路径，跳过设置/保存对话框，并验证 Float32 WAVE。"""

import argparse
import array
import fcntl
import hashlib
import json
import math
import os
import plistlib
import shutil
import struct
import subprocess
import time
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
APP = Path('/Applications/Logic Pro Creator Studio.app')


class ResidentClient:
    """Install once per bridge version/process, then submit commands without pausing Logic."""

    def __init__(self, pid, library, runtime, work, timeout):
        self.pid, self.library, self.work, self.timeout = pid, library, work, timeout
        self.directory = runtime / f'commands-{pid}-{library.stem}'
        self.directory.mkdir(mode=0o700, exist_ok=True)
        self.ready = self.directory / 'ready.json'
        registry_path = runtime / f'logic-{pid}-server.json'
        process_start = subprocess.check_output(['ps', '-p', str(pid), '-o', 'lstart='], text=True).strip()
        self.lock = (runtime / f'logic-{pid}.lock').open('a')
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('已有客户端正在使用 Logic；不要并行切换工程') from None
        registry = json.loads(registry_path.read_text()) if registry_path.exists() else {}
        if (not self.ready.exists() or registry.get('server_directory') != str(self.directory) or
                registry.get('process_start') != process_start):
            self.ready.unlink(missing_ok=True)
            install = self.directory / 'install.json'
            install.write_text(json.dumps({'action': 'install_server', 'server_directory': str(self.directory),
                                           'output': str(self.ready), 'deadline_unix': time.time() + 60,
                                           'process_start': process_start}))
            lldb = subprocess.run(['xcrun', 'lldb', '--batch', '-p', str(pid),
                                   '-o', f'command script import {HERE / "logic_lldb.py"}',
                                   '-o', f'logic-request {library} {install}', '-o', 'process detach'],
                                  capture_output=True, text=True, timeout=45)
            (work / 'install-lldb.log').write_text(lldb.stdout + lldb.stderr)
            if lldb.returncode or '"scheduled": true' not in lldb.stdout:
                raise RuntimeError('驻留桥未安装；详见 install-lldb.log')
            self._wait(self.ready, 60)
        ready = json.loads(self.ready.read_text())
        if not ready.get('ok') or ready.get('pid') != pid or ready.get('protocol') != 1:
            raise RuntimeError(f'驻留桥状态不匹配：{ready}')

    @staticmethod
    def _wait(response_path, timeout):
        deadline = time.monotonic() + timeout
        while not response_path.exists():
            if time.monotonic() >= deadline:
                raise TimeoutError(f'Logic 尚未返回。不要重复提交；检查 {response_path} 和应用状态')
            time.sleep(0.1)

    def submit(self, request, action, stage):
        response_path = self.work / f'{stage}-response.json'
        request_path = self.work / f'{stage}-request.json'
        cancel_path = self.work / f'{stage}-cancel'
        if request_path.exists() or response_path.exists():
            raise RuntimeError('不覆盖已提交的请求或响应')
        payload = {**request, 'action': action, 'output': str(response_path),
                   'deadline_unix': time.time() + self.timeout, 'cancel_path': str(cancel_path)}
        encoded = json.dumps(payload, ensure_ascii=False, indent=2) + '\n'
        request_path.write_text(encoded)
        queued = self.directory / f'{uuid.uuid4()}.request'
        temporary = queued.with_suffix('.tmp')
        temporary.write_text(encoded)
        os.replace(temporary, queued)
        try:
            self._wait(response_path, self.timeout)
        except TimeoutError:
            cancel_path.touch()
            raise
        response = json.loads(response_path.read_text())
        if not response.get('ok'):
            raise RuntimeError(response.get('error', response))
        return response


def sha256(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def verify_wave(path):
    chunks = []
    fmt = None
    sample_count = 0
    nonzero = 0
    peak = 0.0
    energy = [0.0, 0.0]
    pcm_hash = hashlib.sha256()
    with path.open('rb') as stream:
        header = stream.read(12)
        if header[:4] != b'RIFF' or header[8:] != b'WAVE':
            raise ValueError('Expected RIFF/WAVE output')
        if struct.unpack_from('<I', header, 4)[0] + 8 != path.stat().st_size:
            raise ValueError('RIFF size does not match the completed file')
        while chunk_header := stream.read(8):
            if len(chunk_header) != 8:
                raise ValueError('Truncated WAVE chunk header')
            tag, count = struct.unpack('<4sI', chunk_header)
            chunks.append((tag.decode('ascii', errors='replace'), count))
            if tag == b'fmt ':
                payload = stream.read(count)
                fmt = struct.unpack_from('<HHIIHH', payload)
                if fmt != (3, 2, 48000, 384000, 8, 32):
                    raise ValueError(f'Expected stereo 48 kHz Float32 WAVE, got {fmt}')
            elif tag == b'data':
                if fmt is None or count % 8:
                    raise ValueError('Missing format or unaligned PCM')
                remaining = count
                while remaining:
                    payload = stream.read(min(remaining, 256 * 1024))
                    if not payload:
                        raise ValueError('Truncated PCM')
                    remaining -= len(payload)
                    pcm_hash.update(payload)
                    samples = array.array('f')
                    samples.frombytes(payload)
                    for index, value in enumerate(samples):
                        if not math.isfinite(value):
                            raise ValueError('Nonfinite PCM sample')
                        nonzero += value != 0.0
                        peak = max(peak, abs(value))
                        energy[(sample_count + index) % 2] += value * value
                    sample_count += len(samples)
            else:
                stream.seek(count, 1)
            if count & 1:
                stream.seek(1, 1)
    frames = sample_count // 2
    if not frames:
        raise ValueError('Empty bounce')
    return {'frames': frames, 'duration_seconds': frames / 48000, 'channels': 2,
            'sample_rate': 48000, 'format': 'pcm_f32le', 'nonzero_samples': nonzero,
            'peak': peak, 'rms': [math.sqrt(value / frames) for value in energy],
            'pcm_sha256': pcm_hash.hexdigest(), 'chunks': chunks}


def inspect_adm(path):
    size = path.stat().st_size
    tags = set()
    fmt = None
    data_size = None
    ds64_data = None
    dbmd_bytes = 0
    with path.open('rb') as stream:
        header = stream.read(12)
        if len(header) != 12 or header[:4] not in (b'RIFF', b'RF64', b'BW64') or header[8:] != b'WAVE':
            raise ValueError('ADM 输入必须是 RIFF/RF64/BW64 WAVE')
        while chunk_header := stream.read(8):
            if len(chunk_header) != 8:
                raise ValueError('ADM chunk header 被截断')
            tag, count = struct.unpack('<4sI', chunk_header)
            if tag == b'data' and count == 0xffffffff:
                if ds64_data is None:
                    raise ValueError('缺少 RF64 ds64 data 大小')
                count = ds64_data
            start = stream.tell()
            if start + count > size:
                raise ValueError('ADM chunk 超出文件边界')
            tags.add(tag)
            if tag == b'ds64':
                if count < 28:
                    raise ValueError('无效 ds64')
                ds64_data = struct.unpack('<QQQI', stream.read(28))[1]
            elif tag == b'fmt ':
                if count < 16:
                    raise ValueError('无效 fmt')
                fmt = struct.unpack('<HHIIHH', stream.read(16))
            elif tag == b'data':
                data_size = count
            elif tag == b'dbmd':
                dbmd_bytes = count
            stream.seek(start + count + (count & 1))
    if not {b'axml', b'chna', b'dbmd', b'fmt ', b'data'} <= tags:
        raise ValueError('此验证路径要求 Logic 兼容 ADM，包含 axml/chna/dbmd')
    if not fmt or fmt[2] != 48000 or not 1 <= fmt[1] <= 128 or fmt[4] <= 0:
        raise ValueError('此路径要求 48 kHz、1–128 轨的有效 PCM')
    if not data_size or data_size % fmt[4]:
        raise ValueError('PCM data 大小无效')
    return {'container': header[:4].decode(), 'channels': fmt[1], 'sample_rate': fmt[2],
            'bit_depth': fmt[5], 'frames': data_size // fmt[4], 'data_bytes': data_size,
            'dbmd_bytes': dbmd_bytes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--expected-document', help='并轨已经打开的工程；名称必须匹配')
    source.add_argument('--adm', type=Path, help='自动创建无窗口工程、导入 ADM、配置并轨并清理')
    source.add_argument('--recover', action='store_true', help='恢复超时后遗留的本工具临时工程，不删除用户工程')
    parser.add_argument('--output', type=Path, help='不存在的 Float32 WAVE 输出路径；恢复模式省略')
    parser.add_argument('--start-clock', type=lambda value: int(value, 0))
    parser.add_argument('--end-clock', type=lambda value: int(value, 0))
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--allow-silence', action='store_true', help='明确允许全静音输出；默认把全静音视为验证失败')
    parser.add_argument('--wait-timeout', type=float, default=120)
    args = parser.parse_args()
    if (not args.recover and args.output is None) or (args.recover and args.output is not None):
        parser.error('并轨模式必须提供 --output；恢复模式不要提供 --output')
    if (args.start_clock is None) != (args.end_clock is None):
        parser.error('start-clock / end-clock 必须一起提供；省略时使用 Logic 的默认工程/循环范围')
    if args.start_clock is not None and not (0 < args.start_clock < args.end_clock < (1 << 63)):
        parser.error('显式 clock 必须为有效的正数范围，且小于 2^63')
    if not math.isfinite(args.wait_timeout) or args.wait_timeout <= 0:
        parser.error('wait-timeout 必须是有限正数')
    if args.adm and args.start_clock is not None:
        parser.error('ADM 模式自动按源帧数计算精确范围；clock 选项只用于已有工程')
    output = args.output.expanduser().resolve() if args.output else None
    if output and output.suffix.lower() != '.wav':
        parser.error('输出文件必须使用 .wav 扩展名')
    if output and (output.exists() or not output.parent.is_dir()):
        parser.error('输出文件必须不存在，其父目录必须已存在')
    adm = args.adm.expanduser().resolve() if args.adm else None
    input_info = inspect_adm(adm) if adm else None
    if input_info:
        required = input_info['data_bytes'] + input_info['frames'] * 8 * 3 + 64 * 1024 * 1024
        if shutil.disk_usage(output.parent).free < required + 256 * 1024 * 1024:
            raise RuntimeError('可用空间不足以容纳临时解交织 PCM 和本次输出')
    profile = json.loads((HERE / 'logic_12_3_1_profile.json').read_text())
    info = plistlib.loads((APP / 'Contents/Info.plist').read_bytes())
    if info.get('CFBundleShortVersionString') != profile['version'] or info.get('CFBundleVersion') != profile['build']:
        raise RuntimeError('Logic 版本不匹配；拒绝调用私有 ABI')
    binary = APP / 'Contents/Frameworks/Logic.framework/Logic'
    if sha256(binary) != profile['logic_sha256']:
        raise RuntimeError('Logic 二进制哈希不匹配；需要重新定位 ABI')
    for name, expected in profile.get('framework_sha256', {}).items():
        if sha256(APP / f'Contents/Frameworks/{name}.framework/{name}') != expected:
            raise RuntimeError(f'{name} 二进制哈希不匹配；拒绝调用私有 ABI')
    processes = subprocess.check_output(['ps', '-axo', 'pid=,comm='], text=True)
    executable = str(APP / 'Contents/MacOS' / info['CFBundleExecutable'])
    matches = [int(line.strip().split(None, 1)[0]) for line in processes.splitlines()
               if len(line.strip().split(None, 1)) == 2 and line.strip().split(None, 1)[1] == executable]
    if len(matches) != 1:
        raise RuntimeError('需要恰好一个正在运行的 Logic 实例')
    work = (output.parent / (output.stem + '.logic-bounce') if output else
            ROOT / 'local/logic-bounce-runtime' / f'recovery-{uuid.uuid4()}')
    work.mkdir(parents=True, exist_ok=False)
    bridge_hash = hashlib.sha256((HERE / 'logic_bridge.mm').read_bytes()).hexdigest()[:16]
    runtime = ROOT / 'local/logic-bounce-runtime'
    runtime.mkdir(exist_ok=True)
    library = runtime / f'logic_bridge_{bridge_hash}.dylib'
    if not library.exists():
        subprocess.run(['xcrun', 'clang++', '-O2', '-std=c++20', '-dynamiclib', '-fobjc-arc',
                        '-framework', 'Foundation', '-framework', 'AppKit',
                        str(HERE / 'logic_bridge.mm'), '-o', str(library)], check=True)
    request = {'action': 'render_adm' if adm else ('prepare_bounce' if args.prepare_only else 'bounce'),
               'expected_document': args.expected_document, 'audio_output': str(output),
               'start_clock': args.start_clock or 0,
               'end_clock': args.end_clock or 0, 'prepare_only': args.prepare_only}
    if adm:
        request['adm'] = str(adm)
    client = ResidentClient(matches[0], library, runtime, work, args.wait_timeout)

    def finish_cleanup(cleanup):
        deadline = time.monotonic() + args.wait_timeout
        checks = []
        while True:
            if time.monotonic() >= deadline:
                raise RuntimeError('原工程渲染器仍在恢复；停止新任务并运行 --recover 完成恢复')
            state = client.submit({}, 'restore_original_renderer', f'restore-renderer-{len(checks):03d}')
            checks.append(state)
            if state.get('ready'):
                cleanup.update(renderer_settings_restored=True, renderer_restore_checks=checks)
                return cleanup

    if args.recover:
        recovery = finish_cleanup(client.submit({}, 'discard_probe_document', 'recovery'))
        report = {'success': True, 'recovery': recovery, 'artifacts': str(work)}
        if recovery.get('discarded') and (not all(recovery.get(k) for k in (
                'original_document_state_preserved', 'original_audio_graph_restored', 'renderer_settings_restored'))
                or recovery.get('temporary_path_exists')):
            report['success'] = False
        (work / 'run.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if not report['success']:
            raise RuntimeError('恢复结果有未通过的核对，请检查恢复报告')
        return

    def submit(action, stage, extra=None):
        return client.submit({**request, **(extra or {})}, action, stage)

    if adm:
        creation = submit('create_probe_document', '01-create')
        pending = False
        try:
            imported = submit('import_probe_adm', '02-import')
            if imported.get('import_code') != 0 or imported.get('source_frames') != input_info['frames']:
                raise RuntimeError(f'ADM 导入失败或帧数不匹配：{imported}')
            activation = submit('activate_probe_audio', '03-activate')
            configuration = submit('configure_probe_renderer', '04-configure')
            readiness = []
            deadline = time.monotonic() + args.wait_timeout
            stable = 0
            while stable < 2:
                if time.monotonic() >= deadline:
                    raise RuntimeError('Atmos 音频图未在期限内就绪；本次不会生成参考音频')
                state = submit('probe_readiness', f'05-ready-{len(readiness):03d}')
                readiness.append(state)
                stable = stable + 1 if state.get('ready') else 0
            repeats = []
            candidate = None
            if args.prepare_only:
                response = submit('bounce_probe_document', '06-prepare')
            else:
                for index in range(3):
                    candidate = work / f'candidate-{index + 1}.wav'
                    response = submit('bounce_probe_document', f'06-bounce-{index + 1}',
                                      {'audio_output': str(candidate)})
                    audio = verify_wave(candidate)
                    if audio['frames'] != input_info['frames']:
                        raise RuntimeError('并轨帧数与源 ADM 不一致')
                    if not args.allow_silence and not audio['nonzero_samples']:
                        raise RuntimeError('候选输出全静音，拒绝作为参考音频')
                    repeats.append({'path': str(candidate), 'audio': audio})
                    (work / 'repeat-verification.json').write_text(json.dumps(repeats, indent=2) + '\n')
                    if len(repeats) >= 2 and repeats[-1]['audio']['pcm_sha256'] == repeats[-2]['audio']['pcm_sha256']:
                        break
                else:
                    raise RuntimeError('三遍并轨未得到连续两遍相同 PCM；保留候选诊断文件，不交付参考音频')
        except TimeoutError:
            pending = True
            raise
        finally:
            if not pending:
                cleanup = finish_cleanup(submit('discard_probe_document', '07-cleanup'))
        response.update(creation=creation, activation=activation, imported=imported, cleanup=cleanup,
                        configuration=configuration, readiness_checks=readiness,
                        repeat_verification=repeats,
                        renderer_settings_restored=cleanup.get('renderer_settings_restored', False),
                        original_document_state_preserved=cleanup.get('original_document_state_preserved', False))
    else:
        response = submit(request['action'], 'bounce')
    (work / 'response.json').write_text(json.dumps(response, ensure_ascii=False, indent=2) + '\n')
    if not response.get('preferences_restored'):
        raise RuntimeError('并轨偏好恢复未得到确认')
    if adm and (not response.get('original_document_state_preserved') or
                not response.get('renderer_settings_restored') or
                not response.get('cleanup', {}).get('original_audio_graph_restored') or
                response.get('cleanup', {}).get('temporary_path_exists')):
        raise RuntimeError('临时工程/渲染器/原工程状态的清理核对未通过，详见 response.json')
    report = {'success': True, 'logic_version': profile['version'], 'logic_build': profile['build'],
              'logic_sha256': profile['logic_sha256'], 'request': request, 'response': response}
    if not args.prepare_only:
        if adm:
            # Publish only after repeatability and restoration pass. A hard link avoids a second audio copy
            # and atomically refuses any destination that appeared while Logic was working.
            os.link(candidate, output)
            report['published_output'] = str(output)
        report['audio'] = verify_wave(output)
        if not args.allow_silence and not report['audio']['nonzero_samples']:
            report.update(success=False, failure='unexpected_silence')
            (work / 'failed-verification.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
            raise RuntimeError('输出全静音，未通过验证；仅在有意导出静音时使用 --allow-silence')
        if input_info and report['audio']['frames'] != input_info['frames']:
            raise RuntimeError('并轨帧数与源 ADM 不一致')
        report['file_sha256'] = sha256(output)
    if adm:
        report['input'] = {'path': str(adm), 'sha256': sha256(adm), **input_info}
    (work / 'run.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
