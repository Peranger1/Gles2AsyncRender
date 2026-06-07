[CmdletBinding()]
param(
    [ValidateSet('Auto', 'CMakeNinja', 'CMakeVS', 'QMake')]
    [string]$BuildSystem = 'Auto',

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Debug',

    [string]$Target,
    [switch]$Configure,
    [switch]$ConfigureOnly,
    [switch]$Clean,
    [switch]$KillStale,

    [int]$Parallel = 0,
    [int]$TimeoutSeconds = 300,
    [int]$TailLines = 120,

    [string]$ProjectRoot,
    [string]$BuildRoot,
    [string]$BuildDir,
    [string]$QtRoot,

    [string]$CMakeExe,
    [string]$QMakeExe,
    [string]$NinjaExe,
    [string]$VsDevCmd,

    [string]$VisualStudioGenerator = 'Visual Studio 17 2022',
    [string]$Architecture = 'x64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$script:IsWindowsHost = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
    [System.Runtime.InteropServices.OSPlatform]::Windows)

function Write-Info {
    param([string]$Message)
    Write-Host "[qt-build] $Message"
}

function Convert-ToFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Get-DefaultProjectRoot {
    $scriptParent = Convert-ToFullPath (Split-Path -Parent $PSScriptRoot)
    if ((Test-Path -LiteralPath (Join-Path $scriptParent 'CMakeLists.txt')) -or
        (Get-ChildItem -LiteralPath $scriptParent -Filter '*.pro' -File -ErrorAction SilentlyContinue | Select-Object -First 1)) {
        return $scriptParent
    }

    return Convert-ToFullPath (Get-Location).Path
}

function Resolve-CommandPath {
    param(
        [string]$ExplicitPath,
        [Parameter(Mandatory = $true)][string]$CommandName
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath)) {
            throw "$CommandName was not found: $ExplicitPath"
        }
        return $ExplicitPath
    }

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "$CommandName was not found on PATH. Pass the explicit path."
    }

    return $command.Source
}

function Add-PathFront {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "PATH entry does not exist: $Path"
    }

    $parts = @($env:Path -split ';') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    $parts = @($Path) + @($parts | Where-Object { $_ -ne $Path })
    $env:Path = ($parts -join ';')
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

function Find-VsDevCmd {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($install)) {
            $candidate = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $candidate) {
                return $candidate
            }
        }
    }

    foreach ($edition in @('Enterprise', 'Professional', 'Community', 'BuildTools')) {
        $candidate = Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    return $null
}

