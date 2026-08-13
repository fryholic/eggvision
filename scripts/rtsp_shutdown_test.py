#!/usr/bin/env python3
"""Verify active RTSP clients and zero-copy leases across application shutdown."""

from __future__ import annotations

import argparse
import concurrent.futures
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
        return_code = process.poll()
        if return_code is not None:
            raise RuntimeError(f"server exited before listening with code {return_code}")
        try:
            with socket.create_connection(address, timeout=0.25):
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"RTSP listener did not open within {timeout:.1f}s")


def hold_client(url: str, timeout: float, ready: threading.Barrier) -> None:
    connection = RtspConnection(url, timeout)
    try:
        connection.start()
        ready.wait(timeout=timeout)
        connection.wait_for_server_close()
    finally:
        connection.close()


def stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def validate_log(log: str, return_code: int) -> None:
    failures: list[str] = []
    if return_code != 0:
        failures.append(f"server exit code was {return_code}, expected 0")
    if not re.search(r"\[app\] stopped .*\boutstanding=0\b", log):
        failures.append("final application log does not report outstanding=0")

    capture_errors = [int(value) for value in re.findall(r'"capture_errors":(\d+)', log)]
    if not capture_errors:
        failures.append("no metrics record containing capture_errors was emitted")
    elif any(value != 0 for value in capture_errors):
        failures.append(f"capture_errors was non-zero: {capture_errors}")

    critical = re.search(
        r"(?:GLib|GObject|GStreamer)[^\n]*CRITICAL|"
        r"assertion [^\n]* failed|Segmentation fault",
        log,
        re.IGNORECASE,
    )
    if critical:
        failures.append(f"critical runtime diagnostic: {critical.group(0)}")

    if failures:
        tail = "\n".join(log.splitlines()[-40:])
        raise RuntimeError("; ".join(failures) + f"\n--- server log tail ---\n{tail}")


def run_case(
    app: Path,
    url: str,
    client_count: int,
    duration: int,
    startup_timeout: float,
) -> None:
    with tempfile.TemporaryDirectory(prefix="bsaps-rtsp-shutdown-") as directory:
        log_path = Path(directory) / "server.log"
        with log_path.open("wb") as log_file:
            process = subprocess.Popen(
                [str(app), "--no-inference", "--duration", str(duration)],
                stdout=log_file,
                stderr=subprocess.STDOUT,
            )

        ready = threading.Barrier(client_count + 1)
        client_timeout = duration + startup_timeout + 5.0
        executor = concurrent.futures.ThreadPoolExecutor(max_workers=client_count)
        futures: list[concurrent.futures.Future[None]] = []
        return_code = -1
        try:
            wait_for_listener(url, process, startup_timeout)
            futures = [
                executor.submit(hold_client, url, client_timeout, ready)
                for _ in range(client_count)
            ]
            ready.wait(timeout=startup_timeout)
            return_code = process.wait(timeout=duration + startup_timeout + 5.0)
            for future in futures:
                future.result(timeout=5.0)
        finally:
            stop_process(process)
            executor.shutdown(wait=True, cancel_futures=True)

        log = log_path.read_text(encoding="utf-8", errors="replace")
        validate_log(log, return_code)
        print(
            f"shutdown case passed clients={client_count} "
            f"exit_code={return_code} outstanding=0",
            flush=True,
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, default=Path("./build/bsaps_app"))
    parser.add_argument("--url", default="rtsp://127.0.0.1:8554/stream")
    parser.add_argument("--duration", type=int, default=8)
    parser.add_argument("--startup-timeout", type=float, default=10.0)
    parser.add_argument("--clients", type=int, nargs="+", default=[1, 2])
    args = parser.parse_args()

    app = args.app.resolve()
    if not app.is_file():
        parser.error(f"application does not exist: {app}")
    if args.duration < 2 or args.startup_timeout <= 0:
        parser.error("duration must be at least 2 and startup-timeout must be positive")
    if any(count < 1 for count in args.clients):
        parser.error("every client count must be positive")

    for client_count in args.clients:
        run_case(app, args.url, client_count, args.duration, args.startup_timeout)
    print(f"RTSP shutdown test passed cases={len(args.clients)}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
