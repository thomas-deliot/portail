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

function Invoke-CheckedCmd {
    param([string]$Command)

    & cmd.exe /d /s /c $Command
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Command"
    }
}

function Resolve-RequiredCommandPath {
    param([string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        $path = if ($command.Source) { $command.Source } else { $command.Path }
        if ($path -and (Test-Path -LiteralPath $path)) {
            return (Resolve-Path -LiteralPath $path).Path
        }
    }

    $vsCmakeRoot = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake"
    $candidates = switch ($Name) {
        "cmake" { @("$vsCmakeRoot\CMake\bin\cmake.exe", "$env:ProgramFiles\CMake\bin\cmake.exe") }
        "ninja" { @("$vsCmakeRoot\Ninja\ninja.exe", "$env:ProgramFiles\Ninja\ninja.exe") }
        default { @() }
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "Could not find '$Name'. Make sure it is installed or update this script with the correct path."
}

try {
    $sourceDir = $PSScriptRoot
    $buildPath = Join-Path $sourceDir $BuildDir
    $vcvars = Find-VcVars64
    $cmakeExe = Resolve-RequiredCommandPath "cmake"
    $ninjaExe = Resolve-RequiredCommandPath "ninja"
    $cleanArg = if ($NoClean) { "" } else { "--clean-first " }

    Write-Host "Using Visual Studio environment: $vcvars"
    Write-Host "Using CMake: $cmakeExe"
    Write-Host "Using Ninja: $ninjaExe"
    Write-Host "Configuring and building Portail Stream plugin..."

    # Use absolute paths because this command runs inside cmd.exe after vcvars64.bat.
    # PowerShell may find cmake/ninja through PATH or app shims while cmd.exe may not.
    $buildCommand = "call `"$vcvars`" && `"$cmakeExe`" -S `"$sourceDir`" -B `"$buildPath`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninjaExe`" && `"$cmakeExe`" --build `"$buildPath`" $cleanArg-j $Jobs"
    Invoke-CheckedCmd $buildCommand

    $expectedDlls = @(
        "..\..\Stream\Plugins\portail_stream_host_plugin.dll",
        "..\..\Stream\Plugins\portail_stream_client_plugin.dll",
        "..\..\Stream\Plugins\portail_stream_common.dll",
        "..\..\Stream\Plugins\avcodec-62.dll",
        "..\..\Stream\Plugins\avdevice-62.dll",
        "..\..\Stream\Plugins\avfilter-11.dll",
        "..\..\Stream\Plugins\avformat-62.dll",
        "..\..\Stream\Plugins\avutil-60.dll",
        "..\..\Stream\Plugins\swresample-6.dll",
        "..\..\Stream\Plugins\swscale-9.dll"
    )

    Write-Host "Copied Unity DLLs:"
    foreach ($relativePath in $expectedDlls) {
        $path = Resolve-Path -LiteralPath (Join-Path $sourceDir $relativePath)
        Write-Host "  $path"
    }

    $optionalSteamWebRtc = Join-Path $sourceDir "..\..\Stream\Plugins\steamwebrtc64.dll"
    if (Test-Path -LiteralPath $optionalSteamWebRtc) {
        $path = Resolve-Path -LiteralPath $optionalSteamWebRtc
        Write-Host "  $path"
    }

    Write-Host "Done."

}
catch {
    Write-Host ""
    Write-Host "BuildAndCopyDlls.ps1 failed." -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red

    if ($_.ScriptStackTrace) {
        Write-Host ""
        Write-Host "Script stack trace:" -ForegroundColor DarkGray
        Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray
    }

    Write-Host ""
    Read-Host "Press Enter to exit"
    exit 1
}
