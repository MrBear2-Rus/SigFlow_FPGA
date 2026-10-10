"""``python -m sigflow_edu_agent`` — the SigFlow Edu Agent sidecar entry point.

Startup follows ``docs/edu-agent/sigflow/sidecar-bootstrap.md`` exactly:

1. read one UTF-8 JSON bootstrap line from stdin (exit 2 on any problem, with
   stdout still empty);
2. bind ``127.0.0.1`` *before* announcing readiness (exit 3 if binding fails);
3. write exactly one JSON ready line to stdout and flush;
4. never write to stdout again;
5. serve until SIGTERM/SIGINT or an authenticated ``POST /api/v1/shutdown``.

The ``--selftest-fail`` and ``--selftest-ready-delay-ms`` flags exist only to
exercise the host controller's failure paths.  They are test-only, are not part
of the frozen contract, and must never be used in production.
"""

from __future__ import annotations

import argparse
import logging
import signal
import sys
import threading
import time
from typing import Any, Optional, Sequence, TextIO

from .bootstrap import Bootstrap, BootstrapError, read_from_stream
from .gateway_client import GatewayClient
from .http_server import EduAgentState, ServerConfig, create_server
from .model_client import ModelClient
from .util import RedactionFilter, json_dumps
from .version import AGENT_VERSION, PROTOCOL, READY_TYPE

LOGGER = logging.getLogger("sigflow_edu_agent")

EXIT_BOOTSTRAP_ERROR: int = 2
EXIT_BIND_ERROR: int = 3

SELFTEST_MODES: tuple[str, ...] = (
    "none",
    "exit-before-ready",
    "exit-after-ready",
    "wrong-nonce",
    "wrong-protocol",
    "hang",
    "no-ready",
    # AD-02：合法 ready + 坏 health，用来验证宿主不会在 health 通过前宣布就绪。
    "bad-health",
    "bad-health-protocol",
)
"""Test-only fault-injection modes for the host controller's restart logic."""

_HEALTH_ROUTE: str = "/api/v1/health"


def build_parser() -> argparse.ArgumentParser:
    """Build the command line parser."""
    parser = argparse.ArgumentParser(
        prog="sigflow-edu-agent",
        description=(
            "SigFlow Edu Agent sidecar: a loopback-only teaching service that "
            "holds no EDA execution permission and performs no file or command execution."
        ),
    )
    parser.add_argument(
        "--bootstrap-stdin",
        action="store_true",
        help="read the single JSON bootstrap line from stdin (required for normal operation)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=0,
        help="loopback port to bind; 0 (default) lets the system assign one",
    )
    parser.add_argument(
        "--agent-version",
        default=None,
        help="override the reported agent version (used by the version-mismatch test)",
    )
    parser.add_argument(
        "--selftest-fail",
        choices=SELFTEST_MODES,
        default="none",
        help="TEST-ONLY fault injection; not part of the frozen contract",
    )
    parser.add_argument(
        "--selftest-ready-delay-ms",
        type=int,
        default=0,
        help="TEST-ONLY delay in milliseconds before emitting the ready line",
    )
    return parser


def configure_logging(stream: Optional[TextIO] = None) -> None:
    """Send diagnostics to stderr, and never to stdout."""
    handler = logging.StreamHandler(stream or sys.stderr)
    handler.setFormatter(
        logging.Formatter("%(asctime)s %(levelname)s %(name)s %(message)s")
    )
    root = logging.getLogger()
    root.handlers[:] = [handler]
    root.setLevel(logging.INFO)


def install_redaction(bootstrap: Bootstrap) -> None:
    """Redact every bootstrap secret from every subsequent log record."""
    secret_filter = RedactionFilter(bootstrap.secrets)
    for handler in logging.getLogger().handlers:
        handler.addFilter(secret_filter)


def emit_ready(
    stream: TextIO,
    nonce: str,
    port: int,
    version: str,
    protocol: str = PROTOCOL,
) -> None:
    """Write exactly one ready line to stdout and flush it."""
    record = {
        "type": READY_TYPE,
        "protocol": protocol,
        "nonce": nonce,
        "port": int(port),
        "agent_version": version,
    }
    stream.write(json_dumps(record) + "\n")
    stream.flush()


def _selftest_pre_ready(mode: str) -> Optional[int]:
    """Apply test-only fault injection that happens before readiness."""
    if mode == "exit-before-ready":
        sys.stderr.write("selftest: exiting before readiness\n")
        return 7
    if mode == "hang":
        # Block forever without binding: models a sidecar that never settles.
        while True:
            time.sleep(1.0)
    if mode == "no-ready":
        # Serve nothing and never write ready, then wait to be killed.
        while True:
            time.sleep(1.0)
    return None


