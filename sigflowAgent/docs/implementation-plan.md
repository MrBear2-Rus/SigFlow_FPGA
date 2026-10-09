# U2 Teaching Loop Implementation Plan

> **For agentic workers:** Use executing-plans to implement task-by-task in the user-selected sigflowAgent directory.

**Goal:** Bounded teaching loop with DeepSeek and offline verification.
**Architecture:** Domain state machine owns decisions; evidence/model/verification ports own external effects. Development driver does not replace UCAgent StageManager.
**Tech Stack:** Python >=3.11, dataclasses, Protocol, unittest, http.client, JSON.

## Constraints

All changes under sigflowAgent. No upstream/CMake changes, shell/RTL writing, real EDA execution or real L4 unlock. No credentials in tracked files/state/logs. Defaults: 32 steps, 4 model calls, at most 3 logical verification steps.

## Task 1: Loop and rule cards

Files: sigflow_edu_agent/domain.py, ports.py, pedagogy.py, loop.py, tests/test_loop.py.
Interfaces: TeachingLoop.step/run_until_pause/act; EvidenceSource.collect; CardProvider.generate; VerificationPort.authorized/execute.

- [x] Write tests for pause/finish/authorization/version/budget/idempotency.
- [x] Run `python -m unittest discover -s tests -v` and observe missing implementation.
- [x] Implement transitions, checked cards and one repair before fallback.
- [x] Rerun tests to pass.

```python
loop = TeachingLoop(RunRequest('p', 'r1', 'i', 'why', 'diagnose'), source)
assert loop.run_until_pause().status == Status.WAITING_STUDENT
assert loop.state.level == 1
```

## Task 2: DeepSeek

Files: sigflow_edu_agent/deepseek.py, tests/test_deepseek.py.
Interface: DeepSeekProvider.from_env().generate(CardContext) -> dict.

- [x] Write failing transport contract tests.
- [x] Implement HTTPS non-stream JSON Output, configurable model/key, time/byte limits and stable errors.
- [x] Test provider-to-loop degradation and secret redaction.

```json
{"model":"configured-model","stream":false,"response_format":{"type":"json_object"},"max_tokens":1024}
```

## Task 3: Entrypoint and review

Files: sigflow_edu_agent/__main__.py, demo.py, README.md, .env.example, .gitignore, pyproject.toml.

- [x] Implement `python -m sigflow_edu_agent --demo` and `--provider deepseek`.
- [x] Verify CLI, compileall and full tests.
- [x] Review auth bypass, stale evidence, budgets and secrets; fix findings.
- [x] Document tested environment and pending real-service integration.
