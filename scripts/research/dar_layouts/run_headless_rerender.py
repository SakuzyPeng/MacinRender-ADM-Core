#!/usr/bin/env python3
"""Export 7.1.4/9.1.6 WAV from ADM through the installed Renderer, without UI.

This is a local research adapter for Dolby Atmos Renderer 5.5, not a public
Dolby API. It requires one unmapped re-render strip and an idle Renderer.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import plistlib
import re
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
APP = Path(
    "/Applications/Dolby/Dolby Atmos Renderer/"
    "Dolby Atmos Renderer.app/Contents/MacOS/Dolby Atmos Renderer"
)
FRAMEWORKS = APP.parent.parent / "Frameworks"
PERSISTENCE = Path.home() / "Library/Application Support/Dolby/Dolby Atmos Renderer/persistence.db"
CHANNELS = {"7.1.4": 12, "9.1.6": 16}


def renderer_pid() -> int:
    lines = subprocess.check_output(["ps", "-ax", "-o", "pid=,command="], text=True).splitlines()
    matches = []
    for line in lines:
        parts = line.strip().split(maxsplit=1)
        if len(parts) == 2 and parts[1] == str(APP):
            matches.append(int(parts[0]))
    if len(matches) != 1:
        raise RuntimeError(f"expected one running Renderer, found {len(matches)}")
    return matches[0]


def compile_bridge(path: Path) -> None:
    command = [
        "xcrun", "clang++", "-std=c++20", "-O2", "-fPIC", "-dynamiclib",
        "-DQT_NO_VERSION_TAGGING", "-I/opt/homebrew/include", f"-F{FRAMEWORKS}",
        str(HERE / "dar_batch_bridge.cpp"), "-framework", "QtCore", "-framework", "QtGui",
        "-framework", "QtQml", f"-Wl,-rpath,{FRAMEWORKS}", "-o", str(path),
    ]
    subprocess.run(command, check=True, capture_output=True, text=True)


def key_hashes(connection: sqlite3.Connection) -> dict[str, str | None]:
    result = {}
    for key in ("24", "251"):
        row = connection.execute("select value from entries where key=?", (key,)).fetchone()
        result[key] = hashlib.sha256(row[0]).hexdigest() if row else None
    return result


def backup_settings(destination: Path) -> dict[str, str | None]:
    if destination.exists():
        raise RuntimeError(f"settings backup already exists: {destination}")
    with sqlite3.connect(f"file:{PERSISTENCE}?mode=ro", uri=True) as source:
        with sqlite3.connect(destination) as target:
            source.backup(target)
            return key_hashes(target)


def current_settings_hashes() -> dict[str, str | None]:
    with sqlite3.connect(f"file:{PERSISTENCE}?mode=ro", uri=True) as database:
        return key_hashes(database)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def wav_info(path: Path, expected_channels: int) -> dict:
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0",
         "-show_entries", "stream=codec_name,sample_rate,channels,duration", "-of", "json", str(path)],
        check=True, capture_output=True, text=True,
    )
    streams = json.loads(probe.stdout).get("streams", [])
    if len(streams) != 1:
        raise RuntimeError(f"expected one audio stream in {path}")
    stream = streams[0]
    if int(stream.get("sample_rate", 0)) != 48_000 or int(stream.get("channels", 0)) != expected_channels:
        raise RuntimeError(f"unexpected WAV format: {stream}")
    if not stream.get("codec_name", "").startswith("pcm_") or float(stream.get("duration", 0)) <= 0:
        raise RuntimeError(f"invalid PCM WAV: {stream}")
    subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-f", "null", "-"],
                   check=True, capture_output=True)
    return {
        "path": str(path), "bytes": path.stat().st_size,
        "sha256": file_sha256(path),
        "codec": stream["codec_name"], "channels": expected_channels,
        "sample_rate": 48_000, "duration_seconds": float(stream["duration"]),
    }


class Session:
    def __init__(self, pid: int, bridge: Path, control_dir: Path):
        self.pid = pid
        self.bridge = bridge
        self.control_dir = control_dir
        self.serial = 0
        self.trace: list[dict] = []

    def action(self, action_name: str, **arguments) -> dict:
        if renderer_pid() != self.pid:
            raise RuntimeError("Renderer process changed during the run")
        self.serial += 1
        request = self.control_dir / f"{self.serial:03d}-{action_name}.json"
        response = self.control_dir / f"{self.serial:03d}-{action_name}-result.json"
        request.write_text(json.dumps({"action": action_name, "response": str(response), **arguments},
                                      ensure_ascii=False) + "\n", encoding="utf-8")
        command = [
            "xcrun", "lldb", "--batch", "-p", str(self.pid),
            "-o", f"command script import {HERE / 'dar_batch_lldb.py'}",
            "-o", f"dar-batch-run {self.bridge} {request}", "-o", "detach",
        ]
        run = subprocess.run(command, capture_output=True, text=True, timeout=45)
        if run.returncode != 0 or '{"scheduled": true}' not in run.stdout:
            raise RuntimeError(f"LLDB failed to queue {action_name}: {run.stdout[-1200:]} {run.stderr[-300:]}")
        # Offline exports can occupy Renderer's event loop while processing a
        # longer ADM. A queued Qt response may arrive only when it finishes.
        deadline = time.monotonic() + (10 if action_name == "release_exporter" else 120)
        while not response.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        if not response.exists():
            if action_name == "release_exporter":
                observed = self.inspect()
                if observed["exporter"] is None:
                    result = {"ok": True, "releaseObserved": True}
                else:
                    raise RuntimeError("Renderer did not answer release_exporter; exporter is still present")
            else:
                raise RuntimeError(f"Renderer did not answer {action_name}")
        else:
            result = json.loads(response.read_text(encoding="utf-8"))
        self.trace.append({"action": action_name, "ok": result.get("ok", False),
                           "error": result.get("error")})
        if not result.get("ok"):
            raise RuntimeError(f"Renderer {action_name}: {result.get('error', 'unknown error')}")
        return result

    def inspect(self) -> dict:
        return self.action("inspect")

    def wait_for(self, label: str, predicate, timeout: float = 20) -> dict:
        deadline = time.monotonic() + timeout
        latest = None
        while time.monotonic() < deadline:
            latest = self.inspect()
            if predicate(latest):
                return latest
            time.sleep(0.4)
        raise RuntimeError(f"timed out waiting for {label}; last state: {latest}")


def switch_layout(session: Session, layout: str) -> None:
    state = session.inspect()
    if state["config"]["rows"][0]["layout"] == layout:
        return
    session.action("stage_layout", layout=layout)
    session.wait_for(f"staged {layout}",
                     lambda value: value["config"]["rows"][0]["layout"] == layout)
    session.action("apply_layout", layout=layout)
    session.wait_for(f"applied {layout}",
                     lambda value: value["config"]["rows"][0]["layout"] == layout
                     and not value["config"]["processing"])


def finish_export(session: Session, output_dir: Path, previous: set[Path], expected_files: int,
                  timeout: float) -> list[Path]:
    deadline = time.monotonic() + timeout
    stable_size = -1
    stable_since = time.monotonic()
    last_status = None
    while time.monotonic() < deadline:
        if renderer_pid() != session.pid:
            raise RuntimeError("Renderer exited during export")
        files = sorted(set(output_dir.rglob("*.wav")) - previous)
        if len(files) > expected_files:
            raise RuntimeError(f"export created {len(files)} WAVs; expected {expected_files}")
        if len(files) == expected_files:
            size = sum(path.stat().st_size for path in files)
            if size != stable_size:
                stable_size = size
                stable_since = time.monotonic()
            if size > 1024 and time.monotonic() - stable_since >= 0.8:
                state = session.inspect()
                last_status = state["exporter"]
                if last_status and last_status["state"] == 2 and last_status["percentCompleted"] == 100:
                    return files
        time.sleep(0.25)
    raise RuntimeError(f"export timed out: {last_status}")


def restore(session: Session, initial: dict, original_hashes: dict) -> list[str]:
    errors = []
    try:
        state = session.inspect()
        if state["exporter"] is not None:
            exporter = state["exporter"]
            if exporter["state"] != 2 and exporter["percentCompleted"] != 100:
                try:
                    session.action("cancel_export")
                except Exception as exc:
                    errors.append(str(exc))
            saved = getattr(session, "exporter_original", None)
            if saved:
                try:
                    session.action("restore_exporter", **saved)
                    session.wait_for("restored exporter settings", lambda value:
                                     value["exporter"] is not None
                                     and value["exporter"]["outputDir"] == saved["outputDir"]
                                     and value["exporter"]["exportName"] == saved["exportName"])
                except Exception as exc:
                    errors.append(str(exc))
            session.action("release_exporter")
            session.wait_for("released exporter", lambda value: value["exporter"] is None)
    except Exception as exc:
        errors.append(f"exporter restoration: {exc}")
    try:
        original_row = initial["config"]["rows"][0]
        state = session.inspect()
        row = state["config"]["rows"][0]
        if row["layout"] != original_row["layout"]:
            switch_layout(session, original_row["layout"])
        elif row["newLayoutIndex"] != original_row["newLayoutIndex"]:
            session.action("cancel_layout")
        if current_settings_hashes() != original_hashes:
            errors.append("persistent settings keys 24/251 differ from the backup")
    except Exception as exc:
        errors.append(f"layout restoration: {exc}")
    try:
        original_master = initial["master"]
        state = session.inspect()["master"]
        if state["opened"] != original_master["opened"] or state["path"] != original_master["path"]:
            session.action("restore_master", opened=original_master["opened"],
                           path=original_master["path"])
            session.wait_for("restored master", lambda value:
                             value["master"]["opened"] == original_master["opened"]
                             and value["master"]["path"] == original_master["path"])
    except Exception as exc:
        errors.append(f"master restoration: {exc}")
    return errors


def inspect_timeout_was_recovered(report: dict) -> bool:
    """A late inspect can time out after both files finish; verify every result."""
    if report.get("error") != "Renderer did not answer inspect" or report.get("restore_errors"):
        return False
    if report.get("settings_sha256_after") != report.get("baseline_settings_sha256"):
        return False
    if {item["layout"] for item in report.get("outputs", [])} != set(report["requested_layouts"]):
        return False
    initial, final = report.get("initial"), report.get("final")
    if not initial or not final or final.get("exporter") is not None:
        return False
    if final["config"]["rows"] != initial["config"]["rows"] or final["config"]["processing"]:
        return False
    if final["master"] != initial["master"]:
        return False
    try:
        for item in report["outputs"]:
            files = [item] if report["file_type"] == "interleaved" else item["files"]
            for file in files:
                path = Path(file["path"])
                if file_sha256(path) != file["sha256"] or path.stat().st_size != file["bytes"]:
                    return False
                wav_info(path, file["channels"])
    except (OSError, ValueError, subprocess.CalledProcessError):
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adm", type=Path, required=True, help="ADM BWF input")
    parser.add_argument("--output-dir", type=Path, required=True, help="new, empty output directory")
    parser.add_argument("--layouts", default="7.1.4,9.1.6", help="comma-separated 7.1.4 and/or 9.1.6")
    parser.add_argument("--file-type", choices=("interleaved", "multi-mono"), default="interleaved")
    parser.add_argument("--name", help="ASCII prefix for exported WAV files")
    parser.add_argument("--export-timeout", type=float, default=300)
    args = parser.parse_args()
    layouts = [item.strip() for item in args.layouts.split(",")]
    if not layouts or len(layouts) != len(set(layouts)) or any(item not in CHANNELS for item in layouts):
        parser.error("layouts must be unique choices from 7.1.4,9.1.6")
    adm = args.adm.expanduser().resolve()
    if not adm.is_file():
        parser.error(f"ADM input does not exist: {adm}")
    output_dir = args.output_dir.expanduser().resolve()
    if output_dir.exists() and any(output_dir.iterdir()):
        parser.error("output directory must be empty")
    output_dir.mkdir(parents=True, exist_ok=True)
    prefix = args.name
    if prefix is None:
        prefix = re.sub(r"[^A-Za-z0-9_-]+", "-", adm.stem).strip("-")
        if not prefix:
            prefix = "adm-" + hashlib.sha256(str(adm).encode()).hexdigest()[:8]
    if not re.fullmatch(r"[A-Za-z0-9_-]+", prefix):
        parser.error("--name (or the ADM stem) must contain ASCII letters, digits, _ or -")
    if not APP.is_file() or not PERSISTENCE.is_file():
        parser.error("installed Renderer 5.5 or persistence database not found")
    with (APP.parent.parent / "Info.plist").open("rb") as source:
        version = str(plistlib.load(source).get("CFBundleShortVersionString", ""))
    if not version.startswith("5.5"):
        parser.error(f"bridge was only verified with Renderer 5.5, installed version is {version}")
    if args.export_timeout <= 0:
        parser.error("--export-timeout must be positive")
    pid = renderer_pid()
    baseline = backup_settings(output_dir / "persistence-before.db")
    report = {"adm": str(adm), "adm_sha256": file_sha256(adm),
              "renderer_version": version, "output_dir": str(output_dir), "pid": pid,
              "file_type": args.file_type,
              "requested_layouts": layouts, "outputs": [], "baseline_settings_sha256": baseline}
    failure = None
    with tempfile.TemporaryDirectory(prefix="dar-batch-", dir=ROOT / "local") as temporary:
        control_dir = Path(temporary)
        bridge = control_dir / f"{control_dir.name}.dylib"
        compile_bridge(bridge)
        session = Session(pid, bridge, control_dir)
        initial = None
        try:
            initial = session.inspect()
            if initial["otherExporterPresent"] or initial["exporter"] is not None:
                raise RuntimeError("close the existing re-render export dialog before running")
            rows = initial["config"]["rows"]
            if (len(rows) != 1 or rows[0]["mapped"] or rows[0]["layout"] not in CHANNELS
                    or rows[0]["groups"] != "All beds and objects" or rows[0]["hasInvalidGroups"]):
                raise RuntimeError("requires one unmapped 7.1.4/9.1.6 strip with all beds and objects")
            if initial["config"]["processing"]:
                raise RuntimeError("Renderer is already processing re-renders")
            report["initial"] = initial
            if initial["master"]["path"] != str(adm) or not initial["master"]["opened"]:
                session.action("open_master", path=str(adm))
                session.wait_for("ADM master", lambda state:
                                 state["master"]["opened"] and state["master"]["hasContent"]
                                 and state["master"]["path"] == str(adm))
            ordered = sorted(layouts, key=lambda item: item != rows[0]["layout"])
            for layout in ordered:
                print(f"导出 {layout} …", flush=True)
                switch_layout(session, layout)
                session.action("create_exporter")
                state = session.wait_for(f"{layout} exporter", lambda value:
                                         value["exporter"] is not None)
                exporter = state["exporter"]
                if len(exporter["rows"]) != 1 or exporter["rows"][0]["layout"] != layout:
                    raise RuntimeError(f"exporter row did not refresh to {layout}")
                saved = {key: exporter[key] for key in
                         ("outputDir", "exportName", "enableMultichannelOutput",
                          "enableNumberedMonoFiles", "enableTimeRange")}
                saved["selected"] = exporter["rows"][0]["selected"]
                session.exporter_original = saved
                multichannel = args.file_type == "interleaved"
                session.action("configure_exporter", outputDir=str(output_dir), multichannel=multichannel)
                configured = session.wait_for("output directory", lambda value:
                                              value["exporter"] is not None
                                              and value["exporter"]["outputDirString"] == str(output_dir)
                                              and not value["exporter"]["outputDirError"]
                                              and value["exporter"]["enableMultichannelOutput"] == multichannel
                                              and (multichannel or value["exporter"]["enableNumberedMonoFiles"])
                                              and not value["exporter"]["enableTimeRange"]
                                              and not value["exporter"]["isSelectionEmpty"])
                report.setdefault("export_configurations", []).append({
                    "layout": layout,
                    "output_dir": configured["exporter"]["outputDirString"],
                    "multichannel": configured["exporter"]["enableMultichannelOutput"],
                    "numbered_mono": configured["exporter"]["enableNumberedMonoFiles"],
                    "time_range": configured["exporter"]["enableTimeRange"],
                    "selected_rows": configured["exporter"]["rows"],
                })
                name = f"{prefix}-dar-{layout.replace('.', '')}"
                previous_files = set(output_dir.rglob("*.wav"))
                session.action("start_export", adm=str(adm), layout=layout,
                               outputDir=str(output_dir), name=name, multichannel=multichannel)
                expected_files = 1 if multichannel else CHANNELS[layout]
                files = finish_export(session, output_dir, previous_files, expected_files, args.export_timeout)
                if multichannel:
                    report["outputs"].append({"layout": layout,
                                              **wav_info(files[0], CHANNELS[layout])})
                else:
                    report["outputs"].append({"layout": layout, "channels": CHANNELS[layout],
                                              "files": [wav_info(path, 1) for path in files]})
                session.action("restore_exporter", **saved)
                session.wait_for("original export settings", lambda value:
                                 value["exporter"] is not None
                                 and value["exporter"]["exportName"] == saved["exportName"]
                                 and value["exporter"]["outputDir"] == saved["outputDir"]
                                 and value["exporter"]["enableMultichannelOutput"]
                                     == saved["enableMultichannelOutput"]
                                 and value["exporter"]["enableNumberedMonoFiles"]
                                     == saved["enableNumberedMonoFiles"]
                                 and value["exporter"]["enableTimeRange"] == saved["enableTimeRange"])
                session.action("release_exporter")
                session.wait_for("released exporter", lambda value: value["exporter"] is None)
                del session.exporter_original
            print("导出完成，恢复 Renderer 状态 …", flush=True)
        except Exception as exc:
            failure = str(exc)
        finally:
            if initial is not None:
                report["restore_errors"] = restore(session, initial, baseline)
                try:
                    report["final"] = session.inspect()
                except Exception as exc:
                    report["restore_errors"].append(f"final inspection: {exc}")
            report["action_trace"] = session.trace
            if hasattr(session, "close"):
                try:
                    report["transport_cleanup"] = session.close()
                except Exception as exc:
                    report.setdefault("restore_errors", []).append(f"transport cleanup: {exc}")
    report["settings_sha256_after"] = current_settings_hashes()
    report["error"] = failure
    if inspect_timeout_was_recovered(report):
        report["recovered_error"] = failure
        report["error"] = None
        failure = None
    report["success"] = (not failure and not report.get("restore_errors")
                         and report["settings_sha256_after"] == baseline)
    (output_dir / "run.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n",
                                         encoding="utf-8")
    if report["success"]:
        print(f"完成：{output_dir / 'run.json'}")
        return 0
    print(f"失败：{failure or report.get('restore_errors')}；详情：{output_dir / 'run.json'}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
