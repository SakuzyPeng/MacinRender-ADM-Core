"""One LLDB owner for headless Renderer exports and bounded gain-path tracing."""

import hashlib
import json
import struct
import sys
import time
import traceback
from pathlib import Path

import lldb

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
import run_headless_rerender as rerender

_active = None
_base_session = rerender.Session


def breakpoint_callback(frame, location, internal_dict):
    if _active is not None:
        _active.capture(frame, location)
    return False


class TraceDriver:
    def __init__(self, debugger, config):
        self.debugger = debugger
        self.config = config
        self.target = debugger.GetSelectedTarget()
        self.process = self.target.GetProcess()
        self.breakpoints = {}
        self.counts = {}
        self.serial = 0
        self.current_action = "setup"
        self.loaded = {}
        self.target_stops = {}
        self.records = Path(config["trace_file"])
        self.records.parent.mkdir(parents=True, exist_ok=True)
        if self.records.exists():
            raise RuntimeError("trace file already exists")
        self.stream = self.records.open("x", encoding="utf-8")
        self.module = self.target.FindModule(self.target.GetExecutable())
        actual_uuid = (self.module.GetUUIDString() or "").upper()
        if actual_uuid != config["module_uuid"].upper():
            raise RuntimeError(f"Renderer UUID mismatch: {actual_uuid}")
        if rerender.file_sha256(rerender.APP) != config["binary_sha256"]:
            raise RuntimeError("Renderer executable hash differs from the locator manifest")
        self.load_base = self.module.GetObjectFileHeaderAddress().GetLoadAddress(self.target)
        self.image_base = int(config["image_base"], 0)
        self.saved_async = debugger.GetAsync()
        debugger.SetAsync(True)
        for region in config.get("snapshots", []):
            virtual = int(region["address"], 0)
            size = int(region["size"])
            if size <= 0 or size > 64 * 1024 * 1024:
                raise RuntimeError("snapshot region exceeds the bounded module limit")
            pointer = self.load_base + virtual - self.image_base
            error = lldb.SBError()
            data = self.process.ReadMemory(pointer, size, error)
            if error.Fail() or len(data) != size:
                raise RuntimeError(f"could not read runtime code region {region}: {error}")
            output = self.records.parent / f"runtime-{virtual:x}.bin"
            if output.exists():
                raise RuntimeError(f"runtime snapshot already exists: {output}")
            output.write_bytes(data)
            self.emit({"event": "snapshot", "address": hex(virtual), "size": size,
                       "path": str(output), "sha256": hashlib.sha256(data).hexdigest()})
        for item in config.get("breakpoints", []):
            virtual = int(item["address"], 0)
            bp = self.target.BreakpointCreateByAddress(self.load_base + virtual - self.image_base)
            bp.SetScriptCallbackFunction(__name__ + ".breakpoint_callback")
            self.breakpoints[bp.GetID()] = {"spec": item, "kind": "entry"}
            self.counts[item["name"]] = 0
        self.emit({"event": "start", "pid": self.process.GetProcessID(), "module_uuid": actual_uuid,
                   "binary_sha256": config["binary_sha256"], "load_base": hex(self.load_base),
                   "image_base": hex(self.image_base), "breakpoints": config.get("breakpoints", [])})

    def emit(self, row):
        self.stream.write(json.dumps(row, ensure_ascii=False) + "\n")
        self.stream.flush()

    def registers(self, frame):
        return {name: frame.FindRegister(name).GetValueAsUnsigned()
                for name in [f"x{i}" for i in range(9)] + ["x19", "x20", "x21", "x22", "x29", "x30", "sp"]}

    def memory(self, registers, spec):
        result = {}
        for register, size in spec.items():
            pointer = int(registers.get(register, 0))
            size = min(int(size), 4096)
            if pointer < 65536 or size <= 0:
                continue
            error = lldb.SBError()
            value = self.process.ReadMemory(pointer, size, error)
            result[register] = {"address": hex(pointer), "size": size,
                                "bytes": value.hex() if error.Success() else None,
                                "error": None if error.Success() else str(error)}
        return result

    def regions(self, registers, specs):
        result = {}
        for spec in specs:
            try:
                pointer = int(registers.get(spec["register"], 0))
                for offset in spec.get("pointer_offsets", []):
                    error = lldb.SBError()
                    raw = self.process.ReadMemory(pointer + int(offset), 8, error)
                    if error.Fail():
                        raise RuntimeError(str(error))
                    pointer = struct.unpack("<Q", raw)[0]
                pointer += int(spec.get("offset", 0))
                size = min(int(spec["bytes"]), 4096)
                if pointer < 65536:
                    raise RuntimeError("null or low address")
                error = lldb.SBError()
                raw = self.process.ReadMemory(pointer, size, error)
                if error.Fail():
                    raise RuntimeError(str(error))
                result[spec["name"]] = {"address": hex(pointer), "bytes": raw.hex()}
            except Exception as error:
                result[spec["name"]] = {"error": str(error)}
        return result

    def capture(self, frame, location):
        try:
            identifier = location.GetBreakpoint().GetID()
            context = self.breakpoints.get(identifier)
            if context is None:
                return
            spec = context["spec"]
            if spec.get("phases") and self.current_action not in spec["phases"]:
                return
            if context["kind"] == "entry":
                if self.counts[spec["name"]] >= int(spec.get("max_hits", 8)):
                    location.GetBreakpoint().SetEnabled(False)
                    return
                self.counts[spec["name"]] += 1
            self.serial += 1
            registers = self.registers(frame)
            thread = frame.GetThread()
            stack = []
            for index in range(min(thread.GetNumFrames(), 18)):
                caller = thread.GetFrameAtIndex(index)
                addr = caller.GetPCAddress()
                module = addr.GetModule()
                stack.append({"index": index, "pc": hex(caller.GetPC()),
                              "file_address": hex(addr.GetFileAddress()),
                              "module": module.GetFileSpec().GetFilename() if module.IsValid() else None,
                              "function": caller.GetFunctionName()})
            row = {"event": context["kind"], "sequence": self.serial, "name": spec["name"],
                   "action": self.current_action,
                   "entry_sequence": context.get("entry_sequence"), "thread_id": thread.GetThreadID(),
                   "pc": hex(frame.GetPC()), "registers": {k: hex(v) for k, v in registers.items()},
                   "float_registers": {f"s{i}": frame.FindRegister(f"s{i}").GetValue() for i in range(8)},
                   "stack": stack, "memory": self.memory(registers, spec.get("memory", {}))}
            row["regions"] = self.regions(registers, spec.get("regions", []))
            if context["kind"] == "return":
                row["entry_memory"] = self.memory(context["entry_registers"], spec.get("return_memory", {}))
                row["entry_regions"] = self.regions(context["entry_registers"], spec.get("return_regions", []))
            self.emit(row)
            if context["kind"] == "entry" and spec.get("capture_return", False):
                return_pc = thread.GetFrameAtIndex(1).GetPC() if thread.GetNumFrames() > 1 else registers["x30"]
                if return_pc != lldb.LLDB_INVALID_ADDRESS:
                    bp = self.target.BreakpointCreateByAddress(return_pc)
                    bp.SetThreadID(thread.GetThreadID())
                    bp.SetOneShot(True)
                    bp.SetScriptCallbackFunction(__name__ + ".breakpoint_callback")
                    self.breakpoints[bp.GetID()] = {"spec": spec, "kind": "return",
                                                   "entry_sequence": self.serial,
                                                   "entry_registers": registers}
        except Exception:
            self.emit({"event": "callback_error", "error": traceback.format_exc()})

    def stop(self):
        state = self.process.GetState()
        if state in (lldb.eStateRunning, lldb.eStateStepping):
            error = self.process.Stop()
            if error.Fail():
                raise RuntimeError(f"could not stop Renderer: {error}")
            deadline = time.monotonic() + 10
            while self.process.GetState() not in (lldb.eStateStopped, lldb.eStateCrashed) and time.monotonic() < deadline:
                time.sleep(0.02)
        if self.process.GetState() != lldb.eStateStopped:
            raise RuntimeError(f"Renderer is not safely stopped: {self.process.GetState()}")
        main = next((thread for thread in self.process if thread.GetIndexID() == 1), None)
        if main is not None:
            self.process.SetSelectedThread(main)

    def service_target_stop(self):
        if self.process.GetState() != lldb.eStateStopped:
            return
        stopped = [thread for thread in self.process
                   if thread.GetStopReason() not in (lldb.eStopReasonNone, lldb.eStopReasonInvalid)]
        details = [{"thread_id": thread.GetThreadID(), "reason": thread.GetStopReason(),
                    "data": [thread.GetStopReasonDataAtIndex(index)
                             for index in range(thread.GetStopReasonDataCount())],
                    "description": thread.GetStopDescription(1024),
                    "pc": hex(thread.GetFrameAtIndex(0).GetPC())} for thread in stopped]
        # Darwin may report an unattributed watchpoint while resuming from an
        # expression call. No watchpoints are installed by this driver. Resume
        # the target's normal execution once per such site, keeping the event
        # visible; a repeated stop is an error rather than an unbounded retry.
        if stopped and self.target.GetNumWatchpoints() == 0 and all(
            thread.GetStopReason() == lldb.eStopReasonWatchpoint for thread in stopped
        ):
            key = tuple(item["pc"] for item in details)
            count = self.target_stops.get(key, 0) + 1
            self.target_stops[key] = count
            self.emit({"event": "unattributed_watchpoint_stop", "details": details, "occurrence": count})
            if count > 1 or sum(self.target_stops.values()) > 16:
                raise RuntimeError("unattributed watchpoint stop repeated")
            error = self.process.Continue()
            if error.Fail():
                raise RuntimeError(f"could not continue target stop: {error}")
            return
        if details:
            self.emit({"event": "unexpected_stop", "details": details})
            raise RuntimeError("Renderer stopped outside the configured trace callbacks")

    def call(self, address, signature, argument):
        options = lldb.SBExpressionOptions()
        options.SetLanguage(lldb.eLanguageTypeC_plus_plus)
        options.SetTimeoutInMicroSeconds(20_000_000)
        options.SetUnwindOnError(True)
        options.SetIgnoreBreakpoints(True)
        frame = self.process.GetSelectedThread().GetFrameAtIndex(0)
        result = frame.EvaluateExpression(f"(({signature}){address:#x})({argument})", options)
        if result.GetError().Fail():
            raise RuntimeError(f"bridge expression failed: {result.GetError()}")
        return result

    def load_bridge(self, path):
        path = str(path.resolve())
        if path in self.loaded:
            return self.loaded[path]
        symbols = self.target.FindSymbols("dlopen", lldb.eSymbolTypeCode)
        loader = next((item.GetSymbol().GetStartAddress().GetLoadAddress(self.target)
                       for item in symbols if item.GetSymbol().GetStartAddress().IsValid()), None)
        if loader is None:
            raise RuntimeError("dlopen was not found")
        handle = self.call(loader, "void*(*)(const char*,int)", f"{json.dumps(path)},2")
        if handle.GetValueAsUnsigned() == 0:
            raise RuntimeError("dlopen returned null for the batch bridge")
        module = next((item for item in self.target.modules
                       if str(Path(item.GetFileSpec().GetDirectory()) / item.GetFileSpec().GetFilename()) == path), None)
        if module is None:
            raise RuntimeError("loaded bridge module is not visible")
        queue = next((symbol.GetStartAddress().GetLoadAddress(self.target) for symbol in module
                      if symbol.GetName() == "dar_queue_batch_request"), None)
        if queue is None:
            raise RuntimeError("bridge queue function is missing")
        self.loaded[path] = queue
        return queue

    def close(self):
        try:
            self.stop()
            for identifier in self.breakpoints:
                self.target.BreakpointDelete(identifier)
            self.emit({"event": "end", "counts": self.counts})
            error = self.process.Detach()
            if error.Fail():
                self.emit({"event": "detach_error", "error": str(error)})
        finally:
            self.debugger.SetAsync(self.saved_async)
            self.stream.close()


