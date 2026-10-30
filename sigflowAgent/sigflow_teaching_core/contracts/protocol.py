"""Fail-closed negotiation of versioned extension namespaces over base v1."""

import re

PROTOCOL = 'edu.api.v1'
U2_EXTENSION = 'u2.host.v1'
_IDENTIFIER = re.compile(r'[a-z][a-z0-9_-]*(\.[a-z][a-z0-9_-]*)+\.v[1-9][0-9]*')


def negotiate_extensions(capabilities, supported):
    if type(capabilities) is not dict or capabilities.get('protocol') != PROTOCOL:
        raise ValueError('invalid_capabilities')
    advertised = capabilities.get('extensions', [])
    if (type(advertised) is not list or
            any(type(item) is not str or not _IDENTIFIER.fullmatch(item) for item in advertised)):
        raise ValueError('invalid_extensions')
    return sorted(set(advertised).intersection(supported))
