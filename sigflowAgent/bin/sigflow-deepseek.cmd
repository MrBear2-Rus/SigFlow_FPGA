@echo off
setlocal
set "PYTHONPATH=%~dp0.."
set "PYTHONUTF8=1"
if defined SIGFLOW_EDU_AGENT_PYTHON (
  "%SIGFLOW_EDU_AGENT_PYTHON%" -P -m sigflow_edu_agent.model_config %*
) else if exist "%~dp0..\.venv\Scripts\python.exe" (
  "%~dp0..\.venv\Scripts\python.exe" -P -m sigflow_edu_agent.model_config %*
) else (
  python -P -m sigflow_edu_agent.model_config %*
)
exit /b %errorlevel%