def _selftest_ready_record(
    mode: str,
    nonce: str,
    port: int,
    version: str,
) -> dict[str, Any]:
    """Build the ready record, perturbed when a fault-injection mode asks for it."""
    record: dict[str, Any] = {
        "type": READY_TYPE,
        "protocol": PROTOCOL,
        "nonce": nonce,
        "port": int(port),
        "agent_version": version,
    }
    if mode == "wrong-nonce":
        record["nonce"] = "selftest-mismatched-nonce"
    elif mode == "wrong-protocol":
        record["protocol"] = "edu.api.v0"
    return record


def serve_until_stopped(
    httpd: Any,
    stop_event: threading.Event,
    poll_seconds: float = 0.2,
) -> None:
    """Serve requests until the stop event is set, then shut down gracefully."""
    httpd.timeout = poll_seconds
    while not stop_event.is_set():
        httpd.handle_request()
    httpd.server_close()


def main(argv: Optional[Sequence[str]] = None) -> int:
    """Process entry point; returns the process exit code."""
    args = build_parser().parse_args(argv)
    configure_logging()

    if args.selftest_ready_delay_ms < 0:
        sys.stderr.write("--selftest-ready-delay-ms must be >= 0\n")
        return EXIT_BOOTSTRAP_ERROR
    if args.port < 0 or args.port > 65535:
        sys.stderr.write("--port must be between 0 and 65535\n")
        return EXIT_BOOTSTRAP_ERROR
    if not args.bootstrap_stdin:
        sys.stderr.write(
            "--bootstrap-stdin is required: the host supplies the bootstrap record on stdin\n"
        )
        return EXIT_BOOTSTRAP_ERROR

    try:
        bootstrap = read_from_stream(sys.stdin.buffer)
    except BootstrapError as exc:
        sys.stderr.write("bootstrap rejected: %s\n" % exc.reason)
        return EXIT_BOOTSTRAP_ERROR

    install_redaction(bootstrap)
    version = args.agent_version or AGENT_VERSION

    early_exit = _selftest_pre_ready(args.selftest_fail)
    if early_exit is not None:
        return early_exit

    stop_event = threading.Event()
    config = ServerConfig(
        instance_id=bootstrap.instance_id,
        ui_token=bootstrap.ui_token,
        agent_version=version,
        host="127.0.0.1",
        port=args.port,
        gateway=GatewayClient(
            bootstrap.gateway_url,
            bootstrap.gateway_token,
            secrets=bootstrap.secrets,
        ),
        model=ModelClient(secrets=bootstrap.secrets),
        secrets=bootstrap.secrets,
        stop_event=stop_event,
        # TEST-ONLY：仅当显式选择坏 health 模式时才生效。
        health_fault=(
            args.selftest_fail
            if args.selftest_fail in ("bad-health", "bad-health-protocol")
            else None
        ),
    )
    state = EduAgentState(config)
    try:
        httpd = create_server(state)
    except OSError as exc:
        reason = exc.strerror or str(exc)
        sys.stderr.write("unable to bind loopback port: %s\n" % reason)
        return EXIT_BIND_ERROR

    actual_port = int(httpd.server_address[1])
    _install_signal_handlers(stop_event)
    LOGGER.info(
        "sigflow edu agent %s bound to %s:%d", version, config.host, actual_port
    )

    if args.selftest_ready_delay_ms:
        time.sleep(args.selftest_ready_delay_ms / 1000.0)

    if args.selftest_fail == "none":
        emit_ready(sys.stdout, bootstrap.nonce, actual_port, version)
    else:
        record = _selftest_ready_record(
            args.selftest_fail, bootstrap.nonce, actual_port, version
        )
        sys.stdout.write(json_dumps(record) + "\n")
        sys.stdout.flush()
        if args.selftest_fail == "exit-after-ready":
            return 9

    try:
        serve_until_stopped(httpd, stop_event)
    except KeyboardInterrupt:  # pragma: no cover - interactive use only
        stop_event.set()
        httpd.server_close()
    LOGGER.info("sigflow edu agent stopped")
    return 0


def _install_signal_handlers(stop_event: threading.Event) -> None:
    """Translate SIGTERM/SIGINT into a graceful stop request."""

    def _handle(signum: int, _frame: Any) -> None:
        LOGGER.info("received signal %d, shutting down", signum)
        stop_event.set()

    for name in ("SIGTERM", "SIGINT"):
        handler = getattr(signal, name, None)
        if handler is None:
            continue
        try:
            signal.signal(handler, _handle)
        except (ValueError, OSError):  # pragma: no cover - non-main thread
            LOGGER.debug("cannot install %s handler in this context", name)


if __name__ == "__main__":  # pragma: no cover - module entry point
    sys.exit(main())
