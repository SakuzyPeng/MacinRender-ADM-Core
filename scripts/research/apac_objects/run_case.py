#!/usr/bin/env python3
"""Run one research case in its own process group and save reproducible evidence."""

import argparse
import datetime
import json
import os
from pathlib import Path
import signal
import re
import subprocess
import time


def run_case(command, output, timeout):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    process = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True
    )
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        os.killpg(process.pid, signal.SIGKILL)
        stdout, stderr = process.communicate()
    result = {
        "command": command,
        "cwd": str(Path.cwd()),
        "finished_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "timeout_seconds": timeout,
        "timed_out": timed_out,
        "returncode": process.returncode,
        "elapsed_seconds": time.monotonic() - started,
        "research_environment": {key: os.environ[key] for key in (
            "APAC_PROFILE_CEILING", "APAC_TRACE_POSITIONS", "APAC_BINARY_METADATA", "APAC_TRACE_ERRORS",
            "APAC_DECODER_METADATA", "APAC_TRACE_LFE", "APAC_ADM_TRACE_PACKETS",
            "APAC_ADM_TRACE_WIRE", "APAC_SPATIAL_OBSERVER", "DYLD_INSERT_LIBRARIES",
            "APAC_PROBE_COMPOSITION", "APAC_PROBE_UNITY_MIX", "APAC_TAP_MIXDOWN",
            "APAC_CHAIN_CONSUMERS", "APAC_CHAIN_POLICY") if key in os.environ},
        "events": [],
    }
    exits = re.findall(rb"Process \d+ exited with status = (-?\d+)", stdout)
    if "lldb" in command:
        result["debugger_returncode"] = process.returncode
        result["inferior_returncode"] = int(exits[-1]) if exits else None
        result["returncode"] = int(exits[-1]) if exits else (process.returncode or 1)
    for line in stdout.decode("utf-8", errors="replace").splitlines():
        try:
            result["events"].append(json.loads(line))
        except json.JSONDecodeError:
            pass
    (output / "stdout.txt").write_bytes(stdout)
    (output / "stderr.txt").write_bytes(stderr)
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--debugger", action="store_true", help="60-second deadline instead of 30")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a command is required after --")
    result = run_case(command, args.output, 60 if args.debugger else 30)
    print(json.dumps(result, indent=2))
    return 124 if result["timed_out"] else result["returncode"]


if __name__ == "__main__":
    raise SystemExit(main())