function Import-BatchEnvironment {
    param([Parameter(Mandatory = $true)][string]$BatchPath)

    if (-not (Test-Path -LiteralPath $BatchPath)) {
        throw "VS environment script was not found: $BatchPath"
    }

    $tempFile = Join-Path ([System.IO.Path]::GetTempPath()) ("qt-build-vcvars-" + [System.Guid]::NewGuid().ToString('N') + ".cmd")
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
        [Parameter(Mandatory = $true)][string]$Root
    )

    if (-not $script:IsWindowsHost) {
        return
    }

    $buildDirLower = $FullBuildDir.ToLowerInvariant()
    $buildDirSlash = $buildDirLower.Replace('\', '/')
    $relativeBuildDir = ($FullBuildDir.Substring($Root.Length).TrimStart('\', '/')).ToLowerInvariant()
    $relativeBuildDirSlash = $relativeBuildDir.Replace('\', '/')
    $names = @('cmake.exe', 'ninja.exe', 'msbuild.exe', 'nmake.exe', 'jom.exe')

    $stale = @(Get-CimInstance Win32_Process |
        Where-Object {
            $names -contains $_.Name -and
            -not [string]::IsNullOrWhiteSpace($_.CommandLine) -and
            (
                $_.CommandLine.ToLowerInvariant().Contains($buildDirLower) -or
                $_.CommandLine.ToLowerInvariant().Contains($buildDirSlash) -or
                $_.CommandLine.ToLowerInvariant().Contains($relativeBuildDir) -or
                $_.CommandLine.ToLowerInvariant().Contains($relativeBuildDirSlash)
            )
        })

    if (-not $stale) {
        Write-Info "No stale build processes for this build directory."
        return
    }

    foreach ($proc in $stale) {
        Write-Info ("Stopping stale {0} PID {1}" -f $proc.Name, $proc.ProcessId)
        Stop-ProcessTree -ProcessId ([int]$proc.ProcessId)
    }
}

function ConvertTo-ProcessArgumentString {
    param([string[]]$ArgumentList = @())

    $quoted = foreach ($arg in $ArgumentList) {
        if ($null -eq $arg -or $arg -eq '') {
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

function Write-LogTail {
    param(
        [Parameter(Mandatory = $true)][string]$StdoutLog,
        [Parameter(Mandatory = $true)][string]$StderrLog,
        [int]$Lines = 120
    )

    if (Test-Path -LiteralPath $StdoutLog) {
        Write-Host "[qt-build] stdout tail: $StdoutLog"
        Get-Content -LiteralPath $StdoutLog -Tail $Lines
    }

    if ((Test-Path -LiteralPath $StderrLog) -and ((Get-Item -LiteralPath $StderrLog).Length -gt 0)) {
        Write-Host "[qt-build] stderr tail: $StderrLog"
        Get-Content -LiteralPath $StderrLog -Tail $Lines
    }
}

function Invoke-LoggedCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$LogStem,
        [int]$TimeoutSeconds = 300,
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
        if ($script:IsWindowsHost) {
            Stop-ProcessTree -ProcessId $process.Id
        }
        else {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
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

function Get-QMakeBuildTool {
    if ($script:IsWindowsHost) {
        $jom = Get-Command 'jom.exe' -ErrorAction SilentlyContinue
        if ($jom) {
            return $jom.Source
        }

        return Resolve-CommandPath -CommandName 'nmake.exe'
    }

    $make = Get-Command 'make' -ErrorAction SilentlyContinue
    if ($make) {
        return $make.Source
    }

    throw "make was not found on PATH."
}

$root = if ([string]::IsNullOrWhiteSpace($ProjectRoot)) { Get-DefaultProjectRoot } else { Convert-ToFullPath $ProjectRoot }
if (-not (Test-Path -LiteralPath $root)) {
    throw "Project root was not found: $root"
}

$cmakeLists = Join-Path $root 'CMakeLists.txt'
$proFiles = @(Get-ChildItem -LiteralPath $root -Filter '*.pro' -File -ErrorAction SilentlyContinue)

if ($BuildSystem -eq 'Auto') {
    if (Test-Path -LiteralPath $cmakeLists) {
        $BuildSystem = 'CMakeNinja'
    }
    elseif ($proFiles.Count -gt 0) {
        $BuildSystem = 'QMake'
    }
    else {
        throw "No CMakeLists.txt or .pro file was found in: $root"
    }
}

if ($BuildSystem -like 'CMake*' -and -not (Test-Path -LiteralPath $cmakeLists)) {
    throw "CMakeLists.txt was not found in: $root"
}
if ($BuildSystem -eq 'QMake' -and $proFiles.Count -eq 0) {
    throw "No .pro file was found in: $root"
}

if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $root 'build'
}
$BuildRoot = Convert-ToFullPath $BuildRoot

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $buildDirName = switch ($BuildSystem) {
        'CMakeNinja' { 'cmake-ninja-' + $Configuration.ToLowerInvariant() }
        'CMakeVS' { 'cmake-vs2022' }
        'QMake' { 'qmake-' + $Configuration.ToLowerInvariant() }
    }
    $BuildDir = Join-Path $BuildRoot $buildDirName
}
$BuildDir = Convert-ToFullPath $BuildDir

if (-not (Test-Path -LiteralPath $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
}

$logDir = Join-Path $BuildDir 'qt-build-logs'
if (-not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'

Merge-ProcessPath

if (-not [string]::IsNullOrWhiteSpace($QtRoot)) {
    $qtBin = Join-Path $QtRoot 'bin'
    Add-PathFront -Path $qtBin
    $env:QTDIR = $QtRoot
    $env:CMAKE_PREFIX_PATH = $QtRoot
}

if ($script:IsWindowsHost -and ($BuildSystem -eq 'CMakeVS' -or $BuildSystem -eq 'QMake')) {
    if ([string]::IsNullOrWhiteSpace($VsDevCmd)) {
        $VsDevCmd = Find-VsDevCmd
    }
    if ([string]::IsNullOrWhiteSpace($VsDevCmd)) {
        throw "vcvars64.bat was not found. Pass -VsDevCmd explicitly."
    }
    Import-BatchEnvironment -BatchPath $VsDevCmd
    Merge-ProcessPath
}

if ($KillStale) {
    Stop-StaleBuildProcesses -FullBuildDir $BuildDir -Root $root
}

switch ($BuildSystem) {
    'CMakeNinja' {
        $cmake = Resolve-CommandPath -ExplicitPath $CMakeExe -CommandName 'cmake'
        $ninja = Resolve-CommandPath -ExplicitPath $NinjaExe -CommandName 'ninja'
        $buildNinja = Join-Path $BuildDir 'build.ninja'

        if ($Configure -or -not (Test-Path -LiteralPath $buildNinja)) {
            $args = @(
                '-S', $root,
                '-B', $BuildDir,
                '-G', 'Ninja',
                ('-DCMAKE_MAKE_PROGRAM={0}' -f $ninja),
                ('-DCMAKE_BUILD_TYPE={0}' -f $Configuration),
                '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
            )
            if (-not [string]::IsNullOrWhiteSpace($QtRoot)) {
                $args += ('-DCMAKE_PREFIX_PATH={0}' -f $QtRoot)
            }

            Invoke-LoggedCommand -FilePath $cmake -ArgumentList $args -WorkingDirectory $root -LogStem (Join-Path $logDir ($timestamp + '-configure')) -TimeoutSeconds $TimeoutSeconds -TailLines $TailLines
        }

        if ($ConfigureOnly) {
            break
        }

        if ($Clean) {
            $cleanArgs = @('--build', $BuildDir, '--target', 'clean', '--verbose')
            Invoke-LoggedCommand -FilePath $cmake -ArgumentList $cleanArgs -WorkingDirectory $root -LogStem (Join-Path $logDir ($timestamp + '-clean')) -TimeoutSeconds $TimeoutSeconds -TailLines $TailLines
        }

        $buildArgs = @('--build', $BuildDir, '--verbose')
        if (-not [string]::IsNullOrWhiteSpace($Target)) {
            $buildArgs += @('--target', $Target)
        }
        $buildArgs += if ($Parallel -gt 0) { @('--parallel', [string]$Parallel) } else { @('--parallel') }
        Invoke-LoggedCommand -FilePath $cmake -ArgumentList $buildArgs -WorkingDirectory $root -LogStem (Join-Path $logDir ($timestamp + '-build')) -TimeoutSeconds $TimeoutSeconds -TailLines $TailLines
    }

    'CMakeVS' {
        if (-not $script:IsWindowsHost) {
            throw "CMakeVS is only supported on Windows."
        }

        $cmake = Resolve-CommandPath -ExplicitPath $CMakeExe -CommandName 'cmake'
        $solution = @(Get-ChildItem -LiteralPath $BuildDir -Filter '*.sln' -File -ErrorAction SilentlyContinue | Select-Object -First 1)

        if ($Configure -or -not $solution) {
            $args = @(
                '-S', $root,
                '-B', $BuildDir,
                '-G', $VisualStudioGenerator,
                '-A', $Architecture,
                '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
            )
            if (-not [string]::IsNullOrWhiteSpace($QtRoot)) {
                $args += ('-DCMAKE_PREFIX_PATH={0}' -f $QtRoot)
            }

            Invoke-LoggedCommand -FilePath $cmake -ArgumentList $args -WorkingDirectory $root -LogStem (Join-Path $logDir ($timestamp + '-configure')) -TimeoutSeconds $TimeoutSeconds -TailLines $TailLines
        }

        if ($ConfigureOnly) {
            break
        }

        if ($Clean) {
            $cleanArgs = @('--build', $BuildDir, '--config', $Configuration, '--target', 'clean', '--verbose')
            Invoke-LoggedCommand -FilePath $cmake -ArgumentList $cleanArgs -WorkingDirectory $root -LogStem (Join-Path $logDir ($timestamp + '-clean')) -TimeoutSeconds $TimeoutSeconds -TailLines $TailLines
        }

        $buildArgs = @('--build', $BuildDir, '--config', $Configuration, '--verbose')
        if (-not [string]::IsNullOrWhiteSpace($Target)) {
            $buildArgs += @('--target', $Target)
        }
        $buildArgs += if ($Parallel -gt 0) { @('--parallel', [string]$Parallel) } else { @('--parallel') }
        Invoke-LoggedCommand -FilePath $cmake -ArgumentList $buildArgs -WorkingDirectory $root -LogStem (Join-Path $logDir ($timestamp + '-build')) -TimeoutSeconds $TimeoutSeconds -TailLines $TailLines
    }

    'QMake' {
        $qmake = Resolve-CommandPath -ExplicitPath $QMakeExe -CommandName 'qmake'
        $makeTool = Get-QMakeBuildTool
        $proFile = $proFiles[0].FullName
        $makefile = Join-Path $BuildDir 'Makefile'

        if ($Configure -or -not (Test-Path -LiteralPath $makefile)) {
            $configArg = if ($Configuration -eq 'Debug') { 'CONFIG+=debug' } else { 'CONFIG+=release' }
            Invoke-LoggedCommand -FilePath $qmake -ArgumentList @($proFile, $configArg) -WorkingDirectory $BuildDir -LogStem (Join-Path $logDir ($timestamp + '-configure')) -TimeoutSeconds $TimeoutSeconds -TailLines $TailLines
        }

        if ($ConfigureOnly) {
            break
        }

        if ($Clean) {
            Invoke-LoggedCommand -FilePath $makeTool -ArgumentList @('clean') -WorkingDirectory $BuildDir -LogStem (Join-Path $logDir ($timestamp + '-clean')) -TimeoutSeconds $TimeoutSeconds -TailLines $TailLines
        }

        $makeArgs = @()
        if ($script:IsWindowsHost -and (Split-Path -Leaf $makeTool) -eq 'nmake.exe') {
            if (-not [string]::IsNullOrWhiteSpace($Target)) {
                $makeArgs += $Target
            }
        }
        else {
            if ($Parallel -gt 0) {
                $makeArgs += ('-j' + $Parallel)
            }
            if (-not [string]::IsNullOrWhiteSpace($Target)) {
                $makeArgs += $Target
            }
        }

        Invoke-LoggedCommand -FilePath $makeTool -ArgumentList $makeArgs -WorkingDirectory $BuildDir -LogStem (Join-Path $logDir ($timestamp + '-build')) -TimeoutSeconds $TimeoutSeconds -TailLines $TailLines
    }
}

Write-Info "Done. BuildSystem=$BuildSystem BuildDir=$BuildDir"