class PersistentSession(_base_session):
    def action(self, action_name, **arguments):
        if self.pid != _active.process.GetProcessID() or rerender.renderer_pid() != self.pid:
            raise RuntimeError("Renderer process changed during the trace")
        self.serial += 1
        request = self.control_dir / f"{self.serial:03d}-{action_name}.json"
        response = self.control_dir / f"{self.serial:03d}-{action_name}-result.json"
        request.write_text(json.dumps({"action": action_name, "response": str(response), **arguments}) + "\n")
        _active.stop()
        _active.current_action = action_name
        queue = _active.load_bridge(self.bridge)
        queued = _active.call(queue, "bool(*)(const char*)", json.dumps(str(request)))
        if queued.GetValueAsUnsigned() == 0:
            raise RuntimeError(f"could not queue {action_name}")
        _active.emit({"event": "action", "action": action_name, "arguments": arguments})
        continued = _active.process.Continue()
        if continued.Fail():
            raise RuntimeError(f"could not resume Renderer: {continued}")
        deadline = time.monotonic() + 120
        began = time.monotonic()
        while not response.exists() and time.monotonic() < deadline:
            if _active.process.GetState() in (lldb.eStateExited, lldb.eStateCrashed, lldb.eStateDetached):
                raise RuntimeError(f"Renderer stopped unexpectedly during {action_name}")
            if time.monotonic() - began > 0.1:
                _active.service_target_stop()
            time.sleep(0.03)
        if not response.exists():
            raise RuntimeError(f"Renderer did not answer {action_name}")
        result = json.loads(response.read_text())
        self.trace.append({"action": action_name, "ok": result.get("ok", False), "error": result.get("error")})
        if not result.get("ok"):
            raise RuntimeError(f"Renderer {action_name}: {result.get('error')}")
        return result


def run(debugger, command, result, internal_dict):
    global _active
    driver = None
    saved_argv = sys.argv
    try:
        config = json.loads(Path(command.strip()).read_text())
        driver = TraceDriver(debugger, config)
        _active = driver
        if config.get("export_arguments"):
            rerender.Session = PersistentSession
            sys.argv = ["run_headless_rerender.py", *config["export_arguments"]]
            code = rerender.main()
            driver.emit({"event": "export_completed", "exit_code": code})
            print(json.dumps({"trace_finished": True, "export_exit_code": code}), flush=True)
        else:
            print(json.dumps({"trace_finished": True, "snapshot_only": True}), flush=True)
    except Exception:
        if driver is not None:
            driver.emit({"event": "driver_error", "error": traceback.format_exc()})
        print(json.dumps({"trace_finished": False, "error": traceback.format_exc()}), flush=True)
    finally:
        sys.argv = saved_argv
        rerender.Session = _base_session
        if driver is not None:
            driver.close()
        _active = None


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f dar_gain_lldb.run dar-gain-trace")
