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
#   5. FATAL: headers under main/ using fixed-width ints (uint64_t, ...) without <cstdint>.
#   6. FATAL: #include "..." whose path case does not match the real file (Linux is case-sensitive).
#   7. WARN: deprecated std::filesystem::u8path outside main/platform/** (C++20; use Utf8Path()).
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

# --- Rule 5: fixed-width ints without <cstdint> in headers (fatal) ----------
$rule5 = New-Object System.Collections.ArrayList
$r5IntPattern = '\b(uint8_t|uint16_t|uint32_t|uint64_t|int8_t|int16_t|int32_t|int64_t)\b'
foreach ($f in $headerFiles) {
    $rel = Get-RelativePath $Root $f.FullName
    if (Test-Excluded $rel @('main\platform\') @()) { continue }
    $content = [System.IO.File]::ReadAllText($f.FullName)
    if ($content -notmatch $r5IntPattern) { continue }
    if ($content -match '#\s*include\s*<cstdint>') { continue }
    [void]$rule5.Add([pscustomobject]@{ file = $rel; line = 0; text = 'uses fixed-width ints without <cstdint>' })
}

# --- Rule 6: include path case mismatch (fatal; Linux is case-sensitive) ----
$rule6 = New-Object System.Collections.ArrayList
$caseMap = @{}
foreach ($f in $allMainFiles) {
    $relFwd = (Get-RelativePath $Root $f.FullName).Replace('\', '/')
    $caseMap[$relFwd.ToLower()] = $relFwd
}
$r6Pattern = '#\s*include\s+"([^"]+\.(?:h|hpp|hxx))"'
foreach ($f in @($allMainFiles | Where-Object { $_.Extension -match '(?i)\.(cpp|h|hpp|hxx|cc|cxx)$' })) {
    $rel = Get-RelativePath $Root $f.FullName
    $relFwd = $rel.Replace('\', '/')
    $dir = ($relFwd -replace '/[^/]+$', '')
    $n = 0
    foreach ($line in [System.IO.File]::ReadAllLines($f.FullName)) {
        $n++
        $m = [regex]::Match($line, $r6Pattern)
        if (-not $m.Success) { continue }
        $inc = $m.Groups[1].Value
        foreach ($cand in @(($dir + '/' + $inc), ('main/' + $inc))) {
            $key = $cand.ToLower()
            if ($caseMap.ContainsKey($key) -and $caseMap[$key] -cne $cand) {
                [void]$rule6.Add([pscustomobject]@{ file = $rel; line = $n;
                    text = ("include '{0}' but actual file is '{1}'" -f $inc, $caseMap[$key]) })
                break
            }
        }
    }
}

# --- Rule 7: deprecated std::filesystem::u8path (warn) ----------------------
$rule7 = New-Object System.Collections.ArrayList
$r7Pattern = '\bu8path\b'
foreach ($f in @($allMainFiles | Where-Object { $_.Extension -match '(?i)\.(cpp|h|hpp|hxx|cc|cxx)$' })) {
    $rel = Get-RelativePath $Root $f.FullName
    if (Test-Excluded $rel @('main\platform\') @()) { continue }
    $n = 0
    foreach ($line in [System.IO.File]::ReadAllLines($f.FullName)) {
        $n++
        if ($line -match $r7Pattern -and $line -notmatch '^\s*//') {
            [void]$rule7.Add([pscustomobject]@{ file = $rel; line = $n; text = $line.Trim() })
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
Write-Host "[Rule 5] headers missing <cstdint> (FATAL):"
Show-Hits "violations" $rule5
Write-Host "[Rule 6] include case mismatch (FATAL):"
Show-Hits "violations" $rule6
Write-Host "[Rule 7] deprecated std::filesystem::u8path (WARN):"
Show-Hits "warnings" $rule7
Write-Host ""
$fatal = $rule1.Count + $rule2.Count + $rule5.Count + $rule6.Count
Write-Host ("Summary: fatal={0} (r1={1}, r2={2}, r5={3}, r6={4}), warn={5} (r3={6}, r7={7}), report={8}" -f `
    $fatal, $rule1.Count, $rule2.Count, $rule5.Count, $rule6.Count, `
    ($rule3.Count + $rule7.Count), $rule3.Count, $rule7.Count, $rule4.Count)

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
