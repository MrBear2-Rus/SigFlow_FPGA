@echo off
setlocal
set "PYTHONPATH=%~dp0.."
if defined SIGFLOW_EDU_AGENT_PYTHON (
  "%SIGFLOW_EDU_AGENT_PYTHON%" -P -m sigflow_edu_agent.host %*
) else (
  python -P -m sigflow_edu_agent.host %*
)
exit /b %errorlevel%
