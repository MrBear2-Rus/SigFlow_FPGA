"""历史上下文不得使用查询时点之后生成的派生信息。"""
from datetime import datetime, timedelta
from unittest.mock import patch
import json
import unittest

import test_module as helper
from ua10 import TeachingTask


class TemporalTests(unittest.TestCase):
    setUp = helper.ModuleTests.setUp
    tearDown = helper.ModuleTests.tearDown

    def clocks(self):
        facts = self.host.authority.facts(self.api._binding()[0])
        cutoff = max(datetime.fromisoformat(f['available_at']) for f in facts) + timedelta(seconds=10)
        return cutoff.isoformat(), (cutoff + timedelta(seconds=10)).isoformat()

    def test_future_pattern_and_strategy_cannot_enter_past_bundle(self):
        cutoff, generated = self.clocks()
        with patch('ua10._core.semantics.now', return_value=generated):
            self.api.build_learning_patterns(('latch',), provider=helper.FakePatternProvider())
        self.assertFalse(self.api.get_learning_patterns(as_of=cutoff).patterns)
        with patch('ua10._core.bundles.now', return_value=generated):
            past = self.api.get_teaching_memory_bundle(TeachingTask('锁存器', 'latch', 'current', 'r3', cutoff))
            current = self.api.get_teaching_memory_bundle(TeachingTask('锁存器', 'latch', 'current', 'r3', generated))
        self.assertTrue(past.facts)
        self.assertFalse(past.patterns, '未来生成的规律进入了历史上下文')
        self.assertFalse(past.recommendations, '未来生成的建议进入了历史上下文')
        self.assertTrue(current.patterns)
        self.assertTrue(current.recommendations)

    def test_missing_pattern_time_is_not_silently_inferred(self):
        cutoff, generated = self.clocks()
        with patch('ua10._core.semantics.now', return_value=cutoff):
            p = self.api.build_learning_patterns(('latch',), provider=helper.FakePatternProvider())
        row = self.host.projection.db.execute('SELECT data FROM patterns WHERE id=?', (p.pattern_id,)).fetchone()
        data = json.loads(row[0]); data.pop('created_at')
        with self.host.projection.db:
            self.host.projection.db.execute('UPDATE patterns SET data=? WHERE id=?', (json.dumps(data), p.pattern_id))
        self.assertFalse(self.api.get_learning_patterns(as_of=generated).patterns)

    def test_future_availability_update_and_validity_are_filtered(self):
        cutoff, future = self.clocks()
        with patch('ua10._core.semantics.now', return_value=cutoff):
            p = self.api.build_learning_patterns(('latch',), provider=helper.FakePatternProvider())
        row = self.host.projection.db.execute('SELECT data FROM patterns WHERE id=?', (p.pattern_id,)).fetchone()
        original = json.loads(row[0])
        for field in ('available_at', 'valid_from', 'updated_at'):
            with self.subTest(field=field):
                value = dict(original, **{field: future})
                with self.host.projection.db:
                    self.host.projection.db.execute('UPDATE patterns SET data=? WHERE id=?', (json.dumps(value), p.pattern_id))
                self.assertFalse(self.api.get_learning_patterns(as_of=cutoff).patterns)

    def test_complete_legacy_times_remain_compatible_and_expiry_is_exclusive(self):
        cutoff, future = self.clocks()
        with patch('ua10._core.semantics.now', return_value=cutoff):
            p = self.api.build_learning_patterns(('latch',), provider=helper.FakePatternProvider())
        row = self.host.projection.db.execute('SELECT data FROM patterns WHERE id=?', (p.pattern_id,)).fetchone()
        value = json.loads(row[0]); value.pop('available_at'); value.pop('valid_from')
        with self.host.projection.db:
            self.host.projection.db.execute('UPDATE patterns SET data=? WHERE id=?', (json.dumps(value), p.pattern_id))
        self.assertTrue(self.api.get_learning_patterns(as_of=cutoff).patterns)
        self.assertFalse(self.api.get_learning_patterns(as_of=value['expires_at']).patterns)


if __name__ == '__main__':
    unittest.main(verbosity=2)
