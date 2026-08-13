#!/usr/bin/env python3
"""Deterministically stop before, and while, watchdog recovery starts."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path

from rtsp_error_recovery_test import stop_process, wait_for_listener
from rtsp_lifecycle_test import RtspConnection


def wait_for_log(path: Path, evidence: str, process: subprocess.Popen[bytes], timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        log = path.read_text(encoding="utf-8", errors="replace")
        if evidence in log:
            return log
        if process.poll() is not None:
            raise RuntimeError(f"server exited before {evidence!r}: code={process.returncode}")
        time.sleep(0.02)
    raise TimeoutError(f"server log did not contain {evidence!r}")


def validate(log: str, code: int, phase: str, iteration: int) -> None:
    failures: list[str] = []
    if code != 0:
        failures.append(f"exit code was {code}, expected 0")
    final = re.search(
        r"\[app\] stopped .*\boutstanding=(\d+)\b.*\brtsp_recoveries=(\d+)\b",
        log,
    )
    if not final:
        failures.append("final application state is missing")
    elif tuple(map(int, final.groups())) != (0, 0):
        failures.append(f"final outstanding/recoveries was {final.groups()}, expected 0/0")
    for evidence in (
        "test hook injecting three appsrc push errors",
        "recovery request pending token=",
        "pending recovery cancelled by stop token=",
    ):
        if evidence not in log:
            failures.append(f"missing evidence: {evidence}")
    phase_evidence = (
        "test hook holding pending recovery before watchdog"
        if phase == "pending"
        else "test hook watchdog recovery entered; delaying"
    )
    if phase_evidence not in log:
        failures.append(f"missing phase evidence: {phase_evidence}")
    if "recovery teardown started" in log:
        failures.append("a recovery worker started after stop won ownership")
    critical = re.search(
        r"(?:GLib|GObject|GStreamer)[^\n]*CRITICAL|assertion [^\n]* failed|Segmentation fault",
        log,
        re.IGNORECASE,
    )
    if critical:
        failures.append(f"critical runtime diagnostic: {critical.group(0)}")
    if failures:
        raise RuntimeError(
            f"{phase} iteration {iteration}: " + "; ".join(failures)
            + "\n--- log tail ---\n" + "\n".join(log.splitlines()[-80:])
        )


def run_case(app: Path, url: str, timeout: float, phase: str, iteration: int) -> None:
    with tempfile.TemporaryDirectory(prefix=f"bsaps-rtsp-{phase}-stop-") as directory:
        root = Path(directory)
        trigger = root / "inject-error"
        pause = root / "pause-recovery"
        log_path = root / "server.log"
        environment = os.environ.copy()
        environment["BSAPS_RTSP_TEST_PUSH_ERROR_TRIGGER"] = str(trigger)
        environment["G_DEBUG"] = "fatal-criticals"
        if phase == "pending":
            pause.touch()
            environment["BSAPS_RTSP_TEST_RECOVERY_PAUSE"] = str(pause)
        else:
            environment["BSAPS_RTSP_TEST_WATCHDOG_RECOVERY_DELAY_MS"] = "1500"
        with log_path.open("wb") as log_file:
            process = subprocess.Popen(
                [str(app), "--no-inference", "--duration", "30"],
                stdout=log_file,
                stderr=subprocess.STDOUT,
                env=environment,
            )
        connection: RtspConnection | None = None
        try:
            wait_for_listener(url, process, timeout)
            connection = RtspConnection(url, timeout)
            connection.start()
            trigger.touch()
            evidence = (
                "test hook holding pending recovery before watchdog"
                if phase == "pending"
                else "test hook watchdog recovery entered; delaying"
            )
            wait_for_log(log_path, evidence, process, timeout)
            process.terminate()
            connection.wait_for_server_close()
            code = process.wait(timeout=timeout)
        finally:
            if connection is not None:
                connection.close()
            stop_process(process)
        log = log_path.read_text(encoding="utf-8", errors="replace")
        validate(log, code, phase, iteration)
        print(f"recovery-stop phase={phase} iteration={iteration} passed", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, default=Path("./build/bsaps_app"))
    parser.add_argument("--url", default="rtsp://127.0.0.1:8554/stream")
    parser.add_argument("--iterations", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    app = args.app.resolve()
    if not app.is_file() or args.iterations < 1 or args.timeout <= 0:
        parser.error("valid app, positive iterations, and timeout are required")
    for phase in ("pending", "watchdog"):
        for iteration in range(1, args.iterations + 1):
            run_case(app, args.url, args.timeout, phase, iteration)
    print(f"RTSP recovery/stop race test passed cases={args.iterations * 2}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
