[CmdletBinding()]
param(
    [ValidateSet('app')]
    [string]$Mode = 'app',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [switch]$Build,
    [switch]$Reconfigure,
    [switch]$KillExisting,
    [switch]$Wait,

    [int]$TimeoutSeconds = 0,

    [string]$VsDevCmd = 'D:\CodePrograms\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$QtRoot = 'D:\CodePrograms\Qt\5.15.1\msvc2019_64',
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-Info {
    param([string]$Message)
    Write-Host "[run] $Message"
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

function Invoke-ExternalCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory = (Get-Location).Path
    )

    $commandLine = $FilePath
    $argsArray = @($ArgumentList)
    if ($argsArray.Count -gt 0) {
        $commandLine += ' ' + ($argsArray -join ' ')
    }

    Write-Info $commandLine
    Push-Location $WorkingDirectory
    try {
        & $FilePath @argsArray
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code ${LASTEXITCODE}: $commandLine"
        }
    }
    finally {
        Pop-Location
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$projectFile = Join-Path $repoRoot 'Gles2AsyncRender.pro'
if (-not (Test-Path -LiteralPath $projectFile)) {
    throw "Project file was not found: $projectFile"
}

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $buildDirName = if ($Configuration -eq 'Debug') { 'qt5151-debug' } else { 'qt5151-release' }
    $BuildDir = Join-Path $repoRoot ('build\' + $buildDirName)
}

$qtBinDir = Join-Path $QtRoot 'bin'
$qmakeExe = Join-Path $qtBinDir 'qmake.exe'
$platformPluginPath = Join-Path $QtRoot 'plugins\platforms'
if (-not (Test-Path -LiteralPath $qmakeExe)) {
    throw "qmake.exe was not found: $qmakeExe"
}

Import-BatchEnvironment -BatchPath $VsDevCmd

$env:QTDIR = $QtRoot
$env:QT_QPA_PLATFORM_PLUGIN_PATH = $platformPluginPath
$env:Path = $qtBinDir + ';' + $env:Path

$nmakeCommand = Get-Command 'nmake.exe' -ErrorAction SilentlyContinue
$nmakeExe = if ($nmakeCommand) { $nmakeCommand.Source } else { $null }
if ([string]::IsNullOrWhiteSpace($nmakeExe)) {
    $vsVcRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $VsDevCmd))
    $toolRoot = Join-Path $vsVcRoot 'Tools\MSVC'
    if (Test-Path -LiteralPath $toolRoot) {
        $candidate = Get-ChildItem -LiteralPath $toolRoot -Filter 'nmake.exe' -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -like '*Hostx64\x64\nmake.exe' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($candidate) {
            $nmakeExe = $candidate.FullName
        }
    }
}

if ($KillExisting) {
    $running = @(Get-Process -Name 'Gles2AsyncRender' -ErrorAction SilentlyContinue)
    if ($running) {
        Write-Info "Stopping existing Gles2AsyncRender processes."
        $running | Stop-Process -Force
    }
}

if (-not (Test-Path -LiteralPath $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
}

$makefilePath = Join-Path $BuildDir 'Makefile'
if ($Reconfigure -or $Build -or -not (Test-Path -LiteralPath $makefilePath)) {
    Invoke-ExternalCommand -FilePath $qmakeExe -ArgumentList @($projectFile) -WorkingDirectory $BuildDir
}

if ($Build) {
    $buildTarget = if ($Configuration -eq 'Debug') { 'debug' } else { 'release' }
    if ([string]::IsNullOrWhiteSpace($nmakeExe)) {
        throw "nmake.exe was not found after importing the VS build environment."
    }
    Invoke-ExternalCommand -FilePath $nmakeExe -ArgumentList @($buildTarget) -WorkingDirectory $BuildDir
}

$outputSubDir = if ($Configuration -eq 'Debug') { 'debug' } else { 'release' }
$exePath = Join-Path $BuildDir ($outputSubDir + '\Gles2AsyncRender.exe')
if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Executable was not found: $exePath. Run with -Build first."
}

$programArgs = @()
$programArgs = @($programArgs)

$workingDirectory = Split-Path -Parent $exePath

$commandLine = $exePath
if ($programArgs.Count -gt 0) {
    $commandLine += ' ' + ($programArgs -join ' ')
}

Write-Info $commandLine
$process = Start-Process -FilePath $exePath -ArgumentList $programArgs -WorkingDirectory $workingDirectory -PassThru
Write-Info ("Started PID {0}" -f $process.Id)

if ($Wait) {
    if ($TimeoutSeconds -gt 0) {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            throw "Process did not exit within $TimeoutSeconds seconds."
        }
    }
    else {
        $process.WaitForExit()
    }

    if ($process.ExitCode -ne 0) {
        throw "Program exited with code $($process.ExitCode)."
    }

    Write-Info ("Process exited with code {0}" -f $process.ExitCode)
}
