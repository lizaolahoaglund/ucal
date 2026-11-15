"""Opt-in physical USB refresh acceptance against an isolated Switch instance.

The package supplies authorized backend/vendor DLLs. No hardware name, serial,
Hub port, vendor discovery command, or window handle is built into this script.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid


def configuration(path: Path) -> dict:
    state = json.loads(path.read_text(encoding="utf-8-sig"))
    return {key: state[key] for key in ("applications", "projects", "activeProjectId", "softwareBindings")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--switch", type=Path, required=True, help="Packaged UcalManager.exe with the backend DLLs to test")
    parser.add_argument("--keyword", required=True, help="Keyword matching only the adapter under test")
    parser.add_argument("--expected-endpoints", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New or empty evidence directory")
    parser.add_argument("--seed-workspace", type=Path, help="Optional profile data copied into this test's private workspace")
    parser.add_argument("--backend-runtime-overrides", type=Path,
                        help="Station JSON mapping backend IDs to original vendor runtime paths")
    parser.add_argument("--power-hook", type=Path, help="Optional station-owned PowerShell script: param($Action), Disconnect/Reconnect")
    parser.add_argument("--rapid-reconnect", action="store_true", help="Also exercise a rapid change; requires an automatic station hook")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This acceptance requires a Windows desktop and real hardware.")
    if args.expected_endpoints < 1 or args.timeout <= 0:
        parser.error("Endpoint count and timeout must be positive.")
    if args.rapid_reconnect and not args.power_hook:
        parser.error("Rapid reconnect requires --power-hook; manual timing is not deterministic.")
    executable = args.switch.resolve(strict=True)
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("Output must be new or empty; existing evidence is never removed.")
    output.mkdir(parents=True, exist_ok=True)
    settings = output / "settings"
    settings.mkdir()
    if args.backend_runtime_overrides:
        overrides = json.loads(args.backend_runtime_overrides.read_text(encoding="utf-8-sig"))
        if not isinstance(overrides, dict):
            parser.error("Runtime overrides must be a backend-ID/path object.")
        import re
        backend_settings = settings / "backend-plugins"
        backend_settings.mkdir()
        for backend_id, runtime in overrides.items():
            if not re.fullmatch(r"[a-z0-9-]+", backend_id) or not (
                    executable.parent / "backends" / backend_id / "backend.json").is_file():
                parser.error(f"Unknown backend override: {backend_id}")
            runtime_path = str(Path(runtime).resolve(strict=True))
            if "\r" in runtime_path or "\n" in runtime_path:
                parser.error("Runtime paths must fit one INI value.")
            (backend_settings / (backend_id + ".ini")).write_text(
                "[runtime]\nlibrary=" + runtime_path + "\n", encoding="utf-16")
    workspace = settings / "profiles.json"
    if args.seed_workspace:
        # Parse before copying; only test-owned state will be edited/refreshed.
        value = json.loads(args.seed_workspace.read_text(encoding="utf-8-sig"))
        workspace.write_text(json.dumps(value), encoding="utf-8")
    hook = args.power_hook.resolve(strict=True) if args.power_hook else None
    import psutil
    from pywinauto import Desktop
    import win32con
    import win32gui
    import win32process

    environment = {k: v for k, v in os.environ.items() if not k.startswith("UCAL_")}
    for key in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM"):
        environment.pop(key, None)
    environment.update(UCAL_MANAGER_SETTINGS_DIRECTORY=str(settings), UCAL_BROKER_INSTANCE="ucal_usb_test_" + uuid.uuid4().hex)
    process = subprocess.Popen([str(executable), "--language", "en", "--page", "hardware"],
                               cwd=executable.parent, env=environment, creationflags=subprocess.CREATE_NO_WINDOW)
    owner = psutil.Process(process.pid)
    report = {"schema": "org.ucal.manager-usb-acceptance/1", "status": "failed",
              "processId": process.pid, "expectedEndpoints": args.expected_endpoints,
              "steps": [], "powerControl": "station hook" if hook else "manual"}
    window = None
    restored = False

    def focus():
        # Only a fresh, enabled top-level window of this exact owned process.
        handles = []
        win32gui.EnumWindows(lambda h, _: handles.append(h) if
                             win32process.GetWindowThreadProcessId(h)[1] == process.pid and
                             win32gui.IsWindowVisible(h) and win32gui.IsWindowEnabled(h) else None, None)
        if not handles:
            raise RuntimeError("Owned Switch window is not ready, or a dialog is blocking it.")
        Desktop(backend="win32").window(handle=handles[0]).set_focus()
        until = time.monotonic() + 2
        while time.monotonic() < until:
            if win32process.GetWindowThreadProcessId(win32gui.GetForegroundWindow())[1] == process.pid:
                return
            time.sleep(.05)
        raise RuntimeError("The test window did not acquire foreground; no input was sent.")

    def states():
        labels = [x.window_text() for x in window.descendants(control_type="Text")]
        return labels.count("Online"), labels.count("Offline")

    def wait_for(expected, label):
        started = time.monotonic()
        samples = []
        while time.monotonic() - started < args.timeout:
            if process.poll() is not None:
                raise RuntimeError(f"Switch exited with {process.returncode}")
            value = states()
            samples.append([round(time.monotonic() - started, 3), *value])
            if value == expected:
                focus()
                picture = window.capture_as_image()
                if win32process.GetWindowThreadProcessId(win32gui.GetForegroundWindow())[1] != process.pid:
                    raise RuntimeError("Foreground changed during capture; screenshot discarded.")
                picture.save(output / (label + ".png"))
                report["steps"].append({"id": label, "samples": samples})
                return
            time.sleep(.25)
        raise RuntimeError(f"{label}: expected {expected}, observed {samples[-1:]}")

    def power(action):
        if hook:
            powershell = Path(os.environ["WINDIR"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
            result = subprocess.run([str(powershell), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(hook),
                                     "-Action", action], capture_output=True, timeout=15)
            (output / ("power-" + action + ".log")).write_bytes(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError(f"Power hook {action} failed ({result.returncode}).")
        else:
            input(f"{action} ONLY the adapter matching {args.keyword!r}, then press Enter: ")

    try:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            handles = []
            win32gui.EnumWindows(lambda h, _: handles.append(h) if
                                 win32process.GetWindowThreadProcessId(h)[1] == process.pid and
                                 win32gui.GetWindowText(h) == "UCAL Manager" and win32gui.IsWindowVisible(h) else None, None)
            if handles:
                window = Desktop(backend="uia").window(handle=handles[0])
                break
            if process.poll() is not None:
                raise RuntimeError("Switch exited before its main window appeared.")
            time.sleep(.1)
        if window is None:
            raise RuntimeError("Switch main window did not appear.")
        # The current page is selected by a supported launch argument. No
        # coordinate click, rescan button, injected message or vendor command.
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            edits = [x for x in window.descendants(control_type="Edit")
                     if x.element_info.name == "Filter hardware channels"]
            if edits:
                edits[0].set_edit_text(args.keyword)
                break
            time.sleep(.1)
        else:
            raise RuntimeError("Hardware filter was not found in the English product UI.")
        wait_for((args.expected_endpoints, 0), "initial-online")
        baseline = configuration(workspace)
        power("Disconnect")
        wait_for((0, args.expected_endpoints), "disconnected")
        power("Reconnect")
        restored = True
        wait_for((args.expected_endpoints, 0), "reconnected")
        if args.rapid_reconnect:
            restored = False
            power("Disconnect")
            time.sleep(.8)
            power("Reconnect")
            restored = True
            wait_for((args.expected_endpoints, 0), "rapid-reconnected")
        time.sleep(3)
        if states() != (args.expected_endpoints, 0):
            raise RuntimeError("A late scan overwrote the reconnected state.")
        if configuration(workspace) != baseline:
            raise RuntimeError("Application/project/software configuration changed during hardware refresh.")
        report.update(status="passed", configurationPreserved=True)
    except Exception as error:
        report["error"] = str(error)
    finally:
        if not restored and report["steps"] and report["steps"][0]["id"] == "initial-online":
            try:
                power("Reconnect")
            except Exception as error:
                report["restoreError"] = str(error)
        try:
            children = owner.children(recursive=True)
            if process.poll() is None and window is not None:
                tid, pid = win32process.GetWindowThreadProcessId(window.handle)
                if pid == process.pid:
                    win32gui.PostThreadMessage(tid, win32con.WM_QUIT, 0, 0)
                try:
                    process.wait(15)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(5)
            elif process.poll() is None:
                process.terminate()
                process.wait(5)
            for child in children:
                try:
                    if child.is_running() and child.name() in ("UcalBroker.exe", "UcalJ2534DiscoveryHost.exe"):
                        child.terminate()
                        child.wait(5)
                except psutil.NoSuchProcess:
                    pass
        except Exception as error:
            report["cleanupError"] = str(error)
            report["status"] = "failed"
        (output / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
