# check_portability.ps1
# Cross-platform portability gate (see docs/CrossPlatformPlan.md, Appendix B/C.1).
#
# Rules:
#   1. FATAL: files under main/ (excluding main/jobs/PlatformProcess.* and
#      main/platform/**) must not include OS-specific headers
#      (windows.h / Windows.h / bcrypt.h / setupapi.h / debugapi.h / unistd.h).
#   2. FATAL: headers under main/ (excluding main/platform/**) must not use
#      Win32 types HANDLE / HMODULE / DWORD / LSTATUS / INVALID_HANDLE_VALUE.
#   3. WARN: bare "\\" path concatenation in main/**/*.cpp (excluding main/platform/**).
#   4. REPORT: ".exe"/".dll" literals outside main/platform/** and tests/**.
#
# Exits non-zero on rule 1 or rule 2 violations. Pass -Baseline to force exit 0
# (report-only mode for the P0 transitional state).

[CmdletBinding()]
param(
    [string]$Root = '',
    [switch]$Baseline,
    [int]$MaxDetails = 20
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = Split-Path -Parent $PSScriptRoot
}
$Root = [System.IO.Path]::GetFullPath($Root)

function Get-RelativePath([string]$base, [string]$full) {
    $baseUri = New-Object System.Uri(($base.TrimEnd('\') + '\'))
    $fullUri = New-Object System.Uri($full)
    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($fullUri).ToString()).Replace('/', '\')
}

function Test-Excluded([string]$rel, [string[]]$prefixes, [string[]]$suffixes) {
    foreach ($p in $prefixes) {
        if ($rel -like ($p + '*')) { return $true }
    }
    foreach ($s in $suffixes) {
        if ($rel -like ('*' + $s)) { return $true }
    }
    return $false
}

function Show-Hits([string]$title, $hits) {
    Write-Host ("  {0}: {1}" -f $title, $hits.Count)
    $shown = 0
    foreach ($h in $hits) {
        if ($shown -ge $MaxDetails) {
            Write-Host ("    ... ({0} more)" -f ($hits.Count - $shown))
            break
        }
        Write-Host ("    {0}:{1}: {2}" -f $h.file, $h.line, $h.text)
        $shown++
    }
}

$mainDir = Join-Path $Root 'main'
if (-not (Test-Path -LiteralPath $mainDir)) {
    Write-Error "main/ directory not found under root: $Root"
    exit 2
}

$allMainFiles = @(Get-ChildItem -LiteralPath $mainDir -Recurse -File)

# --- Rule 1: forbidden OS headers (fatal) ---------------------------------
$rule1 = New-Object System.Collections.ArrayList
$r1Pattern = '(?i)^\s*#\s*include\s*[<"](windows\.h|bcrypt\.h|setupapi\.h|debugapi\.h|unistd\.h)[>"]'
$r1ExcludePrefixes = @('main\platform\')
$r1ExcludeSuffixes = @('main\jobs\PlatformProcess.h', 'main\jobs\PlatformProcess.cpp')
foreach ($f in $allMainFiles) {
    $rel = Get-RelativePath $Root $f.FullName
    if (Test-Excluded $rel $r1ExcludePrefixes $r1ExcludeSuffixes) { continue }
    $n = 0
    foreach ($line in [System.IO.File]::ReadAllLines($f.FullName)) {
        $n++
        if ($line -match $r1Pattern) {
            [void]$rule1.Add([pscustomobject]@{ file = $rel; line = $n; text = $line.Trim() })
        }
    }
}

# --- Rule 2: Win32 types in headers (fatal) -------------------------------
$rule2 = New-Object System.Collections.ArrayList
$r2Patterns = @('\bHANDLE\b', '\bHMODULE\b', '\bDWORD\b', '\bBOOL\b', '\bLSTATUS\b', '\bINVALID_HANDLE_VALUE\b')
$r2ExcludePrefixes = @('main\platform\')
$headerFiles = @($allMainFiles | Where-Object { $_.Extension -match '(?i)\.(h|hpp|hxx)$' })
foreach ($f in $headerFiles) {
    $rel = Get-RelativePath $Root $f.FullName
    if (Test-Excluded $rel $r2ExcludePrefixes @()) { continue }
    $n = 0
    foreach ($line in [System.IO.File]::ReadAllLines($f.FullName)) {
        $n++
        foreach ($pat in $r2Patterns) {
            if ($line -cmatch $pat) {
                [void]$rule2.Add([pscustomobject]@{ file = $rel; line = $n; text = $line.Trim() })
                break
            }
        }
    }
}

# --- Rule 3: bare backslash path concatenation (warn) ----------------------
$rule3 = New-Object System.Collections.ArrayList
$r3Pattern = '"[\\]{2}"'
$r3ExcludePrefixes = @('main\platform\')
$cppFiles = @($allMainFiles | Where-Object { $_.Extension -match '(?i)\.(cpp|cc|cxx)$' })
foreach ($f in $cppFiles) {
    $rel = Get-RelativePath $Root $f.FullName
    if (Test-Excluded $rel $r3ExcludePrefixes @()) { continue }
    $n = 0
    foreach ($line in [System.IO.File]::ReadAllLines($f.FullName)) {
        $n++
        if ($line -match $r3Pattern) {
            [void]$rule3.Add([pscustomobject]@{ file = $rel; line = $n; text = $line.Trim() })
        }
    }
}

# --- Rule 4: .exe / .dll literals (report) --------------------------------
$rule4 = New-Object System.Collections.ArrayList
$r4Pattern = '(?i)\.(exe|dll)\b'
$r4ExcludePrefixes = @('main\platform\')
$r4TextExtensions = @('.cpp', '.h', '.hpp', '.hxx', '.cc', '.cxx', '.txt', '.cmake', '.ps1', '.py', '.json', '.in')
$allRepoFiles = @(Get-ChildItem -LiteralPath $Root -Recurse -File |
    Where-Object {
        $r4TextExtensions -contains $_.Extension.ToLower() -and
        $_.FullName -notmatch '(?i)\\(build|build-local|build-gcc|external|3rd|\.git|docs)\\'
    })
foreach ($f in $allRepoFiles) {
    $rel = Get-RelativePath $Root $f.FullName
    if ($rel -like 'tests\*') { continue }
    if (Test-Excluded $rel $r4ExcludePrefixes @()) { continue }
    $n = 0
    foreach ($line in [System.IO.File]::ReadAllLines($f.FullName)) {
        $n++
        if ($line -match $r4Pattern) {
            [void]$rule4.Add([pscustomobject]@{ file = $rel; line = $n; text = $line.Trim() })
        }
    }
}

# --- Report ----------------------------------------------------------------
Write-Host ""
Write-Host "=== SigFlow portability check ==="
Write-Host ("Root: {0}" -f $Root)
Write-Host ""
Write-Host "[Rule 1] forbidden OS headers (FATAL):"
Show-Hits "violations" $rule1
Write-Host "[Rule 2] Win32 types in headers (FATAL):"
Show-Hits "violations" $rule2
Write-Host "[Rule 3] bare backslash path concat (WARN):"
Show-Hits "warnings" $rule3
Write-Host "[Rule 4] .exe/.dll literals (REPORT):"
Show-Hits "occurrences" $rule4
Write-Host ""
$fatal = $rule1.Count + $rule2.Count
Write-Host ("Summary: fatal={0} (rule1={1}, rule2={2}), warn={3}, report={4}" -f `
    $fatal, $rule1.Count, $rule2.Count, $rule3.Count, $rule4.Count)

if ($fatal -gt 0 -and -not $Baseline) {
    Write-Host "RESULT: FAIL"
    exit 1
}
if ($fatal -gt 0) {
    Write-Host "RESULT: FAIL (baseline mode, exit 0)"
    exit 0
}
Write-Host "RESULT: PASS"
exit 0
