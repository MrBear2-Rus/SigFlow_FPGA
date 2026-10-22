"""Contract-test launcher; synthetic memory/model only, never a UA-10 service."""
import json
from pathlib import Path
import sys
from threading import Lock
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from sigflow_edu_agent.host.__main__ import main


def launch():
    mode, log_path = sys.argv[1], Path(sys.argv[2])
    lock = Lock()

    def record(kind, data):
        with lock, log_path.open('a', encoding='utf-8') as output:
            output.write(json.dumps(dict(kind=kind, data=data), ensure_ascii=True)+'\n')

    class Receiver:
        attempted = False

        def record_teaching_fact(self, fact):
            record('fact', fact)
            if not self.attempted:
                self.attempted = True
                return dict(accepted=True, event_id='wrong-id')
            return dict(accepted=True, event_id=fact['event_id'])

        def get_teaching_context(self, **scope):
            record('history', scope)
            return dict(learner_id='foreign' if mode == 'foreign' else scope['learner_id'],
                project_id=scope['project_id'], concept_ids=scope['concept_ids'], summary='history-probe')

    class Provider:
        config = SimpleNamespace(api_key='synthetic-model-key')

        def generate(self, context):
            record('model', dict(level=context.level, history=context.learning_history))
            return dict(level=context.level,
                hint='history-probe' if context.learning_history else 'current-evidence-only',
                question='What does the current report establish?', evidence_ids=[context.evidence.evidence_id])

    with patch('sigflow_edu_agent.host.factory.DeepSeekProvider.from_settings', return_value=Provider()):
        return main(['--bootstrap-stdin', '--gateway-evidence', '--no-course'],
                    teaching_memory=object() if mode == 'invalid' else Receiver())


if __name__ == '__main__':
    raise SystemExit(launch())
