[CmdletBinding()]
param(
    [ValidateSet("all", "x64", "x86")]
    [string]$Architecture = "x64",

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$sourceRoot = $PSScriptRoot
$solutionPath = Join-Path $sourceRoot "AudioBridge.sln"
$appProjectPath = Join-Path $sourceRoot "src\AudioBridge\AudioBridge.csproj"

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" |
            Select-Object -Last 1
        if ($found) {
            return $found
        }
    }

    throw "MSBuild was not found. Install Visual Studio with the Desktop development with C++ workload."
}

function Find-PlatformToolset {
    param([string]$MSBuildPath)

    $msbuildDirectory = Split-Path -Parent $MSBuildPath
    $visualStudioRoot = [IO.Path]::GetFullPath((Join-Path $msbuildDirectory "..\..\.."))
    $candidates = @(
        @{ Name = "v145"; Paths = @(
            "MSBuild\Microsoft\VC\v180\Platforms\x64\PlatformToolsets\v145\Toolset.props",
            "MSBuild\Microsoft\VC\v180\Platforms\Win32\PlatformToolsets\v145\Toolset.props"
        ) },
        @{ Name = "v143"; Paths = @(
            "MSBuild\Microsoft\VC\v170\Platforms\x64\PlatformToolsets\v143\Toolset.props",
            "MSBuild\Microsoft\VC\v170\Platforms\Win32\PlatformToolsets\v143\Toolset.props"
        ) }
    )

    foreach ($candidate in $candidates) {
        if ($candidate.Paths | Where-Object { Test-Path -LiteralPath (Join-Path $visualStudioRoot $_) }) {
            return $candidate.Name
        }
    }

    throw "A supported Visual C++ platform toolset (v145 or v143) was not found."
}

function Invoke-Checked {
    param(
        [string]$Description,
        [scriptblock]$Command
    )

    Write-Host "[$Description]"
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

function Build-Architecture {
    param(
        [string]$Platform,
        [string]$RuntimeIdentifier
    )

    Invoke-Checked "restore $Platform" {
        & dotnet restore $appProjectPath "-p:Platform=$Platform"
    }

    Invoke-Checked "build $Configuration $Platform" {
        & $script:msbuildPath $solutionPath "/m" "/t:Build" "/p:Configuration=$Configuration" "/p:Platform=$Platform" "/p:RuntimeIdentifier=$RuntimeIdentifier" "/p:PlatformToolset=$script:platformToolset" "/verbosity:minimal"
    }
}

if (-not (Test-Path -LiteralPath $solutionPath) -or -not (Test-Path -LiteralPath $appProjectPath)) {
    throw "Run this script from an intact AudioBridge source checkout."
}

$script:msbuildPath = Find-MSBuild
$script:platformToolset = Find-PlatformToolset $script:msbuildPath

Write-Host "[source] $sourceRoot"
Write-Host "[msbuild] $script:msbuildPath"
Write-Host "[toolset] $script:platformToolset"

if ($Architecture -in @("all", "x64")) {
    Build-Architecture "x64" "win-x64"
}
if ($Architecture -in @("all", "x86")) {
    Build-Architecture "x86" "win-x86"
}

Write-Host "[done] AudioBridge build completed."
