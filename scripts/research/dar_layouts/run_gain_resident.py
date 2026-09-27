#!/usr/bin/env python3
"""Headless capture with one LLDB installation and a resident command transport."""

import json
import fcntl
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

import run_headless_rerender as rerender

HERE = Path(__file__).resolve().parent
CONFIG = {}
BASE_SESSION = rerender.Session


def switch_layout(session, layout):
    if session.inspect()["config"]["rows"][0]["layout"] == layout:
        return
    session.action("stage_layout", layout=layout, applyImmediately=True)
    session.wait_for(f"committed {layout}", lambda state:
                     state["config"]["rows"][0]["layout"] == layout and not state["config"]["processing"])


def compile_bridge(path):
    result = subprocess.run([
        "xcrun", "clang++", "-std=c++20", "-O2", "-fPIC", "-dynamiclib",
        "-DQT_NO_VERSION_TAGGING", "-I/opt/homebrew/include", f"-F{rerender.FRAMEWORKS}",
        str(HERE / "dar_gain_resident.cpp"), "-framework", "QtCore", "-framework", "QtGui",
        "-framework", "QtQml", f"-Wl,-rpath,{rerender.FRAMEWORKS}", "-o", str(path),
    ], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"resident bridge compilation failed:\n{result.stderr}")


class ResidentSession(BASE_SESSION):
    def __init__(self, pid, bridge, control_dir):
        super().__init__(pid, bridge, control_dir)
        self.inbox = control_dir / "inbox.json"
        ready = control_dir / "ready.json"
        config_path = control_dir / "resident-config.json"
        config = {"library": str(bridge), "inbox": str(self.inbox), "ready_response": str(ready),
                  "module_uuid": CONFIG["module_uuid"],
                  "trace_file": CONFIG["trace_file"], "capture": CONFIG.get("capture", bool(CONFIG.get("breakpoints"))),
                  "image_base": CONFIG["image_base"], "max_records": CONFIG.get("max_records", 2048),
                  "record_stride": CONFIG.get("record_stride", 16),
                  "output_channels": CONFIG.get("output_channels", 0)}
        config.update({key: CONFIG[key] for key in ("vtable_slot", "gain_function", "hooks", "size_dispatch") if key in CONFIG})
        config_path.write_text(json.dumps(config, indent=2) + "\n")
        command = ["xcrun", "lldb", "--batch", "-p", str(pid),
                   "-o", f"command script import {HERE / 'dar_resident_lldb.py'}",
                   "-o", f"dar-resident-start {config_path}", "-o", "detach"]
        initial_process_state = subprocess.check_output(["ps", "-p", str(pid), "-o", "state="], text=True).strip()
        if "T" in initial_process_state:
            raise RuntimeError("Renderer was already stopped before capture installation")
        attached = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (Path(CONFIG["trace_file"]).parent / "resident-install.log").write_text(
            attached.stdout + attached.stderr)
        if attached.returncode != 0 or '"resident_scheduled": true' not in attached.stdout:
            raise RuntimeError(f"resident installation failed: {attached.stdout[-1800:]} {attached.stderr[-300:]}")
        detached_state = subprocess.check_output(["ps", "-p", str(pid), "-o", "state="], text=True).strip()
        if "T" in detached_state:
            # LLDB occasionally detaches with its attach SIGSTOP still pending.
            # Restore the observed running state; never resume a pre-stopped app.
            os.kill(pid, signal.SIGCONT)
        deadline = time.monotonic() + 30
        while not ready.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        if not ready.exists():
            raise RuntimeError("resident server did not become ready after detach")
        result = json.loads(ready.read_text())
        if not result.get("ok"):
            raise RuntimeError(f"resident setup failed: {result}")

    def action(self, action_name, **arguments):
        if rerender.renderer_pid() != self.pid:
            raise RuntimeError("Renderer process changed during resident capture")
        if self.inbox.exists():
            raise RuntimeError("previous resident request was not consumed")
        self.serial += 1
        response = self.control_dir / f"{self.serial:03d}-{action_name}-result.json"
        temporary = self.control_dir / f"{self.serial:03d}-request.tmp"
        temporary.write_text(json.dumps({"action": action_name, "response": str(response), **arguments}) + "\n")
        temporary.replace(self.inbox)
        deadline = time.monotonic() + (120 if action_name == "start_export" else 30)
        while not response.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        if not response.exists():
            raise RuntimeError(f"resident Renderer did not answer {action_name}")
        result = json.loads(response.read_text())
        self.trace.append({"action": action_name, "ok": result.get("ok", False), "error": result.get("error")})
        if not result.get("ok"):
            raise RuntimeError(f"resident {action_name} failed: {result}")
        return result

    def close(self):
        result = {}
        error = None
        try:
            result["capture"] = self.action("gain_trace_finish")
        except Exception as caught:
            error = caught
        try:
            result["server"] = self.action("resident_shutdown")
        except Exception as caught:
            error = error or caught
        if error is not None:
            raise error
        return result


def main():
    global CONFIG
    path = Path(sys.argv[1]).resolve()
    # All research suites share one Renderer process. Fail before attaching when
    # another resident export owns it; never race its master/settings restore.
    export_lock = (rerender.ROOT / "local/dar-resident-export.lock").open("a")
    try:
        fcntl.flock(export_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as error:
        raise RuntimeError("another resident export is using Renderer") from error
    CONFIG = json.loads(path.read_text())
    if rerender.file_sha256(rerender.APP) != CONFIG["binary_sha256"]:
        raise ValueError("Renderer executable differs from the identified binary")
    uuids = subprocess.check_output(["xcrun", "dwarfdump", "--uuid", str(rerender.APP)], text=True)
    if f"{CONFIG['module_uuid']} (arm64)" not in uuids:
        raise ValueError("Renderer arm64 UUID differs from the identified binary")
    rerender.Session = ResidentSession
    rerender.switch_layout = switch_layout
    rerender.compile_bridge = compile_bridge
    sys.argv = ["run_headless_rerender.py", *CONFIG["export_arguments"]]
    try:
        return rerender.main()
    finally:
        export_lock.close()


if __name__ == "__main__":
    raise SystemExit(main())
