"""Single-writer teaching loop; pauses are states, not busy-waiting loops."""

from copy import deepcopy
from uuid import uuid4

from .domain import (Card, CardContext, EvidenceError, Plan, ProviderError, RunRequest,
                     RunState, Status, Trace, VerificationResult)
from .pedagogy import RuleProvider, checked_card, valid_evidence
from .ports import CardProvider, DenyVerification, EvidenceSource, VerificationPort


class TeachingLoop:
    def __init__(self, request: RunRequest, source: EvidenceSource,
                 provider: CardProvider | None = None, *, plan: Plan | None = None,
                 verification: VerificationPort | None = None,
                 max_steps: int = 32, max_model_calls: int = 4):
        if type(max_steps) is not int or not 1 <= max_steps <= 1000:
            raise ValueError('invalid_step_budget')
        if type(max_model_calls) is not int or not 0 <= max_model_calls <= 100:
            raise ValueError('invalid_model_budget')
        if request.intent == 'verify' and plan is None:
            raise ValueError('plan_required')
        if plan is not None and (plan.project_id, plan.revision) != (request.project_id, request.revision):
            raise ValueError('stale_plan')
        self.request, self.source, self.provider = request, source, provider
        self.plan, self.verification = plan, verification or DenyVerification()
        self.max_steps, self.max_model_calls = max_steps, max_model_calls
        self._state = RunState(uuid4().hex)
        self._evidence = None
        self._actions = {}
        self._grant_ref = ''
        self._job_index = 0
        self._results = []

    @property
    def state(self) -> RunState:
        return deepcopy(self._state)

    def run_until_pause(self) -> RunState:
        while self._state.status == Status.RUNNING:
            self.step()
        return self.state

    def step(self) -> RunState:
        state = self._state
        if state.status != Status.RUNNING:
            return self.state
        if state.steps >= self.max_steps:
            state.status, state.reason = Status.BUDGET_EXCEEDED, 'step_budget_exceeded'
            state.version += 1
            return self.state
        stage = state.stage
        state.steps += 1
        state.reason = ''
        if stage == 'E0':
            state.stage = 'E1'
        elif stage == 'E1':
            try:
                self._evidence = self.source.collect(self.request.project_id, self.request.revision)
            except EvidenceError as error:
                state.status, state.reason = Status.WAITING_EVIDENCE, error.code
            except Exception:
                state.status, state.reason = Status.WAITING_EVIDENCE, 'evidence_unavailable'
            else:
                state.stage = 'E2'
        elif stage == 'E2':
            if not valid_evidence(self._evidence, self.request):
                state.status, state.reason = Status.WAITING_EVIDENCE, 'missing_or_stale_evidence'
            else:
                state.stage = 'E3'
        elif stage == 'E3':
            state.stage = 'E4'
        elif stage == 'E4':
            state.cards.append(self._generate_card())
            if self.request.intent == 'explain':
                state.status = Status.COMPLETED
            else:
                state.stage = 'E5'
                state.status = (Status.WAITING_APPROVAL if self.request.intent == 'verify'
                                else Status.WAITING_STUDENT)
        elif stage == 'E6':
            self._execute_step()
        elif stage == 'E7':
            self._feedback()
        else:
            state.status, state.reason = Status.FAILED, 'invalid_stage'
        state.version += 1
        state.trace.append(Trace(stage, state.status, state.reason, state.version))
        return self.state

    def _generate_card(self) -> Card:
        state = self._state
        context = CardContext(self.request.question, state.level, self._evidence)
        if self.provider is not None:
            for attempt in range(2):
                if state.model_calls >= self.max_model_calls:
                    state.reason = 'model_budget_exceeded'
                    break
                state.model_calls += 1
                context = CardContext(context.question, context.level, context.evidence, attempt == 1)
                try:
                    value = self.provider.generate(context)
                except ProviderError as error:
                    state.reason = error.code
                    if error.code == 'invalid_output':
                        continue
                    break
                except Exception:
                    state.reason = 'model_unavailable'
                    break
                card = checked_card(value, context, 'model')
                if card is not None:
                    state.reason = ''
                    return card
                state.reason = 'invalid_output'
        card = checked_card(RuleProvider().generate(context), context, 'rule')
        assert card is not None
        return card

    def act(self, kind: str, action_id: str, expected_version: int, *, grant_ref: str = '') -> RunState:
        if not isinstance(action_id, str) or not action_id.strip() or len(action_id) > 128:
            raise ValueError('invalid_action_id')
        fingerprint = (kind, expected_version, grant_ref)
        if action_id in self._actions:
            if self._actions[action_id] != fingerprint:
                raise ValueError('action_conflict')
            return self.state
        state = self._state
        if type(expected_version) is not int or expected_version != state.version:
            raise ValueError('stale_state')
        if kind not in {'cancel', 'finish', 'hint_next', 'refresh_evidence', 'execute_plan',
                        'reference_request', 'reference_confirm'}:
            raise ValueError('unknown_action')
        if state.status in {Status.COMPLETED, Status.CANCELLED, Status.FAILED, Status.BUDGET_EXCEEDED}:
            raise ValueError('run_terminal')
        if kind in {'reference_request', 'reference_confirm'}:
            raise ValueError('receipt_unavailable')
        if kind == 'cancel':
            state.status, state.reason = Status.CANCELLED, 'cancelled_by_user'
        elif kind == 'finish' and state.status == Status.WAITING_STUDENT:
            state.status = Status.COMPLETED
        elif kind == 'hint_next' and state.status == Status.WAITING_STUDENT:
            if state.level >= 3:
                raise ValueError('reference_required')
            state.level += 1
            # Recollect before generating; do not reuse an expired report.
            state.stage, state.status = 'E1', Status.RUNNING
        elif kind == 'refresh_evidence' and state.status == Status.WAITING_EVIDENCE:
            state.stage, state.status = 'E1', Status.RUNNING
        elif kind == 'execute_plan' and state.status == Status.WAITING_APPROVAL:
            if not self._authorized(grant_ref):
                raise ValueError('approval_denied')
            self._grant_ref = grant_ref
            state.stage, state.status = 'E6', Status.RUNNING
        else:
            raise ValueError('action_not_allowed')
        state.version += 1
        self._actions[action_id] = fingerprint
        return self.state

    def _authorized(self, grant_ref: str) -> bool:
        if not isinstance(grant_ref, str) or not grant_ref or len(grant_ref) > 512 or self.plan is None:
            return False
        try:
            return self.verification.authorized(self.plan, grant_ref) is True
        except Exception:
            return False

    def _execute_step(self):
        state = self._state
        try:
            current = self.source.collect(self.request.project_id, self.request.revision)
            if not valid_evidence(current, self.request):
                state.status, state.reason = Status.FAILED, 'stale_execution_evidence'
                return
            if not self._authorized(self._grant_ref):
                state.status, state.reason = Status.FAILED, 'approval_denied'
                return
            key = f'{state.run_id}:{self.plan.fingerprint}:{self._job_index}'
            result = self.verification.execute(self.plan, self._job_index, self._grant_ref, key)
        except Exception:
            # Unknown submission outcome never causes automatic retry.
            state.status, state.reason = Status.FAILED, 'verification_unavailable'
            return
        if not isinstance(result, VerificationResult) or result.state != 'Succeeded':
            state.status, state.reason = Status.FAILED, 'job_not_succeeded'
            return
        if not valid_evidence(result.evidence, self.request):
            state.status, state.reason = Status.FAILED, 'invalid_result_evidence'
            return
        seen_jobs = {self._evidence.job_id} | {r.evidence.job_id for r in self._results}
        if result.evidence.job_id in seen_jobs:
            state.status, state.reason = Status.FAILED, 'reused_job_result'
            return
        self._results.append(result)
        self._job_index += 1
        if self._job_index == len(self.plan.steps):
            state.stage = 'E7'

    def _feedback(self):
        last = self._results[-1]
        if last.matches_expected is True:
            hint = '本次测试覆盖范围内的观察与预期一致；不能据此认定整个设计正确或已掌握概念。'
        elif last.matches_expected is False:
            hint = '本次测试覆盖范围内的观察与预期不一致，请重新核对假设与证据。'
        else:
            hint = '工具任务已完成，但没有完整的功能判定；不能据此认定功能正确或已掌握概念。'
        limits = ('仅覆盖当前计划与当前版本。',)
        if last.evidence.origin == 'fixture':
            limits += ('离线示例结果，不是真实 EDA 验证。',)
        self._state.cards.append(Card(self._state.level, hint, '你如何解释这次观察？',
                                      (last.evidence.evidence_id,), 'rule', limits))
        self._state.status = Status.COMPLETED
