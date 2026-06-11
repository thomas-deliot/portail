param(
    [string]$BuildDir = "build_refactor",
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$NoClean
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Find-VcVars64 {
    $candidates = New-Object System.Collections.Generic.List[string]
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"

    if (Test-Path -LiteralPath $vswhere) {
        $installPaths = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        foreach ($installPath in $installPaths) {
            if ($installPath) {
                $candidates.Add((Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"))
            }
        }
    }

    foreach ($version in @("18", "17")) {
        foreach ($edition in @("Community", "Professional", "Enterprise", "BuildTools")) {
            $candidates.Add("C:\Program Files\Microsoft Visual Studio\$version\$edition\VC\Auxiliary\Build\vcvars64.bat")
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "Could not find vcvars64.bat. Install Visual Studio C++ build tools or update this script with the correct path."
}

function Resolve-RequiredCommandPath {
    param([Parameter(Mandatory = $true)][string]$CommandName)

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($command) {
        $path = if ($command.Source) { $command.Source } else { $command.Path }
        if ($path -and (Test-Path -LiteralPath $path)) {
            return (Resolve-Path -LiteralPath $path).Path
        }
    }

    $vsCmakeRoot = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake"
    $candidates = switch ($CommandName) {
        "cmake" { @("$vsCmakeRoot\CMake\bin\cmake.exe", "$env:ProgramFiles\CMake\bin\cmake.exe") }
        "ninja" { @("$vsCmakeRoot\Ninja\ninja.exe", "$env:ProgramFiles\Ninja\ninja.exe") }
        default { @() }
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "Could not find '$CommandName'. Make sure it is installed or update this script with the correct path."
}

function Invoke-CheckedCmd {
    param([string]$Command)

    & cmd.exe /d /s /c $Command
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Command"
    }
}

try {
    $sourceDir = $PSScriptRoot
    $buildPath = Join-Path $sourceDir $BuildDir
    $vcvars = Find-VcVars64

    # Resolve these in PowerShell first, then pass absolute paths into cmd.exe.
    # This avoids PATH differences between PowerShell, cmd.exe, and vcvars64.bat.
    $cmakeExe = Resolve-RequiredCommandPath "cmake"
    $ninjaExe = Resolve-RequiredCommandPath "ninja"

    $cleanArg = if ($NoClean) { "" } else { "--clean-first " }

    Write-Host "Using Visual Studio environment: $vcvars"
    Write-Host "Using CMake: $cmakeExe"
    Write-Host "Using Ninja: $ninjaExe"
    Write-Host "Configuring and building Portail Capture plugin..."

    $buildCommand = "call `"$vcvars`" && `"$cmakeExe`" -S `"$sourceDir`" -B `"$buildPath`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninjaExe`" && `"$cmakeExe`" --build `"$buildPath`" $cleanArg-j $Jobs"
    Invoke-CheckedCmd $buildCommand

    $expectedDll = Resolve-Path -LiteralPath (Join-Path $sourceDir "..\..\Capture\Plugins\portail_capture_plugin.dll")
    Write-Host "Copied Unity DLL:"
    Write-Host "  $expectedDll"
    Write-Host "Done."
}
catch {
    Write-Host ""
    Write-Host "ERROR:" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red

    if ($_.ScriptStackTrace) {
        Write-Host ""
        Write-Host "Stack trace:" -ForegroundColor DarkGray
        Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray
    }

    Write-Host ""
    Read-Host "Press Enter to close"
    exit 1
}
