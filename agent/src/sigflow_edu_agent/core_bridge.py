"""Optional bridge to the packaged teaching-core runtime.

The v1 HTTP surface keeps its stable DTOs and request dispatcher, while this
bridge composes the production core runtime against the same Gateway identity
when the teaching core is installed beside the sidecar.  Absence of the
optional package is reported as a degraded capability and the base rules stay
available.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import importlib
import importlib.metadata
from pathlib import Path
from typing import Any, Optional

from .gateway_client import parse_loopback_base_url


@dataclass
class TeachingCoreRuntime:
    """The one optional core runtime owned by the v1 sidecar process.

    The v1 HTTP contract remains unchanged, but when the production core wheel
    is installed we compose its real runtime here.  That keeps the Gateway
    identity, durable store and verification journal in one process instead of
    merely checking that a package can be imported.  The objects are kept
    private so credentials and local paths never cross the v1 wire contract.
    """

    available: bool
    package_version: Optional[str]
    gateway_url: str
    instance_id: str
    reason: str
    model_readiness: dict[str, Any] = field(default_factory=dict)
    course_readiness: dict[str, Any] = field(default_factory=dict)
    capability_coverage: tuple[dict[str, Any], ...] = ()
    runtime: Any = field(default=None, repr=False)
    store: Any = field(default=None, repr=False)
    verification: Any = field(default=None, repr=False)

    def close(self) -> None:
        """Close core workers and journals without extending sidecar shutdown."""
        if self.runtime is not None:
            try:
                self.runtime.close(timeout=2)
            except Exception:
                pass
        if self.store is not None:
            try:
                self.store.close()
            except Exception:
                pass
        if self.verification is not None:
            try:
                self.verification.close()
            except Exception:
                pass


def connect_teaching_core(gateway_url: str, gateway_token: str,
                          instance_id: str, data_directory: str = "") -> TeachingCoreRuntime:
    """Compose the installed teaching core against the bootstrap Gateway.

    The core's production ``Runtime`` is initialized only after the same
    loopback URL, token and instance id have been validated.  Its durable
    session store and verification journal are scoped below the sidecar data
    directory, with stable workspace filenames and transient instance rebinding.  A missing or broken
    optional wheel is a degraded capability, while the v1 rules remain usable.
    """
    parsed = parse_loopback_base_url(gateway_url)
    if parsed is None or not gateway_token or not instance_id or not data_directory:
        return TeachingCoreRuntime(False, None, gateway_url, instance_id,
                                   "shared Gateway bootstrap is invalid")
    try:
        importlib.import_module("sigflow_teaching_core")
        version = importlib.metadata.version("sigflow-teaching-core")
    except Exception:
        return TeachingCoreRuntime(False, None, gateway_url, instance_id,
                                   "sigflow-teaching-core is not installed")
    store = verification = runtime = None
    try:
        from sigflow_teaching_core.clients.gateway import GatewayConfig
        from sigflow_teaching_core.host.factory import make_factory, make_report_resolver
        from sigflow_teaching_core.host.runtime import Runtime
        from sigflow_teaching_core.host.store import DurableStore
        from sigflow_teaching_core.host.verification import GatewayVerification
        from sigflow_teaching_core.courses import SQLiteCourseProvider
        from sigflow_teaching_core.capability_matrix import public_matrix

        gateway = GatewayConfig(parsed[1], gateway_token, instance_id)
        directory = Path(data_directory) / "u2-host"
        directory.mkdir(parents=True, exist_ok=True)
        # The workspace directory is stable; instance_id is only a transient
        # owner and is rebound by the durable store after a sidecar restart.
        store = DurableStore(directory / "learning.sqlite3", instance_id, rebind=True)
        verification = GatewayVerification(
            gateway,
            directory / "learning-verification.sqlite3",
            make_report_resolver(gateway, (), evidence_origin="tool"),
        )
        course_provider = SQLiteCourseProvider()
        factory = make_factory(
            gateway,
            anchors=(),
            evidence_origin="tool",
            verification=verification,
            course_provider=course_provider,
        )
        runtime = Runtime(store, instance_id, factory, evidence_origin="tool")
        model_readiness = dict(available=factory.model_available,
            provider=factory.model_provider, model=factory.model_name,
            limits=dict(factory.model_limits),
            reason="configured" if factory.model_available else "model_unconfigured_rule_fallback")
        course_readiness = dict(factory.course_readiness)
        coverage = tuple(public_matrix(course_readiness=course_readiness,
                                       prediction_available=factory.prediction_available))
    except Exception:
        if runtime is not None:
            try:
                runtime.close(timeout=2)
            except Exception:
                pass
        if store is not None:
            try:
                store.close()
            except Exception:
                pass
        if verification is not None:
            try:
                verification.close()
            except Exception:
                pass
        return TeachingCoreRuntime(False, version, gateway_url, instance_id,
                                   "sigflow-teaching-core initialization failed")
    return TeachingCoreRuntime(True, version, gateway_url, instance_id,
                               "shared Gateway runtime connected", model_readiness,
                               course_readiness, coverage, runtime, store, verification)
