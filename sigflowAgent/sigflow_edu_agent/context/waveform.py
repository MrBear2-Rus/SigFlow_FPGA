"""Check bounded Gateway waveform data; never infer reset contracts or run EDA."""

from dataclasses import dataclass
import re

from .models import EvidenceAnchor
from .report import valid_anchor


class WaveformError(ValueError):
    def __init__(self, code='invalid_waveform'):
        self.code = code if code in {'invalid_waveform', 'incomplete_waveform', 'stale_waveform',
                                     'unsupported_value', 'invalid_prediction'} else 'invalid_waveform'
        super().__init__(self.code)


def _tick(value):
    if (type(value) is not str or len(value) > 20
            or re.fullmatch(r'[0-9]+', value) is None or int(value) > 2**64 - 1):
        raise WaveformError()
    return int(value)


def _text(value):
    if type(value) is not str or not value.strip() or len(value) > 4096:
        return False
    try:
        value.encode('utf-8')
    except UnicodeError:
        return False
    return True


def _signal_id(value):
    return type(value) is int and 0 <= value <= 2**31 - 1


def _bits(value, width):
    if type(value) is not str or not 1 <= len(value) <= 8192:
        raise WaveformError('unsupported_value')
    value = value.lower()
    sized = re.fullmatch(r"([0-9]{1,4})'([bh])([0-9a-fxz]+)", value)
    if sized:
        size, base, digits = sized.groups()
        if int(size) != width:
            raise WaveformError('unsupported_value')
        if base == 'h':
            value = ''.join(d*4 if d in 'xz' else format(int(d, 16), '04b') for d in digits)
            # Padding bits in a final hex nibble may be discarded only if zero.
            if len(value) > width:
                excess = value[:-width]
                if set(excess) != {'0'}:
                    raise WaveformError('unsupported_value')
                value = value[-width:]
        else:
            value = digits
    if re.fullmatch(r'[01xz]+', value) is None or len(value) > width:
        raise WaveformError('unsupported_value')
    return value.rjust(width, value[0] if value[0] in 'xz' else '0')


@dataclass(frozen=True)
class WaveSignal:
    signal_id: int
    full_name: str
    width: int
    initial_value: str | None
    transitions: tuple[tuple[int, str], ...]


@dataclass(frozen=True)
class WaveWindow:
    anchor: EvidenceAnchor
    start_tick: int
    end_tick: int
    timescale: str
    signals: tuple[WaveSignal, ...]
    origin: str

    def value_at(self, signal_id, tick):
        point = _tick(tick)
        if not _signal_id(signal_id) or not self.start_tick <= point <= self.end_tick:
            raise WaveformError('invalid_prediction')
        signal = next((s for s in self.signals if s.signal_id == signal_id), None)
        if signal is None:
            raise WaveformError('invalid_prediction')
        value = signal.initial_value
        for time, observed in signal.transitions:
            if time > point:
                break
            value = observed
        return value


@dataclass(frozen=True)
class SampleComparison:
    status: str
    anchor: EvidenceAnchor
    signal_id: int
    full_name: str
    tick: int
    timescale: str
    expected: str
    observed: str | None
    origin: str


