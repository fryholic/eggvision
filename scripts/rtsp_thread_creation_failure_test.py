#!/usr/bin/env python3
"""Verify deterministic RTSP owner-thread creation failure containment.

This script requires a ``-DEGGVISION_ENABLE_TEST_HOOKS=ON`` build. It proves that
recovery/cleanup owner creation failures are reported exactly once, unwind a
partially started camera/RTSP service with no outstanding lease, and do not
poison a subsequent healthy launch and ERROR recovery.
"""

from __future__ import annotations

import argparse
import os
import re
import socket
import subprocess
import tempfile
from pathlib import Path
from urllib.parse import urlsplit

from rtsp_error_recovery_test import (
    probe_until_rtp,
    stop_process,
    wait_for_listener,
)
from rtsp_lifecycle_test import RtspConnection


CRITICAL = re.compile(
    r"(?:GLib|GObject|GStreamer)[^\n]*CRITICAL|"
    r"assertion [^\n]* failed|Segmentation fault|"
    r"terminate called|\bAborted\b|\[fatal\]",
    re.IGNORECASE,
)


def assert_listener_closed(url: str) -> None:
    parsed = urlsplit(url)
    try:
        with socket.create_connection((parsed.hostname, parsed.port or 554), timeout=0.5):
            raise RuntimeError("failed startup left the RTSP listener open")
    except OSError:
        pass


def run_start_failure(
    app: Path,
    url: str,
    hook: str,
    evidence: str,
    expected_recovery_failures: int,
    timeout: float,
) -> None:
    with tempfile.TemporaryDirectory(prefix="eggvision-rtsp-thread-failure-") as directory:
        log_path = Path(directory) / "server.log"
        environment = os.environ.copy()
        environment[hook] = "1"
        environment["G_DEBUG"] = "fatal-criticals"
        with log_path.open("wb") as log_file:
            process = subprocess.Popen(
                [str(app), "--no-inference", "--no-event-recording", "--duration", "5"],
                stdout=log_file,
                stderr=subprocess.STDOUT,
                env=environment,
            )
        try:
            code = process.wait(timeout=timeout)
        finally:
            stop_process(process)

        log = log_path.read_text(encoding="utf-8", errors="replace")
        final = re.search(
            r"\[app\] startup failed outstanding=(\d+) .*"
            r"rtsp_errors=(\d+) .*rtsp_recovery_failures=(\d+)",
            log,
        )
        failures: list[str] = []
        if code != 3:
            failures.append(f"exit code was {code}, expected safe startup failure 3")
        if not final:
            failures.append("startup failure metrics are missing")
        else:
            outstanding, rtsp_errors, recovery_failures = map(int, final.groups())
            if outstanding != 0:
                failures.append(f"outstanding leases was {outstanding}, expected 0")
            if rtsp_errors != 1:
                failures.append(f"rtsp_errors was {rtsp_errors}, expected exactly 1")
            if recovery_failures != expected_recovery_failures:
                failures.append(
                    "rtsp_recovery_failures was "
                    f"{recovery_failures}, expected {expected_recovery_failures}"
                )
        if evidence not in log:
            failures.append(f"missing injected failure evidence: {evidence}")
        critical = CRITICAL.search(log)
        if critical:
            failures.append(f"fatal runtime diagnostic: {critical.group(0)}")
        assert_listener_closed(url)
        if failures:
            raise RuntimeError("; ".join(failures) + "\n--- server log ---\n" + log)
        print(
            f"thread failure contained hook={hook} exit=3 outstanding=0 "
            f"rtsp_errors=1 recovery_failures={expected_recovery_failures}",
            flush=True,
        )


def run_healthy_recovery(app: Path, url: str, duration: int, timeout: float) -> None:
    with tempfile.TemporaryDirectory(prefix="eggvision-rtsp-thread-relaunch-") as directory:
        root = Path(directory)
        trigger = root / "inject-error"
        log_path = root / "server.log"
        environment = os.environ.copy()
        environment["EGGVISION_RTSP_TEST_PUSH_ERROR_TRIGGER"] = str(trigger)
        environment["G_DEBUG"] = "fatal-criticals"
        with log_path.open("wb") as log_file:
            process = subprocess.Popen(
                [str(app), "--no-inference", "--no-event-recording",
                 "--duration", str(duration)],
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
            connection.wait_for_server_close()
            connection.close()
            connection = None
            probe_until_rtp(url, timeout)
            code = process.wait(timeout=duration + timeout)
        finally:
            if connection is not None:
                connection.close()
            stop_process(process)

        log = log_path.read_text(encoding="utf-8", errors="replace")
        final = re.search(
            r"\[app\] stopped .*\boutstanding=(\d+)\b.*"
            r"\brtsp_recoveries=(\d+)\b.*\brtsp_recovery_failures=(\d+)\b",
            log,
        )
        failures: list[str] = []
        if code != 0:
            failures.append(f"healthy relaunch exit code was {code}, expected 0")
        if not final:
            failures.append("healthy relaunch final state is missing")
        else:
            outstanding, recoveries, recovery_failures = map(int, final.groups())
            if outstanding != 0:
                failures.append(f"healthy relaunch outstanding was {outstanding}")
            if recoveries != 1 or recovery_failures != 0:
                failures.append(
                    f"healthy relaunch recoveries={recoveries} "
                    f"failures={recovery_failures}, expected 1/0"
                )
        for evidence in (
            "recovery teardown started: repeated appsrc push failure",
            "recovery completed; media cache cleared and listener resumed",
        ):
            if evidence not in log:
                failures.append(f"missing healthy recovery evidence: {evidence}")
        critical = CRITICAL.search(log)
        if critical:
            failures.append(f"fatal runtime diagnostic: {critical.group(0)}")
        if failures:
            raise RuntimeError("; ".join(failures) + "\n--- server log ---\n" + log)
        print(
            "healthy relaunch passed initial_rtp=yes fresh_rtp=yes "
            "recoveries=1 outstanding=0",
            flush=True,
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, default=Path("./build/eggvision_app"))
    parser.add_argument("--url", default="rtsp://127.0.0.1:8554/stream")
    parser.add_argument("--duration", type=int, default=12)
    parser.add_argument("--timeout", type=float, default=12.0)
    args = parser.parse_args()
    app = args.app.resolve()
    if not app.is_file() or args.duration < 8 or args.timeout <= 0:
        parser.error("valid hooks-enabled app, duration >= 8, and timeout are required")

    run_start_failure(
        app,
        args.url,
        "EGGVISION_RTSP_TEST_FAIL_RECOVERY_THREAD_CREATE",
        "recovery thread creation failed",
        1,
        args.timeout,
    )
    run_start_failure(
        app,
        args.url,
        "EGGVISION_RTSP_TEST_FAIL_CLEANUP_THREAD_CREATE",
        "session cleanup thread creation failed",
        0,
        args.timeout,
    )
    run_healthy_recovery(app, args.url, args.duration, args.timeout)
    print("RTSP thread creation failure test passed cases=3", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
