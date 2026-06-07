[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$Target = 'async_future_tests',

    [switch]$Configure,
    [switch]$CleanTarget,
    [switch]$RunTest,
    [switch]$KillStale,

    [int]$Parallel = 0,
    [int]$TimeoutSeconds = 180,
    [int]$TailLines = 120,

    [string]$VsDevCmd = 'D:\CodePrograms\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$QtRoot = 'D:\CodePrograms\Qt\5.15.1\msvc2019_64',
    [string]$CMakeExe = 'D:\CodePrograms\Qt\Tools\CMake_64\bin\cmake.exe',
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-Info {
    param([string]$Message)
    Write-Host "[build] $Message"
}

function Convert-ToFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Merge-ProcessPath {
    $procEnv = [System.Environment]::GetEnvironmentVariables('Process')
    $pathValue = $procEnv['Path']
    $pathUpperValue = $procEnv['PATH']
    $merged = @($pathValue, $pathUpperValue) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { $_ -split ';' } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -Unique

    [System.Environment]::SetEnvironmentVariable('PATH', $null, 'Process')
    [System.Environment]::SetEnvironmentVariable('Path', ($merged -join ';'), 'Process')
}

function Import-BatchEnvironment {
    param([Parameter(Mandatory = $true)][string]$BatchPath)

    if (-not (Test-Path -LiteralPath $BatchPath)) {
        throw "VS environment script was not found: $BatchPath"
    }

    $tempFile = Join-Path ([System.IO.Path]::GetTempPath()) ("codex-vcvars-" + [System.Guid]::NewGuid().ToString('N') + ".cmd")
    @(
        '@echo off'
        ('call "{0}" >nul' -f $BatchPath)
        'if errorlevel 1 exit /b %errorlevel%'
        'set'
    ) | Set-Content -LiteralPath $tempFile -Encoding Ascii

    try {
        $output = & (Join-Path $env:SystemRoot 'System32\cmd.exe') /d /c $tempFile
        $exitCode = $LASTEXITCODE
    }
    finally {
        Remove-Item -LiteralPath $tempFile -Force -ErrorAction SilentlyContinue
    }

    if ($exitCode -ne 0) {
        throw "Failed to import VS build environment from: $BatchPath"
    }

    $seenNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($line in $output) {
        if ($line -notmatch '^(.*?)=(.*)$') {
            continue
        }

        $name = $Matches[1]
        $value = $Matches[2]
        if ([string]::IsNullOrWhiteSpace($name) -or $name.StartsWith('=')) {
            continue
        }

        if (-not $seenNames.Add($name)) {
            continue
        }

        Set-Item -Path ("Env:" + $name) -Value $value
    }
}

function Get-ProcessTreeIds {
    param([Parameter(Mandatory = $true)][int]$ProcessId)

    $allProcesses = @(Get-CimInstance Win32_Process)
    $childrenByParent = @{}
    foreach ($proc in $allProcesses) {
        $parentId = [int]$proc.ParentProcessId
        if (-not $childrenByParent.ContainsKey($parentId)) {
            $childrenByParent[$parentId] = New-Object 'System.Collections.Generic.List[int]'
        }
        $childrenByParent[$parentId].Add([int]$proc.ProcessId)
    }

    $result = New-Object 'System.Collections.Generic.List[int]'
    $stack = New-Object 'System.Collections.Generic.Stack[int]'
    $stack.Push($ProcessId)
    while ($stack.Count -gt 0) {
        $id = $stack.Pop()
        $result.Add($id)
        if ($childrenByParent.ContainsKey($id)) {
            foreach ($childId in $childrenByParent[$id]) {
                $stack.Push($childId)
            }
        }
    }

    return @($result)
}

function Stop-ProcessTree {
    param([Parameter(Mandatory = $true)][int]$ProcessId)

    $ids = @(Get-ProcessTreeIds -ProcessId $ProcessId)
    [array]::Reverse($ids)
    foreach ($id in $ids) {
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
    }
}

function Stop-StaleBuildProcesses {
    param(
        [Parameter(Mandatory = $true)][string]$FullBuildDir,
        [Parameter(Mandatory = $true)][string]$RepoRoot
    )

    $buildDirBackslash = $FullBuildDir.ToLowerInvariant()
    $buildDirSlash = $buildDirBackslash.Replace('\', '/')
    $relativeBuildDir = ($FullBuildDir.Substring($RepoRoot.Length).TrimStart('\', '/')).ToLowerInvariant()
    $relativeBuildDirSlash = $relativeBuildDir.Replace('\', '/')

    $names = @('cmake.exe', 'ninja.exe')
    $stale = @(Get-CimInstance Win32_Process |
        Where-Object {
            $names -contains $_.Name -and
            -not [string]::IsNullOrWhiteSpace($_.CommandLine) -and
            (
                $_.CommandLine.ToLowerInvariant().Contains($buildDirBackslash) -or
                $_.CommandLine.ToLowerInvariant().Contains($buildDirSlash) -or
                $_.CommandLine.ToLowerInvariant().Contains($relativeBuildDir) -or
                $_.CommandLine.ToLowerInvariant().Contains($relativeBuildDirSlash)
            )
        })

    if (-not $stale) {
        Write-Info "No stale CMake/Ninja processes for this build directory."
        return
    }

    foreach ($proc in $stale) {
        Write-Info ("Stopping stale {0} PID {1}" -f $proc.Name, $proc.ProcessId)
        Stop-ProcessTree -ProcessId ([int]$proc.ProcessId)
    }
}

function Write-LogTail {
    param(
        [Parameter(Mandatory = $true)][string]$StdoutLog,
        [Parameter(Mandatory = $true)][string]$StderrLog,
        [int]$Lines = 120
    )

    if (Test-Path -LiteralPath $StdoutLog) {
        Write-Host "[build] stdout tail: $StdoutLog"
        Get-Content -LiteralPath $StdoutLog -Tail $Lines
    }

    if ((Test-Path -LiteralPath $StderrLog) -and ((Get-Item -LiteralPath $StderrLog).Length -gt 0)) {
        Write-Host "[build] stderr tail: $StderrLog"
        Get-Content -LiteralPath $StderrLog -Tail $Lines
    }
}

function ConvertTo-ProcessArgumentString {
    param([string[]]$ArgumentList = @())

    $quoted = foreach ($arg in $ArgumentList) {
        if ($null -eq $arg) {
            '""'
        }
        elseif ($arg -eq '') {
            '""'
        }
        elseif ($arg -notmatch '[\s"]') {
            $arg
        }
        else {
            '"' + ($arg -replace '"', '\"') + '"'
        }
    }

    return ($quoted -join ' ')
}

function Write-TextFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [AllowNull()][string]$Text
    )

    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Invoke-LoggedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$LogStem,
        [int]$TimeoutSeconds = 180,
        [int]$TailLines = 120
    )

    $stdoutLog = $LogStem + '.stdout.log'
    $stderrLog = $LogStem + '.stderr.log'
    Remove-Item -LiteralPath $stdoutLog, $stderrLog -Force -ErrorAction SilentlyContinue

    $commandLine = $FilePath
    if ($ArgumentList.Count -gt 0) {
        $commandLine += ' ' + ($ArgumentList -join ' ')
    }
    Write-Info $commandLine
    Write-Info "Log: $stdoutLog"

    $processInfo = New-Object System.Diagnostics.ProcessStartInfo
    $processInfo.FileName = $FilePath
    $processInfo.Arguments = ConvertTo-ProcessArgumentString -ArgumentList $ArgumentList
    $processInfo.WorkingDirectory = $WorkingDirectory
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $processInfo
    if (-not $process.Start()) {
        throw "Failed to start command: $commandLine"
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $completed = if ($TimeoutSeconds -gt 0) {
        $process.WaitForExit($TimeoutSeconds * 1000)
    }
    else {
        $process.WaitForExit()
        $true
    }

    if (-not $completed) {
        Write-Info ("Timeout after {0}s; stopping PID {1} and children." -f $TimeoutSeconds, $process.Id)
        Stop-ProcessTree -ProcessId $process.Id
        $stdoutTask.Wait(1000) | Out-Null
        $stderrTask.Wait(1000) | Out-Null
        Write-TextFile -Path $stdoutLog -Text $stdoutTask.Result
        Write-TextFile -Path $stderrLog -Text $stderrTask.Result
        Write-LogTail -StdoutLog $stdoutLog -StderrLog $stderrLog -Lines $TailLines
        throw "Command timed out after $TimeoutSeconds seconds: $commandLine"
    }

    $process.WaitForExit()
    $stdoutTask.Wait()
    $stderrTask.Wait()
    Write-TextFile -Path $stdoutLog -Text $stdoutTask.Result
    Write-TextFile -Path $stderrLog -Text $stderrTask.Result
    Write-LogTail -StdoutLog $stdoutLog -StderrLog $stderrLog -Lines $TailLines

    if ($process.ExitCode -ne 0) {
        throw "Command failed with exit code $($process.ExitCode): $commandLine"
    }
}

$repoRoot = Convert-ToFullPath (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt'))) {
    throw "CMakeLists.txt was not found under: $repoRoot"
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $buildDirName = if ($Configuration -eq 'Debug') { 'win-local-clang-debug' } else { 'win-local-clang-release' }
    $BuildDir = Join-Path $repoRoot ('build\' + $buildDirName)
}
$BuildDir = Convert-ToFullPath $BuildDir

if (-not (Test-Path -LiteralPath $CMakeExe)) {
    $cmakeCommand = Get-Command 'cmake.exe' -ErrorAction SilentlyContinue
    if (-not $cmakeCommand) {
        throw "cmake.exe was not found. Pass -CMakeExe explicitly."
    }
    $CMakeExe = $cmakeCommand.Source
}

$qtBinDir = Join-Path $QtRoot 'bin'
if (-not (Test-Path -LiteralPath $qtBinDir)) {
    throw "Qt bin directory was not found: $qtBinDir"
}

Merge-ProcessPath
Import-BatchEnvironment -BatchPath $VsDevCmd
Merge-ProcessPath

$env:CMAKE_PREFIX_PATH = $QtRoot
$env:QTDIR = $QtRoot
$env:Path = $qtBinDir + ';' + $env:Path

if ($KillStale) {
    Stop-StaleBuildProcesses -FullBuildDir $BuildDir -RepoRoot $repoRoot
}

if (-not (Test-Path -LiteralPath $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
}

$logDir = Join-Path $BuildDir 'codex-build-logs'
if (-not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$buildNinja = Join-Path $BuildDir 'build.ninja'
if ($Configure -or -not (Test-Path -LiteralPath $buildNinja)) {
    $configureArgs = @(
        '-S', $repoRoot,
        '-B', $BuildDir,
        '-G', 'Ninja',
        ('-DCMAKE_BUILD_TYPE={0}' -f $Configuration),
        ('-DCMAKE_PREFIX_PATH={0}' -f $QtRoot),
        '-DCMAKE_CXX_COMPILER=clang-cl',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
    )
    Invoke-LoggedCommand `
        -FilePath $CMakeExe `
        -ArgumentList $configureArgs `
        -WorkingDirectory $repoRoot `
        -LogStem (Join-Path $logDir ($timestamp + '-configure')) `
        -TimeoutSeconds $TimeoutSeconds `
        -TailLines $TailLines
}

if ($CleanTarget) {
    $ninjaCommand = Get-Command 'ninja.exe' -ErrorAction SilentlyContinue
    if (-not $ninjaCommand) {
        throw "ninja.exe was not found after importing the build environment."
    }
    Invoke-LoggedCommand `
        -FilePath $ninjaCommand.Source `
        -ArgumentList @('-C', $BuildDir, '-t', 'clean', $Target) `
        -WorkingDirectory $repoRoot `
        -LogStem (Join-Path $logDir ($timestamp + '-clean-' + $Target)) `
        -TimeoutSeconds $TimeoutSeconds `
        -TailLines $TailLines
}

$buildArgs = @('--build', $BuildDir, '--target', $Target, '--verbose')
if ($Parallel -gt 0) {
    $buildArgs += @('--parallel', [string]$Parallel)
}
else {
    $buildArgs += '--parallel'
}

Invoke-LoggedCommand `
    -FilePath $CMakeExe `
    -ArgumentList $buildArgs `
    -WorkingDirectory $repoRoot `
    -LogStem (Join-Path $logDir ($timestamp + '-build-' + $Target)) `
    -TimeoutSeconds $TimeoutSeconds `
    -TailLines $TailLines

if ($RunTest) {
    $testExe = Join-Path $BuildDir ($Target + '.exe')
    if (-not (Test-Path -LiteralPath $testExe)) {
        throw "Test executable was not found: $testExe"
    }

    Invoke-LoggedCommand `
        -FilePath $testExe `
        -ArgumentList @() `
        -WorkingDirectory $BuildDir `
        -LogStem (Join-Path $logDir ($timestamp + '-test-' + $Target)) `
        -TimeoutSeconds $TimeoutSeconds `
        -TailLines $TailLines
}

Write-Info "Done."
