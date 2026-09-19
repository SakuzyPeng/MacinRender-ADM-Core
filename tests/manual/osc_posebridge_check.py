"""Headless integration of a built C ABI library with the actual PoseBridge simulator.

No GUI, audio device, sensor configuration or external Python dependencies are used.
Build mradm_capi_bundle in the existing Release tree, then pass --library and --posebridge.
"""

import argparse
import ctypes as c
import json
import math
from pathlib import Path
import socket
import subprocess
import time


class Config(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("listen_port", c.c_uint32)]


class Pose(c.Structure):
    _fields_ = [
        ("struct_size", c.c_uint32), ("has_pose", c.c_uint32),
        ("fresh", c.c_uint32), ("reserved", c.c_uint32),
        ("session_id", c.c_uint64), ("sequence", c.c_uint64),
        ("received_ns", c.c_uint64), ("age_ms", c.c_uint64),
        ("quaternion", c.c_float * 4),
        ("yaw", c.c_float), ("pitch", c.c_float), ("roll", c.c_float),
        ("reserved_tail", c.c_uint32),
    ]


class PoseV2(c.Structure):
    _fields_ = [
        ("struct_size", c.c_uint32), ("protocol_version", c.c_uint32), ("pose", Pose),
        ("sample_time_kind", c.c_uint32), ("reserved", c.c_uint32),
        ("source_session_id", c.c_uint64), ("source_sequence", c.c_uint64),
        ("source_received_ns", c.c_uint64), ("sample_time_ms", c.c_uint64),
        ("sample_clock_epoch", c.c_uint64),
    ]


