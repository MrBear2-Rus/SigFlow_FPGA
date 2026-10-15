"""T2 tests: the bootstrap → ready handshake, driven exactly as the host does.

Every test here starts a **real** sidecar process (``python -m
sigflow_edu_agent --bootstrap-stdin``) and asserts one rule of T2
(接口文档 §3 + §6.1).  Nothing is mocked inside the process: the point is that
the process boundary -- stdin line, stdout ready line, exit codes, signals --
behaves the way ``AgentServiceController`` expects.

The rules under test:

1. an invalid bootstrap line exits **2** with **zero** bytes on stdout;
2. a bind failure exits **3**, stdout still empty;
3. the happy path writes exactly **one** ready line and never touches stdout again;
4. the ready line echoes ``nonce`` and reports the port that was really bound;
5. tokens and ``data_directory`` stay in memory (nothing leaks to stdout, stderr
   or the data directory);
6. ``SIGTERM``/``SIGINT`` and an authenticated ``POST /shutdown`` stop the
   process with exit code **0**;
7. ``type``/``protocol``/six non-empty fields/``gateway_url`` are validated;
8. the six ``--selftest-fail`` modes report the codes the host's restart logic
   keys on (``exit-before-ready``=7, ``exit-after-ready``=9, ...).
"""

from __future__ import annotations

import json
import os
import queue
import signal
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from typing import Any, Optional

AGENT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(AGENT_DIR, "src")
REPO_ROOT = os.path.dirname(AGENT_DIR)

sys.path.insert(0, SRC_DIR)

EXIT_BOOTSTRAP_ERROR = 2
EXIT_BIND_ERROR = 3
EXIT_SELFTEST_BEFORE_READY = 7
EXIT_SELFTEST_AFTER_READY = 9

READY_TIMEOUT_SECONDS = 15.0
STOP_TIMEOUT_SECONDS = 20.0

UI_TOKEN = "ui-t2-secret-token"
GATEWAY_TOKEN = "gw-t2-secret-token"
NONCE = "nonce-" + "b" * 32


def bootstrap_record(**overrides: Any) -> dict:
    """A valid bootstrap record; keyword arguments override single fields."""
    record: dict[str, Any] = {
        "type": "sigflow-bootstrap",
        "protocol": "edu.api.v1",
        "instance_id": "inst-t2",
        "nonce": NONCE,
        "gateway_url": "http://127.0.0.1:59999",
        "gateway_token": GATEWAY_TOKEN,
        "ui_token": UI_TOKEN,
        "data_directory": "",
    }
    record.update(overrides)
    return record


