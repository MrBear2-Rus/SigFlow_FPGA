@echo off
rem SigFlow Edu Agent launcher (Windows).
rem
rem The host sets SIGFLOW_EDU_AGENT_EXECUTABLE to this file and starts it with a
rem single argument, --bootstrap-stdin.  The bootstrap line arrives on stdin.
rem
rem SIGFLOW_EDU_AGENT_PYTHON selects the interpreter; default is "python".
rem The package is imported from the adjacent src/ tree via PYTHONPATH, so no
rem installation step is required for development or for a portable install.

setlocal
set "here=%~dp0"
if defined SIGFLOW_EDU_AGENT_PYTHON (
  set "PY=%SIGFLOW_EDU_AGENT_PYTHON%"
) else (
  set "PY=python"
)
set "PYTHONPATH=%here%..\src;%PYTHONPATH%"
"%PY%" -m sigflow_edu_agent %*
set "code=%ERRORLEVEL%"
endlocal & exit /b %code%
