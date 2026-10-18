#!/usr/bin/env python3
"""Time public C API room preparation with an inactive versus a pinned weak cache.

Release library required. 'Cold' means no live prepared handle, not a flushed OS
file cache. Each hit still reads/authenticates every layer. No audio is played.
"""
import argparse
import ctypes as C
import hashlib
import importlib.util
import json
from pathlib import Path
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--room', required=True, type=Path)
    parser.add_argument('--hrir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--repetitions', type=int, default=3)
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error('repetitions must be positive')
    root = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location('room_capi', root / 'tests/unit/room_96k_test.py')
    api_types = importlib.util.module_from_spec(spec); spec.loader.exec_module(api_types)
    api = api_types.Api(args.library.resolve())
    handle = api_types.PTR()
    start = time.perf_counter()
    api.ok(api.lib.adm_room_load(api.context, str(args.room.resolve()).encode(), C.byref(handle)))
    profile_seconds = time.perf_counter() - start
    rate = json.loads(args.room.read_text(encoding='utf-8'))['sample_rate']
    room = api_types.Room(handle=handle, hrir=str(args.hrir.resolve()).encode(), metres=1.5)
    config = api_types.Config(rendering=api_types.Renderer(renderer=8, layout=b'binaural'), input_rate=rate,
                              output_rate=rate, ring=32768, watermark=1)
    results = []
    try:
        for repetition in range(args.repetitions):
            first, second = api_types.PTR(), api_types.PTR()
            try:
                start = time.perf_counter()
                api.ok(api.lib.adm_create_scene_stream_with_room_quality(api.context, C.byref(config), C.byref(room), 1, None, C.byref(first)))
                cold = time.perf_counter() - start
                start = time.perf_counter()
                api.ok(api.lib.adm_create_scene_stream_with_room_quality(api.context, C.byref(config), C.byref(room), 1, None, C.byref(second)))
                hit = time.perf_counter() - start
                row = dict(repetition=repetition + 1, cold_seconds=cold, active_hit_seconds=hit)
                results.append(row)
                print(json.dumps(row), flush=True)
            finally:
                if second: api.lib.adm_destroy_scene_stream(second)
                if first: api.lib.adm_destroy_scene_stream(first)
    finally:
        api.lib.adm_room_destroy(handle)
        api.lib.adm_destroy_context(api.context)
    report = dict(schema='mradm.room-preparation-benchmark', version=1, sample_rate=rate,
                  library_sha256=hashlib.sha256(args.library.read_bytes()).hexdigest(),
                  profile_seconds=profile_seconds, results=results,
                  scope='Release public C API Scene creation, including HRIR authentication/preparation and session allocation; cold releases all live rooms; filesystem cache is retained')
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
