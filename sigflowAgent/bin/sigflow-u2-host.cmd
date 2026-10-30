@echo off
setlocal
set "PYTHONPATH=%~dp0.."
if defined SIGFLOW_EDU_AGENT_PYTHON (
  "%SIGFLOW_EDU_AGENT_PYTHON%" -P -m sigflow_teaching_core.host %*
) else (
  python -P -m sigflow_teaching_core.host %*
)
exit /b %errorlevel%
