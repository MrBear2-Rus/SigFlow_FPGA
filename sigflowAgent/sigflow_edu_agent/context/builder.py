"""Whole-fragment packing, with explicit omissions and no mixed revisions."""

from ..domain import EvidenceError
from .models import ContextBundle, ContextItem, Omission

PRIORITY = {'selection': 0, 'report': 1, 'rtl': 2, 'wave': 3, 'course': 4}


def build_context(items, project_id, revision, *, max_bytes=8192, max_tokens=4096):
    if any(type(n) is not int or not 0 <= n <= 1048576 for n in (max_bytes, max_tokens)):
        raise EvidenceError('invalid_budget')
    if not isinstance(items, (tuple, list)) or len(items) > 64:
        raise EvidenceError('invalid_context')
    seen, sized = set(), []
    for item in items:
        if not isinstance(item, ContextItem):
            raise EvidenceError('invalid_context')
        fields = (item.item_id, item.kind, item.project_id, item.revision, item.text)
        if not all(type(value) is str and value.strip() for value in fields):
            raise EvidenceError('invalid_context')
        if item.kind not in PRIORITY or item.item_id in seen or len(item.item_id) > 256:
            raise EvidenceError('invalid_context')
        if (item.project_id, item.revision) != (project_id, revision):
            raise EvidenceError('mixed_context')
        seen.add(item.item_id)
        try:
            size = len(item.text.encode('utf-8'))
        except UnicodeError:
            raise EvidenceError('invalid_context') from None
        sized.append((item, size))
    kept, omitted, used = [], [], 0
    for item, size in sorted(sized, key=lambda pair: PRIORITY[pair[0].kind]):
        # One estimated token per UTF-8 byte is intentionally conservative.
        # This is not a model-tokenizer measurement or a whole-request budget.
        reason = ('byte_budget' if used + size > max_bytes else
                  'token_budget' if used + size > max_tokens else None)
        if reason:
            omitted.append(Omission(item.item_id, reason))
        else:
            kept.append(item)
            used += size
    return ContextBundle(tuple(kept), tuple(omitted), used, used)
