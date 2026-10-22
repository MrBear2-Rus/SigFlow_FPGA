@echo off
rem Trusted source-tree launcher for U1 with the optional U2 composition.
setlocal
set "sigflowU2Root=%~dp0..\.."
if defined SIGFLOW_EDU_AGENT_PYTHON (
  set "sigflowU2Python=%SIGFLOW_EDU_AGENT_PYTHON%"
) else (
  set "sigflowU2Python=%~dp0..\.venv\Scripts\python.exe"
)
set "PYTHONPATH=%sigflowU2Root%\agent\src;%sigflowU2Root%\sigflowAgent;%PYTHONPATH%"
"%sigflowU2Python%" -m sigflow_edu_agent --u2-teaching %*
set "sigflowU2Exit=%ERRORLEVEL%"
endlocal & exit /b %sigflowU2Exit%
