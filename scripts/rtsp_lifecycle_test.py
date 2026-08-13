#!/usr/bin/env python3
"""Exercise hostile RTSP client lifecycles without third-party packages.

The test deliberately pauses, seeks, and drops TCP connections without sending
TEARDOWN.  Every abusive session is followed by a clean probe that must receive
RTP data.  A server that leaves a shared GstRTSPMedia unprepared typically
fails the probe with RTSP 503.
"""

from __future__ import annotations

import argparse
import socket
import sys
import time
from dataclasses import dataclass
from urllib.parse import urlsplit


@dataclass
class Response:
    code: int
    reason: str
    headers: dict[str, str]
    body: bytes


class RtspConnection:
    def __init__(self, url: str, timeout: float) -> None:
        parsed = urlsplit(url)
        if parsed.scheme.lower() != "rtsp" or not parsed.hostname:
            raise ValueError(f"invalid RTSP URL: {url}")
        self.url = url
        self.host = parsed.hostname
        self.port = parsed.port or 554
        self.timeout = timeout
        self.socket = socket.create_connection((self.host, self.port), timeout)
        self.socket.settimeout(timeout)
        self.buffer = bytearray()
        self.cseq = 1
        self.session = ""

    def close(self) -> None:
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.socket.close()

    def _fill(self, size: int = 1) -> None:
        while len(self.buffer) < size:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise ConnectionError("RTSP connection closed by server")
            self.buffer.extend(chunk)

    def _discard_interleaved(self) -> int:
        self._fill(4)
        if self.buffer[0] != ord("$"):
            return 0
        payload_size = int.from_bytes(self.buffer[2:4], "big")
        self._fill(4 + payload_size)
        del self.buffer[: 4 + payload_size]
        return payload_size

    def _read_response(self) -> Response:
        while True:
            self._fill()
            if self.buffer[0] == ord("$"):
                self._discard_interleaved()
                continue
            break

        marker = b"\r\n\r\n"
        while marker not in self.buffer:
            self._fill(len(self.buffer) + 1)
        header_end = self.buffer.index(marker) + len(marker)
        raw_header = bytes(self.buffer[:header_end])
        lines = raw_header.decode("utf-8", "replace").split("\r\n")
        status = lines[0].split(" ", 2)
        if len(status) < 2 or status[0] != "RTSP/1.0":
            raise RuntimeError(f"invalid RTSP response: {lines[0]!r}")
        headers: dict[str, str] = {}
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()
        content_length = int(headers.get("content-length", "0"))
        self._fill(header_end + content_length)
        body = bytes(self.buffer[header_end : header_end + content_length])
        del self.buffer[: header_end + content_length]
        return Response(
            code=int(status[1]),
            reason=status[2] if len(status) == 3 else "",
            headers=headers,
            body=body,
        )

    def request(
        self,
        method: str,
        url: str | None = None,
        headers: dict[str, str] | None = None,
    ) -> Response:
        request_headers = {
            "CSeq": str(self.cseq),
            "User-Agent": "bsaps-rtsp-lifecycle-test/1.0",
        }
        self.cseq += 1
        if self.session:
            request_headers["Session"] = self.session
        if headers:
            request_headers.update(headers)
        target = url or self.url
        message = f"{method} {target} RTSP/1.0\r\n"
        message += "".join(f"{name}: {value}\r\n" for name, value in request_headers.items())
        message += "\r\n"
        self.socket.sendall(message.encode("ascii"))
        return self._read_response()

    @staticmethod
    def require(response: Response, expected: set[int], operation: str) -> None:
        if response.code not in expected:
            raise RuntimeError(
                f"{operation} failed: RTSP {response.code} {response.reason}"
            )

    def _control_url(self, describe: Response) -> str:
        control = ""
        for line in describe.body.decode("utf-8", "replace").splitlines():
            if line.startswith("a=control:"):
                candidate = line.removeprefix("a=control:").strip()
                if candidate != "*":
                    control = candidate
                    break
        if not control:
            raise RuntimeError("DESCRIBE response has no media control URL")
        if control.startswith("rtsp://"):
            return control
        parsed = urlsplit(self.url)
        if control.startswith("/"):
            return f"rtsp://{parsed.netloc}{control}"
        base = describe.headers.get("content-base", self.url)
        return f"{base.rstrip('/')}/{control.lstrip('/')}"

    def start(self) -> None:
        self.require(self.request("OPTIONS"), {200}, "OPTIONS")
        describe = self.request("DESCRIBE", headers={"Accept": "application/sdp"})
        self.require(describe, {200}, "DESCRIBE")
        setup = self.request(
            "SETUP",
            self._control_url(describe),
            {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"},
        )
        self.require(setup, {200}, "SETUP")
        session = setup.headers.get("session", "").split(";", 1)[0]
        if not session:
            raise RuntimeError("SETUP response has no Session header")
        self.session = session
        self.require(self.request("PLAY"), {200}, "PLAY")
        self.wait_for_rtp()

    def wait_for_rtp(self) -> None:
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            self._fill()
            if self.buffer[0] == ord("$") and self._discard_interleaved() > 0:
                return
            if self.buffer.startswith(b"RTSP/1.0"):
                self._read_response()
        raise TimeoutError("no interleaved RTP packet received")

    def teardown(self) -> None:
        self.require(self.request("TEARDOWN"), {200}, "TEARDOWN")


def abusive_session(url: str, timeout: float, mode: str) -> None:
    connection = RtspConnection(url, timeout)
    try:
        connection.start()
        if mode == "pause":
            connection.require(connection.request("PAUSE"), {200}, "PAUSE")
            connection.require(connection.request("PLAY"), {200}, "PLAY after PAUSE")
            connection.wait_for_rtp()
        elif mode == "seek":
            # A live source may reject the range.  The important invariant is
            # that the request must not poison subsequent sessions.
            response = connection.request("PLAY", headers={"Range": "npt=0-"})
            connection.require(response, {200, 457, 501, 551}, "live seek")
        elif mode != "drop":
            raise ValueError(f"unknown abuse mode: {mode}")
        # Deliberately close without TEARDOWN, matching a killed client or a
        # playlist navigation action in a desktop player.
    finally:
        connection.close()


def clean_probe(url: str, timeout: float) -> None:
    connection = RtspConnection(url, timeout)
    try:
        connection.start()
        connection.teardown()
    finally:
        connection.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("url")
    parser.add_argument("--cycles", type=int, default=30)
    parser.add_argument("--settle-ms", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()
    if args.cycles < 1 or args.settle_ms < 0 or args.timeout <= 0:
        parser.error("cycles and timeout must be positive; settle-ms must be non-negative")

    modes = ("drop", "pause", "seek")
    started = time.monotonic()
    mode = "not-started"
    index = 0
    phase = "startup"
    try:
        for index in range(args.cycles):
            mode = modes[index % len(modes)]
            phase = "abusive session"
            abusive_session(args.url, args.timeout, mode)
            if args.settle_ms:
                time.sleep(args.settle_ms / 1000.0)
            phase = "reconnect probe"
            clean_probe(args.url, args.timeout)
            print(
                f"cycle={index + 1}/{args.cycles} mode={mode} "
                f"settle_ms={args.settle_ms} status=passed",
                flush=True,
            )
    except Exception as error:  # noqa: BLE001 - command-line test reports context.
        print(
            f"FAILED: cycle={index + 1}/{args.cycles} mode={mode} "
            f"phase={phase}: {type(error).__name__}: {error}",
            file=sys.stderr,
        )
        return 1
    elapsed = time.monotonic() - started
    print(f"rtsp lifecycle test passed cycles={args.cycles} elapsed={elapsed:.2f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