def free_port() -> int:
    """Return a port the OS just told us is free."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


class SidecarProcess:
    """One real sidecar process, observed the way the host observes it."""

    def __init__(
        self,
        line: Optional[bytes] = None,
        extra_args: tuple[str, ...] = (),
        data_directory: Optional[str] = None,
        bootstrap_stdin: bool = True,
    ) -> None:
        self.data_directory = data_directory or tempfile.mkdtemp(prefix="edu-agent-t2-")
        self._env = dict(os.environ)
        self._env["PYTHONPATH"] = SRC_DIR + os.pathsep + self._env.get("PYTHONPATH", "")
        # No model key: the run path must stay deterministic for these assertions.
        self._env.pop("SIGFLOW_EDU_MODEL_API_KEY", None)
        argv = [sys.executable, "-m", "sigflow_edu_agent"]
        if bootstrap_stdin:
            argv.append("--bootstrap-stdin")
        argv.extend(extra_args)
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=self._env,
            cwd=self.data_directory,
        )
        self.stdout = bytearray()
        self.stderr = bytearray()
        self._lines: "queue.Queue[bytes]" = queue.Queue()
        self._threads = [
            threading.Thread(
                target=self._pump, args=(self.proc.stdout.fileno(), self.stdout, self._lines),
                daemon=True,
            ),
            threading.Thread(
                target=self._pump, args=(self.proc.stderr.fileno(), self.stderr, None),
                daemon=True,
            ),
        ]
        for thread in self._threads:
            thread.start()
        if line is not None:
            self.proc.stdin.write(line)
            self.proc.stdin.flush()
        # ``--bootstrap-stdin`` without a line means "closed pipe": the sidecar
        # must treat the empty read as an invalid bootstrap record.
        self.proc.stdin.close()

    @staticmethod
    def _pump(fd: int, sink: bytearray, lines: Optional["queue.Queue[bytes]"]) -> None:
        partial = bytearray()
        while True:
            try:
                chunk = os.read(fd, 4096)
            except OSError:  # pragma: no cover - closed descriptor
                break
            if not chunk:
                break
            sink.extend(chunk)
            if lines is None:
                continue
            partial.extend(chunk)
            while True:
                index = partial.find(b"\n")
                if index < 0:
                    break
                lines.put(bytes(partial[: index + 1]))
                del partial[: index + 1]

    def ready_line(self, timeout: float = READY_TIMEOUT_SECONDS) -> Optional[bytes]:
        """Return the raw ready line, or ``None`` when none arrives in time."""
        try:
            return self._lines.get(timeout=timeout)
        except queue.Empty:
            return None

    def ready(self, timeout: float = READY_TIMEOUT_SECONDS) -> Optional[dict]:
        """Return the parsed ready record, or ``None``."""
        raw = self.ready_line(timeout)
        return None if raw is None else json.loads(raw.decode("utf-8"))

    def wait(self, timeout: float = STOP_TIMEOUT_SECONDS) -> int:
        """Wait for exit, killing the process if it overruns; return the code."""
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = self.proc.wait()
        for thread in self._threads:
            thread.join(timeout=2.0)
        return code

    def alive(self) -> bool:
        return self.proc.poll() is None

    def close(self) -> None:
        """Kill the process if it is still running and reap it."""
        if self.proc.poll() is None:
            self.proc.kill()
        self.wait(timeout=5.0)
        for stream in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            if stream is not None and not stream.closed:
                stream.close()
        shutil.rmtree(self.data_directory, ignore_errors=True)

    # -- HTTP -------------------------------------------------------------

    def http(
        self,
        method: str,
        path: str,
        port: int,
        body: Optional[Any] = None,
        token: Optional[str] = UI_TOKEN,
        timeout: float = 15.0,
    ) -> tuple[int, dict]:
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(
            "http://127.0.0.1:%d%s" % (port, path), data=data, method=method
        )
        if token is not None:
            request.add_header("Authorization", "Bearer " + token)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return int(response.status), json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            raw = error.read()
            try:
                return int(error.code), json.loads(raw.decode("utf-8"))
            except ValueError:  # pragma: no cover - non-JSON error body
                return int(error.code), {}


class SidecarTestCase(unittest.TestCase):
    """Owns one running sidecar per test and always reaps it."""

    def start(self, record: Optional[dict] = None, **kwargs: Any) -> SidecarProcess:
        directory = tempfile.mkdtemp(prefix="edu-agent-t2-")
        if record is None:
            record = bootstrap_record(data_directory=directory)
        line = (json.dumps(record) + "\n").encode("utf-8")
        sidecar = SidecarProcess(line=line, data_directory=directory, **kwargs)
        self.addCleanup(sidecar.close)
        return sidecar

    def stop(self, sidecar: SidecarProcess, port: int) -> None:
        """Ask the sidecar to stop the way the host does, then require exit 0.

        T2's rule is "an authenticated ``POST /shutdown`` → exit code 0"; the
        host itself treats that request as best-effort and then terminates the
        child, so this helper does not insist on the 202 ack landing within one
        client time slice.  What it does insist on is the exit code: a child
        that is unreachable *and still alive* is a failure with diagnostics,
        while a child that already exited 0 satisfies the rule (a sidecar that
        exits without any stop request is caught by
        ``test_the_service_never_stops_on_its_own``).
        """
        for _attempt in range(3):
            try:
                status, _ = sidecar.http("POST", "/api/v1/shutdown", port, {})
            except (urllib.error.URLError, OSError):
                if sidecar.proc.poll() is None:
                    time.sleep(0.5)
                    continue
                break
            self.assertEqual(status, 202, "shutdown was not acknowledged")
            break
        self.assertEqual(
            sidecar.wait(),
            0,
            "sidecar did not stop cleanly (stderr=%r)" % (bytes(sidecar.stderr)[-300:],),
        )


class HappyPathTests(SidecarTestCase):
    """Item 3 + 4: one ready line, and it tells the truth about the socket."""

    def test_ready_line_is_exactly_one_and_parseable(self) -> None:
        sidecar = self.start()
        raw = sidecar.ready_line()
        self.assertIsNotNone(raw, "no ready line: %r" % bytes(sidecar.stderr))
        assert raw is not None
        self.assertTrue(raw.endswith(b"\n"))
        self.assertEqual(raw.count(b"\n"), 1)
        record = json.loads(raw.decode("utf-8"))
        self.assertEqual(record["type"], "ready")
        self.assertEqual(record["protocol"], "edu.api.v1")
        self.assertTrue(record["agent_version"])
        self.stop(sidecar, record["port"])

    def test_nonce_is_echoed_and_the_port_really_listens(self) -> None:
        sidecar = self.start()
        ready = sidecar.ready()
        self.assertIsNotNone(ready)
        assert ready is not None
        self.assertEqual(ready["nonce"], NONCE)
        self.assertIsInstance(ready["port"], int)
        self.assertNotEqual(ready["port"], 0)
        status, body = sidecar.http("GET", "/api/v1/health", ready["port"], token=None)
        self.assertEqual(status, 200)
        self.assertEqual(body["data"]["protocol"], "edu.api.v1")
        self.stop(sidecar, ready["port"])

    def test_an_explicit_port_is_reported_verbatim(self) -> None:
        for attempt in range(3):
            port = free_port()
            sidecar = self.start(extra_args=("--port", str(port)))
            raw = sidecar.ready_line()
            if raw is None and sidecar.proc.poll() == EXIT_BIND_ERROR:
                # Another process grabbed the port between our probe and the
                # child's bind; that is the bind-failure rule, not this one.
                continue
            self.assertIsNotNone(raw, "no ready line on attempt %d" % (attempt + 1))
            assert raw is not None
            self.assertEqual(json.loads(raw.decode("utf-8"))["port"], port)
            self.stop(sidecar, port)
            return
        self.fail("could not keep a free port for three attempts in a row")

    def test_two_launches_get_different_system_assigned_ports(self) -> None:
        first = self.start()
        second = self.start()
        first_ready = first.ready()
        second_ready = second.ready()
        self.assertIsNotNone(first_ready)
        self.assertIsNotNone(second_ready)
        assert first_ready is not None and second_ready is not None
        self.assertNotEqual(first_ready["port"], second_ready["port"])
        self.stop(first, first_ready["port"])
        self.stop(second, second_ready["port"])

    def test_stdout_stays_silent_after_ready(self) -> None:
        sidecar = self.start()
        raw = sidecar.ready_line()
        self.assertIsNotNone(raw)
        assert raw is not None
        ready = json.loads(raw.decode("utf-8"))

        # Exercise the service, including a run (which logs) and a shutdown.
        status, body = sidecar.http(
            "POST", "/api/v1/sessions", ready["port"],
            {"project_id": "proj-t2", "revision": "rev-1"},
        )
        self.assertEqual(status, 201)
        session_id = body["data"]["session_id"]
        status, _ = sidecar.http(
            "POST", "/api/v1/sessions/%s/runs" % session_id, ready["port"],
            {"kind": "explain", "level": "L1", "selection": {"text": "always @(*) begin\n  if (sel) y = a;\nend\n"}},
        )
        self.assertEqual(status, 201)
        self.stop(sidecar, ready["port"])
        self.assertEqual(bytes(sidecar.stdout), raw, "stdout carried more than the ready line")
        self.assertTrue(sidecar.stderr, "diagnostics belong on stderr")


class InvalidBootstrapTests(SidecarTestCase):
    """Item 1 + 7: every invalid record exits 2 with an empty stdout."""

    def _assert_rejected(self, line: bytes, label: str) -> None:
        sidecar = SidecarProcess(line=line, data_directory=tempfile.mkdtemp(prefix="edu-agent-t2-"))
        self.addCleanup(sidecar.close)
        code = sidecar.wait(timeout=STOP_TIMEOUT_SECONDS)
        self.assertEqual(code, EXIT_BOOTSTRAP_ERROR, "%s exited with %s" % (label, code))
        self.assertEqual(bytes(sidecar.stdout), b"", "%s wrote to stdout" % label)

    def test_invalid_records_exit_two_with_empty_stdout(self) -> None:
        cases: list[tuple[str, bytes]] = [
            ("not JSON", b"{not json\n"),
            ("JSON array", b"[1, 2, 3]\n"),
            ("empty line", b"   \n"),
            ("wrong type", (json.dumps(bootstrap_record(type="something-else")) + "\n").encode()),
            ("wrong protocol", (json.dumps(bootstrap_record(protocol="edu.api.v2")) + "\n").encode()),
            ("file:// gateway_url", (json.dumps(bootstrap_record(gateway_url="file:///etc/passwd")) + "\n").encode()),
            ("non-loopback gateway_url", (json.dumps(bootstrap_record(gateway_url="http://10.0.0.5:9")) + "\n").encode()),
        ]
        missing = bootstrap_record()
        missing.pop("ui_token")
        cases.append(("missing field", (json.dumps(missing) + "\n").encode()))
        blank = bootstrap_record()
        blank["data_directory"] = "   "
        cases.append(("blank field", (json.dumps(blank) + "\n").encode()))
        cases.append(("oversized line", b"x" * (65 * 1024) + b"\n"))
        for label, line in cases:
            with self.subTest(case=label):
                self._assert_rejected(line, label)

    def test_closed_stdin_exits_two_with_empty_stdout(self) -> None:
        self._assert_rejected(b"", "closed stdin")

    def test_missing_bootstrap_flag_exits_two_with_empty_stdout(self) -> None:
        sidecar = SidecarProcess(line=None, bootstrap_stdin=False)
        self.addCleanup(sidecar.close)
        self.assertEqual(sidecar.wait(timeout=STOP_TIMEOUT_SECONDS), EXIT_BOOTSTRAP_ERROR)
        self.assertEqual(bytes(sidecar.stdout), b"")


class BindFailureTests(SidecarTestCase):
    """Item 2: a port we cannot bind is a startup failure, not a silent retry."""

    def test_occupied_port_exits_three_with_empty_stdout(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as blocker:
            blocker.bind(("127.0.0.1", 0))
            blocker.listen(5)
            port = int(blocker.getsockname()[1])
            sidecar = self.start(extra_args=("--port", str(port)))
            self.assertEqual(sidecar.wait(timeout=STOP_TIMEOUT_SECONDS), EXIT_BIND_ERROR)
        self.assertEqual(bytes(sidecar.stdout), b"")
        self.assertIn(b"bind", bytes(sidecar.stderr))


class StopTests(SidecarTestCase):
    """Item 6: three ways to stop, all of them exit code 0."""

    def test_authenticated_shutdown_exits_zero(self) -> None:
        sidecar = self.start()
        ready = sidecar.ready()
        assert ready is not None
        self.stop(sidecar, ready["port"])

    def test_the_service_never_stops_on_its_own(self) -> None:
        """Nothing but the stop paths may end the process.

        This is what keeps :meth:`stop` honest when it accepts an already
        exited child: a sidecar that would exit by itself is caught here.
        """
        sidecar = self.start()
        ready = sidecar.ready()
        assert ready is not None
        sidecar.http("POST", "/api/v1/shutdown", ready["port"], {}, token="wrong")
        sidecar.http("GET", "/api/v1/health", ready["port"], token=None)
        self.assertIsNone(sidecar.proc.poll(), "the sidecar exited without a stop request")
        time.sleep(0.5)
        self.assertIsNone(sidecar.proc.poll(), "the sidecar exited without a stop request")
        self.stop(sidecar, ready["port"])

    def test_unauthenticated_shutdown_is_refused_and_the_service_keeps_running(self) -> None:
        sidecar = self.start()
        ready = sidecar.ready()
        assert ready is not None
        status, body = sidecar.http("POST", "/api/v1/shutdown", ready["port"], {}, token="wrong")
        self.assertEqual(status, 401)
        self.assertEqual(body["error"]["code"], "UNAUTHENTICATED")
        self.assertTrue(sidecar.alive(), "a rejected shutdown must not stop the sidecar")
        self.stop(sidecar, ready["port"])

    @unittest.skipIf(
        sys.platform == "win32",
        "Windows has no catchable SIGTERM: os.kill maps to TerminateProcess, so the "
        "host stops the sidecar with POST /shutdown instead (see AgentServiceController)",
    )
    def test_sigterm_and_sigint_exit_zero(self) -> None:
        for signum in (signal.SIGTERM, signal.SIGINT):
            with self.subTest(signal=signum):
                sidecar = self.start()
                ready = sidecar.ready()
                self.assertIsNotNone(ready)
                sidecar.proc.send_signal(signum)
                self.assertEqual(sidecar.wait(), 0)
                self.assertEqual(sidecar.stdout.count(b"\n"), 1)

    def test_signal_handlers_translate_to_a_stop_request(self) -> None:
        """The handler wiring itself is asserted on every platform."""
        from sigflow_edu_agent.__main__ import _install_signal_handlers

        saved = {name: signal.getsignal(getattr(signal, name)) for name in ("SIGTERM", "SIGINT")}
        try:
            for signum in (signal.SIGTERM, signal.SIGINT):
                stop_event = threading.Event()
                _install_signal_handlers(stop_event)
                handler = signal.getsignal(signum)
                self.assertTrue(callable(handler), "%s has no handler" % signum)
                assert callable(handler)
                handler(signum, None)
                self.assertTrue(stop_event.is_set())
        finally:
            for name, handler in saved.items():
                signal.signal(getattr(signal, name), handler)


class SelfTestModeTests(SidecarTestCase):
    """Item 8: the six fault-injection modes report the codes the host keys on."""

    def test_exit_before_ready_is_seven_with_empty_stdout(self) -> None:
        sidecar = self.start(extra_args=("--selftest-fail", "exit-before-ready"))
        self.assertEqual(sidecar.wait(timeout=STOP_TIMEOUT_SECONDS), EXIT_SELFTEST_BEFORE_READY)
        self.assertEqual(bytes(sidecar.stdout), b"")

    def test_exit_after_ready_is_nine_with_one_ready_line(self) -> None:
        sidecar = self.start(extra_args=("--selftest-fail", "exit-after-ready"))
        raw = sidecar.ready_line()
        self.assertIsNotNone(raw)
        assert raw is not None
        self.assertEqual(json.loads(raw.decode("utf-8"))["type"], "ready")
        self.assertEqual(sidecar.wait(timeout=STOP_TIMEOUT_SECONDS), EXIT_SELFTEST_AFTER_READY)
        self.assertEqual(bytes(sidecar.stdout), raw)

    def test_wrong_nonce_is_reported_verbatim(self) -> None:
        sidecar = self.start(extra_args=("--selftest-fail", "wrong-nonce"))
        ready = sidecar.ready()
        self.assertIsNotNone(ready)
        assert ready is not None
        self.assertNotEqual(ready["nonce"], NONCE)
        self.assertEqual(ready["protocol"], "edu.api.v1")
        # The rule under test is the perturbed ready record; the stop path is
        # covered by StopTests, so this process is simply reaped.
        sidecar.close()

    def test_wrong_protocol_is_reported_verbatim(self) -> None:
        sidecar = self.start(extra_args=("--selftest-fail", "wrong-protocol"))
        ready = sidecar.ready()
        self.assertIsNotNone(ready)
        assert ready is not None
        self.assertNotEqual(ready["protocol"], "edu.api.v1")
        self.assertEqual(ready["nonce"], NONCE)
        sidecar.close()

    def test_hang_and_no_ready_never_announce_readiness(self) -> None:
        for mode in ("hang", "no-ready"):
            with self.subTest(mode=mode):
                sidecar = self.start(extra_args=("--selftest-fail", mode))
                started = time.monotonic()
                self.assertIsNone(
                    sidecar.ready_line(timeout=2.0), "%s must not emit a ready line" % mode
                )
                self.assertGreaterEqual(time.monotonic() - started, 2.0)
                self.assertTrue(sidecar.alive(), "%s must stay up until the host kills it" % mode)
                sidecar.close()
                self.assertEqual(bytes(sidecar.stdout), b"")


class SecretHygieneTests(SidecarTestCase):
    """Item 5: tokens and the data directory never leave process memory."""

    def test_tokens_never_reach_stdout_stderr_or_the_data_directory(self) -> None:
        sidecar = self.start()
        raw = sidecar.ready_line()
        self.assertIsNotNone(raw)
        assert raw is not None
        ready = json.loads(raw.decode("utf-8"))
        status, _ = sidecar.http("POST", "/api/v1/shutdown", ready["port"], {})
        self.assertEqual(status, 202)
        self.assertEqual(sidecar.wait(), 0)

        observed = bytes(sidecar.stdout) + bytes(sidecar.stderr) + raw
        for secret in (UI_TOKEN, GATEWAY_TOKEN):
            self.assertNotIn(secret.encode("utf-8"), observed)
        for root, _dirs, files in os.walk(sidecar.data_directory):
            for name in files:
                with open(os.path.join(root, name), "rb") as handle:
                    content = handle.read()
                for secret in (UI_TOKEN, GATEWAY_TOKEN):
                    self.assertNotIn(secret.encode("utf-8"), content)

    def test_repr_of_the_bootstrap_record_hides_the_secrets(self) -> None:
        from sigflow_edu_agent.bootstrap import parse_line

        rendered = repr(parse_line((json.dumps(bootstrap_record(data_directory="C:/temp")) + "\n").encode()))
        self.assertNotIn(UI_TOKEN, rendered)
        self.assertNotIn(GATEWAY_TOKEN, rendered)


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