def check_waveform(*, catalog, page, anchor, project_id, revision, job_id, snapshot_id,
                   signal_ids, start_tick, end_tick, origin='tool'):
    """Return an immutable single-page observation with host-supplied provenance.

    The host must verify artifact hash ownership before supplying the anchor.
    This checks binding consistency, not the artifact file contents.
    """
    if origin not in ('fixture', 'tool'):
        raise WaveformError()
    if origin == 'tool':
        # Current C++ QueryRange can label the final truncated signal exact
        # (including byte-cap truncation below 10000 events). Fail closed until
        # S2 repairs completeness and supplies a verified service baseline.
        raise WaveformError('incomplete_waveform')
    if (not valid_anchor(anchor, project_id, revision, job_id) or anchor.snapshot_id != snapshot_id
            or re.fullmatch(r'(?:sha256:)?[a-fA-F0-9]{64}', anchor.artifact_hash) is None):
        raise WaveformError('stale_waveform')
    start, end = _tick(start_tick), _tick(end_tick)
    if (end < start or type(signal_ids) is not tuple or not 1 <= len(signal_ids) <= 16
            or not all(_signal_id(s) for s in signal_ids) or len(set(signal_ids)) != len(signal_ids)):
        raise WaveformError()
    if type(catalog) is not dict or type(page) is not dict:
        raise WaveformError()
    if page.get('artifact_id') != anchor.artifact_id or catalog.get('artifact_id') != anchor.artifact_id:
        raise WaveformError('stale_waveform')
    allowed = {'artifact_id', 'start_tick', 'end_tick', 'timescale', 'signals', 'completeness',
               'has_more', 'next_cursor', 'transition_count', 'omitted_signals'}
    if set(page) - allowed or _tick(page.get('start_tick')) != start or _tick(page.get('end_tick')) != end:
        raise WaveformError()
    if (page.get('completeness') not in ('exact', 'complete') or page.get('has_more') is not False
            or page.get('next_cursor') is not None or type(page.get('omitted_signals')) is not int
            or page['omitted_signals'] != 0):
        raise WaveformError('incomplete_waveform')
    timescale = page.get('timescale')
    if (type(timescale) is not str or re.fullmatch(r'(?:1|10|100)(?:fs|ps|ns|us|ms|s)', timescale) is None
            or catalog.get('timescale') != timescale):
        raise WaveformError()
    metadata = catalog.get('signals')
    if (type(metadata) is not list or len(metadata) > 4096 or type(catalog.get('total')) is not int
            or catalog['total'] < len(metadata)):
        raise WaveformError()
    by_id = {}
    for item in metadata:
        if (type(item) is not dict or not _signal_id(item.get('signal_id'))
                or item['signal_id'] in by_id):
            raise WaveformError()
        by_id[item['signal_id']] = item
    rows = page.get('signals')
    if type(rows) is not list or len(rows) != len(signal_ids):
        raise WaveformError('incomplete_waveform')
    signals, seen, count, expanded_bytes = [], set(), 0, 0
    for row in rows:
        if (type(row) is not dict or not _signal_id(row.get('signal_id'))
                or row['signal_id'] not in signal_ids or row['signal_id'] in seen
                or set(row) - {'signal_id', 'full_name', 'width', 'initial_value', 'initial_known', 'transitions'}):
            raise WaveformError()
        sid = row['signal_id']
        seen.add(sid)
        meta = by_id.get(sid, {})
        width, name = meta.get('width'), meta.get('full_name')
        if (type(width) is not int or not 1 <= width <= 4096 or not _text(name)
                or ('width' in row and (type(row['width']) is not int or row['width'] != width))
                or ('full_name' in row and row['full_name'] != name) or 'initial_value' not in row):
            raise WaveformError()
        raw_initial = row['initial_value']
        if ('initial_known' in row and (type(row['initial_known']) is not bool
                or row['initial_known'] != (raw_initial is not None))) or (start == 0 and raw_initial is not None):
            raise WaveformError()
        initial = None if raw_initial is None else _bits(raw_initial, width)
        events = row.get('transitions')
        if type(events) is not list or count + len(events) > 10000:
            raise WaveformError('incomplete_waveform')
        count += len(events)
        expanded_bytes += width * (len(events) + (initial is not None)) + len(name.encode('utf-8'))
        if expanded_bytes > 2 * 1024 * 1024:
            raise WaveformError('incomplete_waveform')
        transitions, previous = [], start
        for event in events:
            if type(event) is not dict or set(event) != {'tick', 'value'}:
                raise WaveformError()
            tick = _tick(event['tick'])
            if not previous <= tick <= end:
                raise WaveformError()
            transitions.append((tick, _bits(event['value'], width)))
            previous = tick
        signals.append(WaveSignal(sid, name, width, initial, tuple(transitions)))
    if type(page.get('transition_count')) is not int or page['transition_count'] != count:
        raise WaveformError('incomplete_waveform')
    return WaveWindow(anchor, start, end, timescale, tuple(signals), origin)


def compare_sample(window, signal_id, tick, expected):
    """Compare an explicit host/student prediction within this observation only."""
    if not isinstance(window, WaveWindow):
        raise WaveformError('invalid_prediction')
    observed = window.value_at(signal_id, tick)
    signal = next(s for s in window.signals if s.signal_id == signal_id)
    prediction = _bits(expected, signal.width)
    if any(c in 'xz' for c in prediction):
        raise WaveformError('invalid_prediction')
    status = ('inconclusive' if observed is None or any(c in 'xz' for c in observed)
              else 'matches' if observed == prediction else 'differs')
    return SampleComparison(status, window.anchor, signal_id, signal.full_name,
                            _tick(tick), window.timescale, prediction, observed, window.origin)
