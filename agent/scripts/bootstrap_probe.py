#!/usr/bin/env python3
"""Sidecar-side acceptance probe for the bootstrap → ready handshake (T2).

Usage (from the repository root or from ``agent/``)::

    python agent/scripts/bootstrap_probe.py
    python agent/scripts/bootstrap_probe.py --python D:\\venvs\\py313\\Scripts\\python.exe

This drives the sidecar exactly the way ``AgentServiceController`` does
(``<python> -m sigflow_edu_agent --bootstrap-stdin``, one JSON line on stdin,
one ready line on stdout) and asserts the **sidecar half** of the handshake
contract (接口文档 §3 + §6.1):

  [1a] stdin is read as exactly one line, extra lines are ignored
  [1b] an oversized bootstrap line is rejected with exit 2 and empty stdout
  [1c] a malformed bootstrap record (type/protocol/field/gateway_url) is rejected
       with exit 2 and empty stdout
  [2]  an occupied port is a bind failure: exit 3 and empty stdout
  [2a] exactly one ready line arrives well inside the host's 15 s budget
  [2b] the ready line carries type/protocol/nonce/port/agent_version
  [3a] nothing is written to stdout after ready
  [3b] ``--selftest-fail exit-before-ready`` exits 7 with empty stdout
  [3c] ``--selftest-fail exit-after-ready`` exits 9 after one ready line
  [3d] ``--selftest-fail hang`` / ``no-ready`` never announce readiness
  [4]  the port is system-assigned and differs between two launches
  [6]  SIGTERM/SIGINT stop the process with exit 0 (POSIX; Windows has no
       catchable SIGTERM, so the host uses ``POST /shutdown`` -- reported SKIP)
  [7a] ``/health`` returns 200 with the frozen protocol
  [7b] ``wrong-nonce`` / ``wrong-protocol`` perturb the ready line
  [5]  ``bad-health`` makes ``/health`` fail (the host must not publish Ready)
  [6]  ``bad-health-protocol`` answers 200 with the wrong protocol
  [8]  neither token appears on stderr or under the data directory
  [9]  diagnostics go to stderr, never to stdout
  [10] a no-model run degrades: state=degraded, model_used=false, source=rule
  [10c] a first-request L4 run is refused with 403
  [11] ``POST /shutdown`` answers 202 and the process exits promptly

The **host half** (15 s ready timeout, health-before-Ready gate, 5 s polling,
two-failures-then-restart, stderr retention, kill after a 300 ms grace) cannot
be observed from here.  Run ``edu_agent_sidecar_smoke`` for those; this script
deliberately does not emulate the host state machine.
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Optional

AGENT_DIR = Path(__file__).resolve().parent.parent
SRC_DIR = AGENT_DIR / "src"

PROTOCOL = "edu.api.v1"
MAX_BOOTSTRAP_BYTES = 64 * 1024
READY_TIMEOUT_SECONDS = 15.0

# A combinational block that assigns ``y`` on only one path: this must hit
# RTL_LATCH_INCOMPLETE_BRANCH so the no-model path produces a ``rule`` card.
LATCH_SNIPPET = "always @(*) begin\n  if (sel) y = a;\nend\n"

_RESULTS: list[tuple[bool, str, str]] = []


def check(ok: bool, name: str, detail: str = "") -> bool:
    """Record one labelled check and echo it immediately."""
    _RESULTS.append((ok, name, detail))
    print(("  ok  : " if ok else "  FAIL: ") + name + ((" -- " + detail) if detail else ""))
    return ok


class Sidecar:
    """One sidecar process, driven the same way the C++ host drives it."""

    def __init__(self, python: str, extra_args: Optional[list[str]] = None) -> None:
        self.python = python
        self.extra_args = list(extra_args or [])
        self.data_dir = Path(tempfile.mkdtemp(prefix="edu-agent-probe-"))
        self.nonce = secrets.token_hex(32)
        self.ui_token = "ui-" + secrets.token_hex(8)
        self.gateway_token = "gw-" + secrets.token_hex(8)
        self.instance_id = "inst-probe-" + secrets.token_hex(4)

        self.ready: Optional[dict[str, Any]] = None
        self.ready_raw = b""
        self.ready_seconds = 0.0
        self.proc: Optional[subprocess.Popen] = None
        self._stdout = bytearray()
        self._stderr = bytearray()
        self._lines: "queue.Queue[bytes]" = queue.Queue()
        self._threads: list[threading.Thread] = []

    # -- bootstrap payload ------------------------------------------------
    def bootstrap_line(
        self,
        data_directory: Optional[str] = None,
        **overrides: Any,
    ) -> bytes:
        """Return the single JSON line the host would write to stdin."""
        record = {
            "type": "sigflow-bootstrap",
            "protocol": PROTOCOL,
            "instance_id": self.instance_id,
            "nonce": self.nonce,
            "gateway_url": "http://127.0.0.1:59999",
            "gateway_token": self.gateway_token,
            "ui_token": self.ui_token,
            "data_directory": data_directory or str(self.data_dir),
        }
        record.update(overrides)
        return (json.dumps(record, separators=(",", ":")) + "\n").encode("utf-8")

    # -- process ----------------------------------------------------------
    def start(self, stdin_payload: bytes) -> None:
        env = dict(os.environ)
        env["PYTHONPATH"] = str(SRC_DIR) + os.pathsep + env.get("PYTHONPATH", "")
        argv = [self.python, "-m", "sigflow_edu_agent", "--bootstrap-stdin", *self.extra_args]
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            cwd=str(self.data_dir),  # the host also runs the sidecar in its data dir
        )
        self._threads = [
            threading.Thread(
                target=self._pump, args=(self.proc.stdout.fileno(), self._stdout, self._lines),
                daemon=True,
            ),
            threading.Thread(
                target=self._pump, args=(self.proc.stderr.fileno(), self._stderr, None),
                daemon=True,
            ),
        ]
        for thread in self._threads:
            thread.start()
        self.proc.stdin.write(stdin_payload)
        self.proc.stdin.flush()

    @staticmethod
    def _pump(fd: int, sink: bytearray, lines: Optional["queue.Queue[bytes]"]) -> None:
        partial = bytearray()
        while True:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
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
                lines.put(bytes(partial[:index]))
                del partial[: index + 1]

    def wait_ready(self, timeout: float = READY_TIMEOUT_SECONDS) -> Optional[dict[str, Any]]:
        """Block for the single ready line and return it, or ``None`` on timeout."""
        started = time.monotonic()
        try:
            self.ready_raw = self._lines.get(timeout=timeout)
        except queue.Empty:
            self.ready_seconds = time.monotonic() - started
            return None
        self.ready_seconds = time.monotonic() - started
        try:
            self.ready = json.loads(self.ready_raw.decode("utf-8"))
        except ValueError:
            self.ready = None
        return self.ready

    def wait(self, timeout: float = 5.0) -> int:
        """Wait for exit, killing the process if it overruns, and join pumps."""
        assert self.proc is not None
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = self.proc.wait()
        for thread in self._threads:
            thread.join(timeout=2.0)
        return code

    def kill(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            self.proc.kill()
        self.wait(timeout=5.0)

    # -- HTTP -------------------------------------------------------------
    def http(
        self,
        method: str,
        path: str,
        body: Optional[Any] = None,
        token: Optional[str] = None,
        timeout: float = 5.0,
    ) -> tuple[int, dict[str, Any]]:
        port = int(self.ready["port"]) if self.ready else 0
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(
            "http://127.0.0.1:%d%s" % (port, path), data=data, method=method
        )
        request.add_header("Authorization", "Bearer " + (token or self.ui_token))
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.status, json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            try:
                return error.code, json.loads(error.read().decode("utf-8"))
            except ValueError:
                return error.code, {}

    def shutdown(self) -> tuple[int, int, float]:
        """POST /api/v1/shutdown, then wait for exit. Returns (http, code, ms)."""
        started = time.monotonic()
        try:
            status, _ = self.http("POST", "/api/v1/shutdown", {})
        except OSError as error:
            status = -1
            print("       (shutdown request failed: %s)" % error)
        code = self.wait(timeout=5.0)
        return status, code, (time.monotonic() - started) * 1000.0

    # -- observations ------------------------------------------------------
    @property
    def extra_stdout_bytes(self) -> int:
        """Bytes written to stdout beyond the single ready line."""
        expected = len(self.ready_raw) + 1 if self.ready_raw else 0
        return max(0, len(self._stdout) - expected)

    @property
    def stdout_bytes(self) -> bytes:
        return bytes(self._stdout)

    @property
    def stderr_text(self) -> str:
        return self._stderr.decode("utf-8", "replace")


def new_session(sidecar: Sidecar) -> str:
    """Create one session and return its id."""
    status, body = sidecar.http(
        "POST", "/api/v1/sessions", {"project_id": "probe", "revision": "rev-1"}
    )
    assert status == 201, body
    return body["data"]["session_id"]


def scenario_normal(python: str) -> None:
    """[1a][2a][2b][3a][4][7a][8][9][10][10c][11] — the happy path."""
    print("\n-- 正常启动 --")
    run = Sidecar(python)
    run.start(run.bootstrap_line())
    ready = run.wait_ready()
    if not check(ready is not None, "[2a] ready 在 %.0fs 内到达" % READY_TIMEOUT_SECONDS,
                 run.stderr_text.strip()[:200]):
        run.kill()
        return

    check(ready.get("type") == "ready", "[2a] ready 行 type=ready", repr(ready.get("type")))
    check(ready.get("protocol") == PROTOCOL, "[2b] ready protocol=%s" % PROTOCOL,
          repr(ready.get("protocol")))
    check(ready.get("nonce") == run.nonce, "[2b] ready nonce 与 bootstrap 一致")
    check(isinstance(ready.get("port"), int) and 0 < ready["port"] < 65536,
          "[2b] ready 回传了合法端口", repr(ready.get("port")))
    check(bool(ready.get("agent_version")), "[2b] ready agent_version 非空")
    print("       ready 耗时 %.1f ms" % (run.ready_seconds * 1000))

    status, body = run.http("GET", "/api/v1/health", token="")
    protocol = body.get("data", {}).get("protocol")
    check(status == 200 and protocol == PROTOCOL, "[7a] /health 200 且 protocol=%s" % PROTOCOL,
          "status=%s protocol=%r" % (status, protocol))

    status, body = run.http("GET", "/api/v1/capabilities")
    data = body.get("data", {})
    execution = data.get("execution", {})
    model = data.get("model", {})
    check(status == 200 and execution.get("can_run_eda") is False,
          "[10] capabilities 报告 can_run_eda=false", repr(execution.get("can_run_eda")))
    print("       capabilities.note: %s" % execution.get("note"))
    check(model.get("available") is False, "[10] capabilities 报告 model.available=false",
          repr(model.get("available")))

    session_id = new_session(run)
    status, body = run.http(
        "POST",
        "/api/v1/sessions/%s/runs" % session_id,
        {"kind": "explain", "level": "L1", "selection": {"text": LATCH_SNIPPET}},
    )
    payload = body.get("data", {})
    cards = payload.get("cards", [])
    first = cards[0] if cards else {}
    check(status == 201 and payload.get("state") == "degraded",
          "[10] 无 Key 运行时 state=degraded", "status=%s state=%r" % (status, payload.get("state")))
    check(payload.get("model_used") is False, "[10] 无 Key 运行时 model_used=false",
          repr(payload.get("model_used")))
    check(first.get("kind") == "rule" and first.get("source") == "rule",
          "[10] 锁存器片段产出规则卡 (kind=rule, source=rule)",
          "kind=%r source=%r" % (first.get("kind"), first.get("source")))

    status, _ = run.http(
        "POST",
        "/api/v1/sessions/%s/runs" % session_id,
        {"kind": "explain", "level": "L4"},
    )
    check(status == 403, "[10c] 首次请求 L4 被拒 (403)", "status=%s" % status)

    http_status, code, elapsed = run.shutdown()
    check(http_status == 202, "[11] /shutdown 返回 202", "status=%s" % http_status)
    check(code == 0, "[11] 进程退出码 0", "code=%s" % code)
    check(elapsed < 3000, "[11] 关闭在 3 s 内完成", "%.1f ms" % elapsed)
    print("       shutdown 耗时 %.1f ms" % elapsed)

    check(run.extra_stdout_bytes == 0, "[3a] ready 之后 stdout 无额外字节",
          "%d bytes" % run.extra_stdout_bytes)
    check(run.stdout_bytes.count(b"\n") == 1, "[2a] stdout 恰好一行",
          "%d newline(s)" % run.stdout_bytes.count(b"\n"))

    stderr = run.stderr_text
    check(run.gateway_token not in stderr and run.ui_token not in stderr,
          "[8] stderr 不含 bootstrap 令牌")
    leaked = [
        str(path)
        for path in run.data_dir.rglob("*")
        if path.is_file() and (
            run.gateway_token in _safe_read(path) or run.ui_token in _safe_read(path)
        )
    ]
    check(not leaked, "[8] 数据目录不含 bootstrap 令牌",
          "%d file(s) under %s" % (len(leaked), run.data_dir))
    check(bool(stderr.strip()) and b"sigflow_edu_agent" not in run.stdout_bytes,
          "[9] 诊断只走 stderr，stdout 上没有日志")

    # [4] a second launch must pick a different system-assigned port
    second = Sidecar(python)
    second.start(second.bootstrap_line())
    other = second.wait_ready()
    if other is not None:
        second.shutdown()
    check(other is not None and other.get("port") != ready.get("port"),
          "[4] 两次启动端口不同（系统随机分配）",
          "first=%s second=%s" % (ready.get("port"), (other or {}).get("port")))

    # [1a] a trailing second line must be ignored: the sidecar reads one line only
    third = Sidecar(python)
    third.start(third.bootstrap_line() + b"this second line must be ignored\n")
    extra_ready = third.wait_ready()
    if extra_ready is not None:
        http_status, code, _ = third.shutdown()
    else:
        third.kill()
        http_status, code = -1, -1
    check(extra_ready is not None and http_status == 202 and code == 0,
          "[1a] 多写一行 stdin：只读第一行，服务仍正常",
          "ready=%s shutdown=%s exit=%s" % (extra_ready is not None, http_status, code))


def _safe_read(path: Path) -> str:
    try:
        return path.read_bytes().decode("utf-8", "replace")
    except OSError:
        return ""


def scenario_faults(python: str) -> None:
    """[1b][3b][5][6][7b] — the sidecar half of the fault-injection modes."""
    print("\n-- 故障注入（sidecar 侧） --")

    oversized = Sidecar(python)
    oversized.start(oversized.bootstrap_line(data_directory="x" * (MAX_BOOTSTRAP_BYTES + 1024)))
    code = oversized.wait(timeout=10.0)
    check(code == 2 and not oversized.stdout_bytes,
          "[1b] 超 64 KiB 的 bootstrap 被拒 (exit 2, stdout 空)",
          "exit=%s stdout=%d bytes" % (code, len(oversized.stdout_bytes)))

    early = Sidecar(python, ["--selftest-fail", "exit-before-ready"])
    early.start(early.bootstrap_line())
    code = early.wait(timeout=10.0)
    check(code == 7 and not early.stdout_bytes,
          "[3b] exit-before-ready：exit 7 且 stdout 空",
          "exit=%s stdout=%d bytes" % (code, len(early.stdout_bytes)))

    nonce_fault = Sidecar(python, ["--selftest-fail", "wrong-nonce"])
    nonce_fault.start(nonce_fault.bootstrap_line())
    fault_ready = nonce_fault.wait_ready()
    nonce_fault.kill()
    check(fault_ready is not None and fault_ready.get("nonce") != nonce_fault.nonce,
          "[7b] wrong-nonce：ready 行 nonce 与 bootstrap 不一致",
          repr((fault_ready or {}).get("nonce")))

    protocol_fault = Sidecar(python, ["--selftest-fail", "wrong-protocol"])
    protocol_fault.start(protocol_fault.bootstrap_line())
    fault_ready = protocol_fault.wait_ready()
    protocol_fault.kill()
    check(fault_ready is not None and fault_ready.get("protocol") != PROTOCOL,
          "[7b] wrong-protocol：ready 行协议不是 %s" % PROTOCOL,
          repr((fault_ready or {}).get("protocol")))

    bad_health = Sidecar(python, ["--selftest-fail", "bad-health"])
    bad_health.start(bad_health.bootstrap_line())
    if bad_health.wait_ready() is not None:
        status, _ = bad_health.http("GET", "/api/v1/health", token="")
        bad_health.shutdown()
        check(status == 503, "[5] bad-health：/health 返回 503，宿主不得发布 Ready",
              "status=%s" % status)
    else:
        bad_health.kill()
        check(False, "[5] bad-health：ready 未到达", "进程没有写出 ready 行")

    bad_protocol = Sidecar(python, ["--selftest-fail", "bad-health-protocol"])
    bad_protocol.start(bad_protocol.bootstrap_line())
    if bad_protocol.wait_ready() is not None:
        status, body = bad_protocol.http("GET", "/api/v1/health", token="")
        reported = body.get("data", {}).get("protocol")
        bad_protocol.shutdown()
        check(status == 200 and reported != PROTOCOL,
              "[6] bad-health-protocol：200 但协议不是 %s，宿主必须拒绝" % PROTOCOL,
              "status=%s protocol=%r" % (status, reported))
    else:
        bad_protocol.kill()
        check(False, "[6] bad-health-protocol：ready 未到达", "进程没有写出 ready 行")


def scenario_exit_codes(python: str) -> None:
    """[1c][2][3c][3d][6] — the exit codes and stop paths the host keys on."""
    print("\n-- 退出码与停止路径 --")

    invalid = [
        ("type 不符", {"type": "something-else"}),
        ("protocol 不符", {"protocol": "edu.api.v2"}),
        ("gateway_url 非 loopback", {"gateway_url": "http://10.0.0.5:9"}),
        ("ui_token 为空", {"ui_token": "   "}),
    ]
    for label, overrides in invalid:
        bad = Sidecar(python)
        bad.start(bad.bootstrap_line(**overrides))
        code = bad.wait(timeout=10.0)
        check(code == 2 and not bad.stdout_bytes,
              "[1c] 非法 bootstrap（%s）→ exit 2 且 stdout 空" % label,
              "exit=%s stdout=%d bytes" % (code, len(bad.stdout_bytes)))

    blocker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        blocker.bind(("127.0.0.1", 0))
        blocker.listen(5)
        occupied = int(blocker.getsockname()[1])
        bind_fail = Sidecar(python, ["--port", str(occupied)])
        bind_fail.start(bind_fail.bootstrap_line())
        code = bind_fail.wait(timeout=10.0)
        check(code == 3 and not bind_fail.stdout_bytes,
              "[2] 端口被占用 → exit 3 且 stdout 空",
              "exit=%s stdout=%d bytes" % (code, len(bind_fail.stdout_bytes)))
    finally:
        blocker.close()

    after = Sidecar(python, ["--selftest-fail", "exit-after-ready"])
    after.start(after.bootstrap_line())
    after_ready = after.wait_ready()
    code = after.wait(timeout=10.0)
    lines = after.stdout_bytes.count(b"\n")
    check(after_ready is not None and code == 9 and lines == 1,
          "[3c] exit-after-ready：exit 9 且 stdout 恰好一行 ready",
          "exit=%s lines=%d" % (code, lines))

    for mode in ("hang", "no-ready"):
        stuck = Sidecar(python, ["--selftest-fail", mode])
        stuck.start(stuck.bootstrap_line())
        stuck_ready = stuck.wait_ready(timeout=2.0)
        alive = stuck.proc is not None and stuck.proc.poll() is None
        stuck.kill()
        check(stuck_ready is None and alive and not stuck.stdout_bytes,
              "[3d] %s：2s 内不写 ready、进程仍存活、stdout 空" % mode,
              "ready=%s alive=%s stdout=%d bytes"
              % (stuck_ready is not None, alive, len(stuck.stdout_bytes)))

    if sys.platform == "win32":
        check(True,
              "[6] SIGTERM/SIGINT → exit 0（SKIP：Windows 无可捕获的 SIGTERM，"
              "宿主以 POST /shutdown 停止 sidecar）")
    else:
        for signame in ("SIGTERM", "SIGINT"):
            interrupted = Sidecar(python)
            interrupted.start(interrupted.bootstrap_line())
            if interrupted.wait_ready() is None:
                interrupted.kill()
                check(False, "[6] %s：ready 未到达" % signame, "")
                continue
            interrupted.proc.send_signal(getattr(signal, signame))
            code = interrupted.wait(timeout=10.0)
            check(code == 0, "[6] %s → exit 0" % signame, "exit=%s" % code)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--python", default=sys.executable,
                        help="interpreter used to launch the sidecar (default: this interpreter)")
    args = parser.parse_args()

    print("agent root : %s" % AGENT_DIR)
    print("interpreter: %s" % args.python)
    if not (SRC_DIR / "sigflow_edu_agent" / "__init__.py").is_file():
        print("FAIL: %s does not look like the agent source tree" % SRC_DIR)
        return 1

    try:
        scenario_normal(args.python)
        scenario_faults(args.python)
        scenario_exit_codes(args.python)
    except FileNotFoundError as error:
        print("FAIL: cannot launch the sidecar: %s" % error)
        return 1

    failed = [name for ok, name, _ in _RESULTS if not ok]
    print("\nSUMMARY: checks=%d failed=%d" % (len(_RESULTS), len(failed)))
    if failed:
        for name in failed:
            print("  - %s" % name)
        return 1
    print("所有 sidecar 侧握手检查通过；宿主侧请跑 edu_agent_sidecar_smoke。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