class Status(c.Structure):
    _fields_ = [
        ("struct_size", c.c_uint32), ("state", c.c_int32),
        ("bound_port", c.c_uint32), ("has_pose", c.c_uint32),
        ("session_id", c.c_uint64), ("sequence", c.c_uint64),
        ("packets_received", c.c_uint64), ("rejected_packets", c.c_uint64),
        ("recovery_count", c.c_uint64), ("age_ms", c.c_uint64),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--posebridge", type=Path, required=True)
    args = parser.parse_args()
    lib = c.CDLL(str(args.library.resolve()))
    handle = c.c_void_p()
    declarations = {
        "adm_create_osc_head_tracking": ([c.POINTER(Config), c.POINTER(c.c_void_p)], c.c_int),
        "adm_destroy_osc_head_tracking": ([c.c_void_p], None),
        "adm_osc_head_tracking_start": ([c.c_void_p], c.c_int),
        "adm_osc_head_tracking_stop": ([c.c_void_p], None),
        "adm_osc_head_tracking_get_pose": ([c.c_void_p, c.POINTER(Pose)], c.c_int),
        "adm_osc_head_tracking_get_pose_v2": ([c.c_void_p, c.POINTER(PoseV2)], c.c_int),
        "adm_osc_head_tracking_get_status": ([c.c_void_p, c.POINTER(Status)], c.c_int),
        "adm_osc_head_tracking_last_error_message": ([c.c_void_p], c.c_char_p),
    }
    for name, (arguments, result) in declarations.items():
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = arguments, result
    assert c.sizeof(Pose) == 80 and c.sizeof(Status) == 64 and c.sizeof(PoseV2) == 136
    assert lib.adm_api_version_major() == 1 and lib.adm_api_version_minor() >= 41
    config = Config(c.sizeof(Config), 0)
    assert lib.adm_create_osc_head_tracking(c.byref(config), c.byref(handle)) == 0

    def pose():
        value = Pose()
        value.struct_size = c.sizeof(Pose)
        assert lib.adm_osc_head_tracking_get_pose(handle, c.byref(value)) == 0
        return value

    def pose_v2():
        value = PoseV2()
        value.struct_size = c.sizeof(PoseV2)
        assert lib.adm_osc_head_tracking_get_pose_v2(handle, c.byref(value)) == 0
        return value

    def status():
        value = Status()
        value.struct_size = c.sizeof(Status)
        assert lib.adm_osc_head_tracking_get_status(handle, c.byref(value)) == 0
        return value

    def simulate(fmt, extra, version="v1", sample_clock=False):
        first_sequence = pose().sequence
        command = [str(args.posebridge.resolve()), "simulate", "--format", fmt,
                   "--sample-rate-hz", "100", "--osc-rate-hz", "100",
                   "--osc-target", f"127.0.0.1:{status().bound_port}", "--duration", "1", "--json", "--osc-version", version] + extra + (["--sample-clock"] if sample_clock else [])
        samples = []
        previous = None
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
            deadline = time.monotonic() + 12
            try:
                while process.poll() is None:
                    timed = pose_v2()
                    value = timed.pose
                    if value.sequence > first_sequence and (not samples or value.sequence != samples[-1][0]):
                        assert value.fresh and all(math.isfinite(v) for v in value.quaternion)
                        assert abs(sum(v * v for v in value.quaternion) - 1) < 1e-5
                        assert timed.protocol_version == (2 if version == "v2" else 1)
                        if version == "v2":
                            assert timed.source_session_id > 0 and timed.source_sequence > 0
                            if previous is not None:
                                assert timed.source_session_id == previous.source_session_id
                                assert timed.source_sequence > previous.source_sequence
                                assert timed.source_received_ns > previous.source_received_ns
                            if sample_clock:
                                assert timed.sample_time_kind == 2 and timed.sample_clock_epoch == 1
                                assert timed.sample_time_ms == timed.source_received_ns // 1000000
                            else:
                                assert (timed.sample_time_kind, timed.sample_time_ms, timed.sample_clock_epoch) == (0,0,0)
                        else:
                            assert (timed.sample_time_kind, timed.source_session_id, timed.source_received_ns) == (0,0,0)
                        previous = timed
                        samples.append((value.sequence, value.yaw, list(value.quaternion)))
                    assert time.monotonic() < deadline, "simulator did not finish"
                    time.sleep(0.002)
                stdout, stderr = process.communicate(timeout=2)
                assert process.returncode == 0, (stdout, stderr)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        last = pose()
        assert last.sequence - first_sequence >= 25 and samples
        return last, samples

    try:
        assert lib.adm_osc_head_tracking_start(handle) == 0, lib.adm_osc_head_tracking_last_error_message(handle)
        assert status().state == 1 and not pose().has_pose
        first_session = status().session_id
        first, _ = simulate("quaternion", ["--yaw", "30", "--pitch", "20", "--roll", "10"])
        assert all(abs(a - b) < 0.001 for a, b in zip((first.yaw, first.pitch, first.roll), (30, 20, 10)))
        time.sleep(0.6)
        stale = pose()
        assert not stale.fresh and status().state == 3
        assert stale.sequence == first.sequence and list(stale.quaternion) == list(first.quaternion)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.sendto(b"invalid OSC", ("127.0.0.1", status().bound_port))
        time.sleep(0.05)
        assert not pose().fresh and status().rejected_packets >= 1
        resumed, _ = simulate("euler", ["--yaw", "-30", "--pitch", "15", "--roll", "-10"])
        assert all(abs(a - b) < 0.001 for a, b in zip((resumed.yaw, resumed.pitch, resumed.roll), (-30, 15, -10)))
        assert resumed.session_id == first_session and status().recovery_count == 1
        _, wrapped = simulate("quaternion", ["--pattern", "wrap"])
        assert any(row[1] > 175 for row in wrapped) and any(row[1] < -175 for row in wrapped)
        for before, after in zip(wrapped, wrapped[1:]):
            dot = abs(sum(a * b for a, b in zip(before[2], after[2])))
            assert dot > 0.98, "wrap generated a large quaternion jump"
        timed_sessions = set()
        for fmt in ["quaternion", "euler"]:
            simulate(fmt, ["--yaw", "30", "--pitch", "20", "--roll", "10"], "v2", True)
            value = pose_v2()
            assert value.source_session_id not in timed_sessions
            timed_sessions.add(value.source_session_id)
            assert value.pose.session_id == first_session
        simulate("quaternion", [], "v2", False)
        assert pose_v2().sample_time_kind == 0
        final_status = status()
        lib.adm_osc_head_tracking_stop(handle)
        assert not pose().fresh and status().state == 4
        assert lib.adm_osc_head_tracking_start(handle) == 0
        assert not pose().has_pose and status().session_id != first_session
        print(json.dumps({"result": "PASS", "formats": ["quaternion", "euler"],
                          "versions": [1, 2], "atomic_timing": True, "source_restart": True,
                          "valid_samples": final_status.sequence,
                          "rejected_packets": final_status.rejected_packets,
                          "stale_and_recovery": True, "wrap_short_arc": True,
                          "stop_restart": True}, indent=2))
    finally:
        lib.adm_destroy_osc_head_tracking(handle)


if __name__ == "__main__":
    main()
