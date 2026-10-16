"""Shared bounded acceptance primitives, with one exception identity under -m."""
import time


class RunFailure(Exception):
    def __init__(self, code, *, execution=None):
        super().__init__(code)
        self.execution = execution


def require(condition, code):
    if not condition:
        raise RunFailure(code)


def wait_for(callback, predicate, deadline, code):
    while time.monotonic() < deadline:
        value = callback()
        if predicate(value):
            return value
        time.sleep(.05)
    raise RunFailure(code)
