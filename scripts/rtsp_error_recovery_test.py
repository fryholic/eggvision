#!/usr/bin/env python3
"""Verify two-client shared-media ERROR recovery and a fresh RTP session.

The application must be configured with ``-DEGGVISION_ENABLE_TEST_HOOKS=ON``.
Normal builds compile the trigger hook out completely.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import urlsplit

from rtsp_lifecycle_test import RtspConnection


def wait_for_listener(url: str, process: subprocess.Popen[bytes], timeout: float) -> None:
    parsed = urlsplit(url)
    if parsed.scheme.lower() != "rtsp" or not parsed.hostname:
        raise ValueError(f"invalid RTSP URL: {url}")
    address = (parsed.hostname, parsed.port or 554)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited before listening with code {process.returncode}")
        try:
            with socket.create_connection(address, timeout=0.25):
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"RTSP listener did not open within {timeout:.1f}s")


def hold_until_recovery(url: str, timeout: float, ready: threading.Barrier) -> None:
    connection = RtspConnection(url, timeout)
    try:
        connection.start()
        ready.wait(timeout=timeout)
        connection.wait_for_server_close()
    finally:
        connection.close()


def probe_until_rtp(url: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        connection: RtspConnection | None = None
        try:
            connection = RtspConnection(url, min(3.0, max(0.5, deadline - time.monotonic())))
            connection.start()
            connection.teardown()
            return
        except Exception as error:  # noqa: BLE001 - retry spans listener restart.
            last_error = error
            time.sleep(0.2)
        finally:
            if connection is not None:
                connection.close()
    raise TimeoutError(f"fresh client did not receive RTP after recovery: {last_error}")


def stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def validate(log: str, return_code: int, expect_timeout: bool) -> None:
    failures: list[str] = []
    if return_code != 0:
        failures.append(f"server exit code was {return_code}, expected 0")
    final = re.search(
        r"\[app\] stopped .*\boutstanding=(\d+)\b.*"
        r"\bcapture_errors=(\d+)\b.*\brtsp_errors=(\d+)\b.*"
        r"\brtsp_recoveries=(\d+)\b.*\brtsp_recovery_failures=(\d+)\b",
        log,
    )
    if not final:
        failures.append("final application state is missing")
    else:
        outstanding, capture_errors, rtsp_errors, recoveries, recovery_failures = map(
            int, final.groups()
        )
        if outstanding != 0:
            failures.append(f"outstanding leases was {outstanding}, expected 0")
        if capture_errors != 0:
            failures.append(f"capture_errors was {capture_errors}, expected 0")
        if rtsp_errors < 3 + int(expect_timeout):
            failures.append(f"rtsp_errors was {rtsp_errors}, expected injected errors")
        if recoveries != 1:
            failures.append(f"rtsp_recoveries was {recoveries}, expected 1")
        expected_failures = int(expect_timeout)
        if recovery_failures != expected_failures:
            failures.append(
                f"rtsp_recovery_failures was {recovery_failures}, "
                f"expected {expected_failures}"
            )
    for required in (
        "test hook injecting three appsrc push errors",
        "recovery teardown started: repeated appsrc push failure",
        "recovery completed; media cache cleared and listener resumed",
    ):
        if required not in log:
            failures.append(f"missing recovery evidence: {required}")
    timeout_evidence = "media did not reach UNPREPARED within 5 seconds"
    if expect_timeout and timeout_evidence not in log:
        failures.append("missing recovery deadline failure evidence")
    if not expect_timeout and timeout_evidence in log:
        failures.append("unexpected recovery deadline failure")
    critical = re.search(
        r"(?:GLib|GObject|GStreamer)[^\n]*CRITICAL|"
        r"assertion [^\n]* failed|Segmentation fault",
        log,
        re.IGNORECASE,
    )
    if critical:
        failures.append(f"critical runtime diagnostic: {critical.group(0)}")
    if failures:
        tail = "\n".join(log.splitlines()[-60:])
        raise RuntimeError("; ".join(failures) + f"\n--- server log tail ---\n{tail}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, default=Path("./build/eggvision_app"))
    parser.add_argument("--url", default="rtsp://127.0.0.1:8554/stream")
    parser.add_argument("--duration", type=int, default=12)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument(
        "--teardown-delay-ms",
        type=int,
        default=0,
        help="test-only delay before the teardown worker starts GStreamer cleanup",
    )
    args = parser.parse_args()
    app = args.app.resolve()
    if not app.is_file():
        parser.error(f"application does not exist: {app}")
    if args.duration < 8 or args.timeout <= 0 or args.teardown_delay_ms < 0:
        parser.error("duration must be at least 8 and timeout must be positive")

    with tempfile.TemporaryDirectory(prefix="eggvision-rtsp-recovery-") as directory:
        root = Path(directory)
        trigger = root / "inject-error"
        log_path = root / "server.log"
        environment = os.environ.copy()
        environment["EGGVISION_RTSP_TEST_PUSH_ERROR_TRIGGER"] = str(trigger)
        if args.teardown_delay_ms:
            environment["EGGVISION_RTSP_TEST_TEARDOWN_DELAY_MS"] = str(
                args.teardown_delay_ms
            )
        environment["G_DEBUG"] = "fatal-criticals"
        with log_path.open("wb") as log_file:
            process = subprocess.Popen(
                [str(app), "--no-inference", "--duration", str(args.duration)],
                stdout=log_file,
                stderr=subprocess.STDOUT,
                env=environment,
            )

        ready = threading.Barrier(3)
        executor = concurrent.futures.ThreadPoolExecutor(max_workers=2)
        futures: list[concurrent.futures.Future[None]] = []
        return_code = -1
        try:
            wait_for_listener(args.url, process, args.timeout)
            futures = [
                executor.submit(hold_until_recovery, args.url, args.timeout, ready)
                for _ in range(2)
            ]
            ready.wait(timeout=args.timeout)
            triggered_at = time.monotonic()
            trigger.touch()
            if args.teardown_delay_ms > 5000:
                deadline = time.monotonic() + args.timeout
                evidence = "media did not reach UNPREPARED within 5 seconds"
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError("server exited while recovery deadline was pending")
                    current_log = log_path.read_text(encoding="utf-8", errors="replace")
                    if evidence in current_log:
                        elapsed = time.monotonic() - triggered_at
                        if elapsed < 4.5 or elapsed > 6.5:
                            raise RuntimeError(
                                f"recovery deadline recorded after {elapsed:.2f}s, expected about 5s"
                            )
                        break
                    time.sleep(0.1)
                else:
                    raise TimeoutError("recovery deadline was not evaluated")

                parsed = urlsplit(args.url)
                try:
                    with socket.create_connection(
                        (parsed.hostname, parsed.port or 554), timeout=0.5
                    ):
                        raise RuntimeError("listener reopened before teardown/cache eviction")
                except OSError:
                    pass
            for future in futures:
                future.result(timeout=args.timeout)
            probe_until_rtp(args.url, args.timeout)
            return_code = process.wait(timeout=args.duration + args.timeout)
        finally:
            stop_process(process)
            executor.shutdown(wait=True, cancel_futures=True)

        log = log_path.read_text(encoding="utf-8", errors="replace")
        validate(log, return_code, args.teardown_delay_ms > 5000)
        print(
            "RTSP error recovery test passed clients=2 fresh_rtp=yes "
            f"recoveries=1 recovery_failures={int(args.teardown_delay_ms > 5000)} "
            "outstanding=0",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
