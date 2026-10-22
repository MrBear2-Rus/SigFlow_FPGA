"""Limits for optional learning history; history confers no evidence authority."""
MAX_HISTORY_BYTES = 1024


def bounded_history(value):
    if (type(value) is not str or len(value)>MAX_HISTORY_BYTES
            or any(ord(c)<32 and c not in '\n\t' for c in value)):
        return ''
    try:
        return value if len(value.encode('utf-8'))<=MAX_HISTORY_BYTES else ''
    except UnicodeError:
        return ''
