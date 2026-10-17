"""Single-writer teaching loop; pauses are states, not busy-waiting loops."""

from copy import deepcopy
from dataclasses import replace
from uuid import uuid4

from .domain import (Card, CardContext, Evidence, EvidenceError, Plan, ProviderError, RunRequest,
                     RunState, Status, Trace, VerificationResult)
from .pedagogy import RuleProvider, checked_card, valid_evidence
from .ports import CardProvider, CourseProvider, DenyVerification, EvidenceSource, VerificationPort
from .checkers import DetailedPlan, PlanChecker, OutcomeChecker
from .patterns import assess


class TeachingLoop:
    def __init__(self, request: RunRequest, source: EvidenceSource,
                 provider: CardProvider | None = None, *, plan: Plan | DetailedPlan | None = None,
                 verification: VerificationPort | None = None,
                 course_provider: CourseProvider | None = None,
                 max_steps: int = 32, max_model_calls: int = 4, baseline_evidence=None):
        if type(max_steps) is not int or not 1 <= max_steps <= 1000:
            raise ValueError('invalid_step_budget')
        if type(max_model_calls) is not int or not 0 <= max_model_calls <= 100:
            raise ValueError('invalid_model_budget')
        if request.intent == 'verify' and plan is None:
            raise ValueError('plan_required')
        if plan is not None and (plan.project_id, plan.revision) != (request.project_id, request.revision):
            raise ValueError('stale_plan')
        if isinstance(plan, DetailedPlan):
            plan = PlanChecker().check(plan)
        if baseline_evidence is not None:
            if not isinstance(baseline_evidence, Evidence):
                raise ValueError('invalid_baseline_evidence')
            baseline_request = RunRequest(request.project_id, baseline_evidence.revision, request.issue_id, request.question)
            if not valid_evidence(baseline_evidence, baseline_request):
                raise ValueError('invalid_baseline_evidence')
        self.request, self.source, self.provider = request, source, provider
        self.plan, self.verification = plan, verification or DenyVerification()
        self.course_provider = course_provider
        self._course_refs = ()
        self._course_evidence = None
        self.max_steps, self.max_model_calls = max_steps, max_model_calls
        self._state = RunState(uuid4().hex)
        self._evidence = None
        self._actions = {}
        self._grant_ref = ''
        self._job_index = 0
        self._results = []
        self._baseline_evidence = baseline_evidence
        self._needs_replan = False
        self._observation_only = False
        self._last_verification = None
        self._last_verification_index = None

    @property
    def state(self) -> RunState:
        return deepcopy(self._state)

    @property
    def verification_progress(self):
        if self.plan is None:
            return None
        result = self._last_verification
        known_states = {'Created', 'Validating', 'Queued', 'Running', 'ValidatingArtifact', 'Cancelling',
                        'Succeeded', 'Failed', 'Cancelled', 'TimedOut', 'Rejected', 'Unknown'}
        evidence = result.evidence if isinstance(result, VerificationResult) else None
        job_id = result.job_id if isinstance(result, VerificationResult) else None
        if job_id is None and isinstance(evidence, Evidence):
            job_id = evidence.job_id
        import re
        if type(job_id) is not str or re.fullmatch(r'[A-Za-z0-9_-]{1,128}', job_id) is None:
            job_id = None
        ready = valid_evidence(evidence, self.request)
        if ready and isinstance(self.plan, DetailedPlan):
            ready = evidence.anchor is not None and evidence.anchor.snapshot_id == self.plan.snapshot_id
        return dict(completed_steps=self._job_index, step_count=len(self.plan.steps),
                    last_observed_step_index=self._last_verification_index,
                    job_id=job_id, job_state=result.state if isinstance(result, VerificationResult)
                    and type(result.state) is str and result.state in known_states else None,
                    result_evidence_ready=ready,
                    can_observe=(self._state.status == Status.RECOVERY_REQUIRED
                        and self._state.reason in ('verification_status_pending', 'verification_evidence_pending')
                        and getattr(self.verification, 'supports_observation', True) is True
                        and callable(getattr(self.verification, 'observe', None))))

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
            self._prepare_courses()
            state.stage = 'E4'
        elif stage == 'E4':
            state.cards.append(self._generate_card())
            if self.request.intent == 'explain':
                state.status = Status.COMPLETED
            else:
                state.stage = 'E5'
                state.status = (Status.WAITING_APPROVAL if self.request.intent == 'verify' and not self._needs_replan
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

    def _prepare_courses(self):
        # Retrieval is teaching context, never a replacement for the fixed
        # engineering observation used by approval, execution and checkpoints.
        self._course_evidence = self._evidence
        self._course_refs = ()
        self._state.course_omitted = ()
        self._state.course_status = 'disabled'
        self._state.course_diagnostics = {}
        if self.course_provider is None:
            return
        from .courses import pack_courses
        try:
            bundle = self.course_provider.prepare(self.request.question, (self._evidence.concept,))
            evidence, refs, status, omitted = pack_courses(self._evidence, bundle)
            if not valid_evidence(evidence, self.request):
                raise ValueError('invalid_course_context')
        except Exception:
            self._state.course_status = 'unavailable'
            return
        self._course_evidence, self._course_refs = evidence, refs
        self._state.course_status, self._state.course_omitted = status, omitted
        self._state.course_diagnostics = evidence.course_diagnostics

    def _generate_card(self) -> Card:
        state = self._state
        context = CardContext(self.request.question, state.level, self._course_evidence or self._evidence,
                              course_refs=self._course_refs, course_status=state.course_status)
        assessment = assess(context.evidence)
        if assessment.status != 'supported':
            state.reason = assessment.reason
        if self.provider is not None and assessment.status == 'supported':
            for attempt in range(2):
                if state.model_calls >= self.max_model_calls:
                    state.reason = 'model_budget_exceeded'
                    break
                state.model_calls += 1
                context = replace(context, repair=attempt == 1)
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
        if kind not in {'cancel', 'finish', 'hint_next', 'refresh_evidence', 'execute_plan', 'observe_verification',
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
            self._observation_only = False
            state.stage, state.status = 'E6', Status.RUNNING
        elif (kind == 'observe_verification' and self.request.intent == 'verify'
                and self.verification_progress['can_observe'] and not grant_ref):
            self._grant_ref = ''
            self._observation_only = True
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
            if isinstance(self.plan, DetailedPlan):
                PlanChecker().check(self.plan)
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
            if isinstance(self.plan, DetailedPlan) and (current.anchor is None or
                    current.anchor.snapshot_id != self.plan.snapshot_id):
                state.status, state.reason = Status.FAILED, 'snapshot_mismatch'
                return
            if not self._observation_only and not self._authorized(self._grant_ref):
                state.status, state.reason = Status.FAILED, 'approval_denied'
                return
            key = f'{state.run_id}:{self.plan.fingerprint}:{self._job_index}'
            if self._observation_only:
                result = self.verification.observe(self.plan, self._job_index, key)
            else:
                result = self.verification.execute(self.plan, self._job_index, self._grant_ref, key)
        except Exception as error:
            # Unknown submission outcome never causes automatic retry.
            code = getattr(error, 'code', '')
            if self._observation_only and code in ('TIMEOUT', 'SERVICE_UNAVAILABLE'):
                previous_job = self.verification_progress['job_id']
                self._last_verification = VerificationResult('Unknown', None, None, previous_job)
                state.status, state.reason = Status.RECOVERY_REQUIRED, 'verification_status_pending'
            elif self._observation_only and isinstance(error, EvidenceError) and code in (
                    'unresolved_evidence', 'incomplete_report', 'diagnostic_missing'):
                state.status, state.reason = Status.RECOVERY_REQUIRED, 'verification_evidence_pending'
            else:
                state.status, state.reason = Status.FAILED, 'verification_unavailable'
            return
        self._last_verification, self._last_verification_index = result, self._job_index
        if not isinstance(result, VerificationResult) or result.state != 'Succeeded':
            if isinstance(result, VerificationResult) and result.state in {
                    'Created', 'Validating', 'Queued', 'Running', 'ValidatingArtifact', 'Cancelling', 'Unknown'}:
                state.status, state.reason = Status.RECOVERY_REQUIRED, 'verification_status_pending'
                return
            state.status, state.reason = Status.FAILED, 'job_not_succeeded'
            return
        if not valid_evidence(result.evidence, self.request):
            if result.evidence is None:
                state.status, state.reason = Status.RECOVERY_REQUIRED, 'verification_evidence_pending'
            else:
                state.status, state.reason = Status.FAILED, 'invalid_result_evidence'
            return
        if result.job_id is not None and result.job_id != result.evidence.job_id:
            state.status, state.reason = Status.FAILED, 'invalid_result_evidence'
            return
        outcome = OutcomeChecker().compare(
            self._baseline_evidence or self._evidence, result.evidence, self.request.revision,
            project_id=self.request.project_id, matches_expected=result.matches_expected,
            seen_job_ids=(current.job_id,) + tuple(r.evidence.job_id for r in self._results),
            snapshot_id=self.plan.snapshot_id if isinstance(self.plan, DetailedPlan) else None)
        if outcome.state == 'rejected':
            state.status, state.reason = Status.FAILED, outcome.reason
            return
        seen_jobs = {self._evidence.job_id} | {r.evidence.job_id for r in self._results}
        if (result.evidence.job_id in seen_jobs
                or result.evidence.evidence_id in {current.evidence_id, self._evidence.evidence_id}
                or any(result.evidence.evidence_id == r.evidence.evidence_id for r in self._results)):
            state.status, state.reason = Status.FAILED, 'reused_job_result'
            return
        self._results.append(result)
        self._job_index += 1
        if (self._job_index == len(self.plan.steps) or
                outcome.state == 'mismatch' and isinstance(self.plan, DetailedPlan)
                and 'observation_mismatch' in self.plan.stop_conditions):
            state.stage = 'E7'
        elif self._observation_only:
            state.stage, state.status, state.reason = 'E5', Status.WAITING_APPROVAL, 'successor_approval_required'

    def _feedback(self):
        flags = [result.matches_expected for result in self._results]
        matches = False if any(flag is False for flag in flags) else (
            True if all(flag is True for flag in flags) else None)
        if matches is True:
            hint = '本次测试覆盖范围内的观察与预期一致；不能据此认定整个设计正确或已掌握概念。'
        elif matches is False:
            hint = '本次测试覆盖范围内的观察与预期不一致，请重新核对假设与证据。'
        else:
            hint = '工具任务已完成，但没有完整的功能判定；不能据此认定功能正确或已掌握概念。'
        limits = ('仅覆盖当前计划与当前版本。',)
        if any(result.evidence.origin == 'fixture' for result in self._results):
            limits += ('离线示例结果，不是真实 EDA 验证。',)
        self._state.cards.append(Card(self._state.level, hint, '你如何解释这次观察？',
                                      tuple(result.evidence.evidence_id for result in self._results), 'rule', limits))
        if matches is False:
            self._grant_ref = ''
            self._needs_replan = True
            self._state.status, self._state.reason = Status.WAITING_EVIDENCE, 'recollect_and_replan'
        else:
            self._state.status = Status.COMPLETED
            if matches is None:
                self._state.reason = 'outcome_indeterminate'
