"""SigFlow Edu Agent — self-developed teaching sidecar for SigFlow.

The package implements the Agent side of the frozen ``edu.api.v1`` contract:

* :mod:`sigflow_edu_agent.bootstrap` parses the one-line stdin handshake;
* :mod:`sigflow_edu_agent.rules` is the deterministic, model-free rule engine;
* :mod:`sigflow_edu_agent.cards` builds TeachingCard / PlanCard payloads;
* :mod:`sigflow_edu_agent.http_server` serves the loopback ``/api/v1`` surface;
* :mod:`sigflow_edu_agent.gateway_client` is the only outbound channel.

This service holds **no** EDA execution permission: it never runs a tool,
never reads or writes a project file, and never accepts a path, script or
command from a client.  Execution is only ever triggered by a grant issued by
SigFlow.
"""

from __future__ import annotations

from .version import AGENT_VERSION, PROTOCOL

__all__ = ["AGENT_VERSION", "PROTOCOL", "__version__"]

__version__ = AGENT_VERSION
